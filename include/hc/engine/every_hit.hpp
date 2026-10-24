// =============================================================================
//  engine/every_hit.hpp — HEM EVERY HIT (tambourine / hi-hat / "sss" tamer that cuts every hit).
//
//  hc.everyhit  The De-esser only cuts hits that go over a fixed THRESHOLD, so quiet hits in quiet
//               parts are missed. EVERY HIT listens to the band above FREQUENCY and compares it with
//               its own running floor (the level between hits). The floor FALLS fast (~150 ms) and
//               RISES slowly (~1.5 s), so a hit stands out whether the song is loud or quiet.
//               When the band jumps SENSITIVITY dB or more over the floor it is a hit, and a high
//               shelf at FREQUENCY is pulled down by AMOUNT dB — the same amount for every hit —
//               with a fast attack (0.5 ms) and a RELEASE ms release (the cut ramps straight back
//               to exactly 0 dB in RELEASE ms, so it really is off between hits). Between hits the audio passes
//               untouched (bit-exact). LISTEN = hear only what is being removed. No latency.
//               A hit is a short thing: if the band stays up for more than 500 ms (a sustained pad or
//               a steady tone coming in) the cut lets go and the floor is left to catch up. A hit ends
//               when the band falls 12 dB under the hit's own peak, so a loud hit and a quiet hit are
//               held for the same part of their decay; RELEASE then ramps the cut back to 0.
//
//  Meters (lock-free, held between reads like the De-esser): takeHitCount() = hits since the UI last
//  asked; takeCutPeakDb() = biggest cut; takeLevelPeakDb() = loudest band level (works while OFF).
//
//  prepare() does not allocate at all; process() never does (RT-safe).
// =============================================================================
#pragma once
#include "hc/dsp/biquad.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/engine/processor.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace hc {

class EveryHitProcessor final : public IProcessor {
public:
    enum : std::size_t { kFreq, kAmount, kSens, kRelease, kListen, kNumParams };
    static constexpr std::uint32_t kSub = 16;              // the shelf's gain is updated every 16 samples
    static constexpr float kAttackMs = 0.5f;               // cut attack
    static constexpr float kEnvAttackMs = 0.3f;            // detector peak follower attack
    static constexpr float kEnvReleaseMs = 20.0f;          // detector peak follower release
    static constexpr float kFloorFallMs = 150.0f;          // the floor follows the level DOWN this fast
    static constexpr float kFloorRiseMs = 1500.0f;         // ...and UP this slowly (a hit must not lift it)
    static constexpr float kMaxHitMs = 500.0f;             // a "hit" longer than this is a sustained sound: let go
    static constexpr float kMinDb = -90.0f;                // silence reads as this, not -600
    static constexpr float kHysteresisDb = 3.0f;           // a hit ends when the band falls this far back under SENSITIVITY...
    static constexpr float kHitEndDb = 12.0f;              // ...or this far under the hit's own peak (then RELEASE takes over)

    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t maxBlock) override {
        (void)maxBlock;
        sr_ = sr;
        lastHz_ = -1.0f;
        updateDetector();
        envAtt_ = coef(kEnvAttackMs);
        envRel_ = coef(kEnvReleaseMs);
        floorFall_ = 1.0f - coef(kFloorFallMs);
        floorRise_ = 1.0f - coef(kFloorRiseMs);
        attN_ = std::max(1.0f, static_cast<float>(sr * kAttackMs * 0.001));
        maxHitN_ = static_cast<std::uint32_t>(sr * kMaxHitMs * 0.001);
        lastRelMs_ = -1.0f;
        reset();
    }
    void reset() noexcept override {
        for (auto& ch : det_) for (auto& f : ch) f.reset();
        for (auto& f : shelf_) f.reset();
        env_ = 0.0f; floorDb_ = kMinDb; cutDb_ = 0.0f; lastCut_ = 0.0f; hit_ = false; hitN_ = 0; hitPeakDb_ = kMinDb; troughDb_ = kMinDb;
    }
    void process(AudioBlock& b) noexcept override {
        updateDetector();
        const std::uint32_t nch = std::min<std::uint32_t>(b.numChannels, 2);
        const float sens = p_[kSens].target(), amount = p_[kAmount].target(), hz = p_[kFreq].target();
        const bool listen = p_[kListen].target() > 0.5f;
        const float relMs = p_[kRelease].target();
        if (relMs != lastRelMs_) { relN_ = std::max(1.0f, static_cast<float>(sr_ * relMs * 0.001)); lastRelMs_ = relMs; }
        const float attStep = amount / attN_, relStep = amount / relN_;   // dB per sample
        const bool off = bypassed();
        float peakEnv = 0.0f, peakCut = 0.0f;
        std::uint32_t hits = 0;
        for (std::uint32_t o = 0; o < b.numFrames; o += kSub) {
            const std::uint32_t n = std::min<std::uint32_t>(kSub, b.numFrames - o);
            // Detector: peak follower on the band above FREQUENCY (a copy; the audio itself is never split).
            for (std::uint32_t i = 0; i < n; ++i) {
                float a = 0.0f;
                for (std::uint32_t c = 0; c < nch; ++c) {
                    double x = b.ch[c][o + i];
                    for (auto& f : det_[c]) x = f.tick(x);
                    a = std::max(a, static_cast<float>(std::fabs(x)));
                }
                env_ = a > env_ ? a + (env_ - a) * envAtt_ : a + (env_ - a) * envRel_;
                peakEnv = std::max(peakEnv, env_);
                const float envDb = std::max(kMinDb, simd::gain_to_db(env_));
                // Running floor: the level between hits. Falls fast, rises slowly.
                floorDb_ += (envDb - floorDb_) * (envDb < floorDb_ ? floorFall_ : floorRise_);
                const float over = envDb - floorDb_;
                // A hit starts on a fresh rise (SENSITIVITY over the floor AND 3 dB over the lowest level since the
                // last hit ended) and ends when the band falls 12 dB under its peak or back near the floor.
                bool up;
                if (hit_) {
                    up = over >= sens - kHysteresisDb && envDb > hitPeakDb_ - kHitEndDb;
                    hitPeakDb_ = std::max(hitPeakDb_, envDb);
                    if (!up) troughDb_ = envDb;
                } else {
                    troughDb_ = std::min(troughDb_, envDb);
                    up = over >= sens && envDb >= troughDb_ + kHysteresisDb;
                    if (up) { ++hits; hitPeakDb_ = envDb; }
                }
                hit_ = up;
                hitN_ = up ? hitN_ + 1 : 0;
                const bool hitNow = up && hitN_ <= maxHitN_;   // longer than kMaxHitMs = a sustained sound, not a hit
                // The cut: ramps to AMOUNT dB in 0.5 ms on a hit, and straight back to 0 in RELEASE ms after it.
                const float want = (hitNow && !off) ? amount : 0.0f;
                if (cutDb_ < want) cutDb_ = std::min(want, cutDb_ + attStep);
                else if (cutDb_ > want) cutDb_ = std::max(want, cutDb_ - relStep);
                peakCut = std::max(peakCut, cutDb_);
            }
            if (off) continue;   // OFF: the audio is not touched (the meters above still work)
            const float cut = cutDb_;
            if (cut <= 0.0f && lastCut_ <= 0.0f && !listen) {   // between hits: leave the audio exactly as it is
                for (std::uint32_t c = 0; c < nch; ++c) for (std::uint32_t i = 0; i < n; ++i) shelf_[c].tick(b.ch[c][o + i]);
                lastCut_ = 0.0f;
                continue;
            }
            // Dynamic high shelf: −cut dB above FREQUENCY. Gain 0 dB is an exact pass-through.
            if (cut != lastCut_) {
                const BiquadCoeffs k = designBiquad(FilterType::HighShelf, sr_, hz, 0.707, -cut);
                for (auto& f : shelf_) f.setCoeffs(k);
                lastCut_ = cut;
            }
            for (std::uint32_t c = 0; c < nch; ++c) {
                float* x = b.ch[c] + o;
                if (listen) std::copy(x, x + n, dry_[c].data());
                shelf_[c].process(x, n);
                if (listen) for (std::uint32_t i = 0; i < n; ++i) x[i] = dry_[c][i] - x[i];   // only what's removed
            }
        }
        if (off) lastCut_ = -1.0f;   // switching back on redesigns the shelf from where the cut is
        publish(peakEnv, peakCut, hits);
    }
    // Meters for the UI; the audio thread keeps the value since the UI last asked, so nothing is missed between frames.
    float takeLevelPeakDb() noexcept { return simd::gain_to_db(std::max(lvlPeak_.exchange(0.0f, std::memory_order_acq_rel), 1.0e-7f)); }
    float takeCutPeakDb() noexcept { return cutPeak_.exchange(0.0f, std::memory_order_acq_rel); }
    std::uint32_t takeHitCount() noexcept { return hitCount_.exchange(0u, std::memory_order_acq_rel); }
    float currentCutDb() const noexcept { return -grDb_.load(std::memory_order_relaxed); }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    float meterValue() const noexcept override { return grDb_.load(std::memory_order_relaxed); }

private:
    float coef(float ms) const noexcept { return static_cast<float>(std::exp(-1.0 / (sr_ * std::max(0.01f, ms) * 0.001))); }
    void updateDetector() noexcept {
        const float hz = p_[kFreq].target();
        if (hz == lastHz_) return;
        lastHz_ = hz;
        const BiquadCoeffs c = designBiquad(FilterType::HighPass, sr_, hz, 0.707, 0.0);
        for (auto& ch : det_) for (auto& f : ch) f.setCoeffs(c);
        lastCut_ = -1.0f;   // force the shelf to be redesigned at the new frequency
    }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"freq", "Frequency", "Hz", 2000, 16000, 7000, Taper::Log}, {"amount", "Amount", "dB", 0, 24, 8},
        {"sens", "Sensitivity", "dB", 3, 18, 9},                   {"release", "Release", "ms", 20, 500, 80, Taper::Log},
        {"listen", "Listen", "", 0, 1, 0}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kFreq, 0.14f, 0.6f, 0.14f}, {ControlKind::Knob, kAmount, 0.36f, 0.6f, 0.14f},
        {ControlKind::Knob, kSens, 0.57f, 0.6f, 0.12f}, {ControlKind::Knob, kRelease, 0.76f, 0.6f, 0.12f},
        {ControlKind::Switch, kListen, 0.93f, 0.6f, 0.05f}};
    static inline const ProcessorInfo kInfo{"hc.everyhit", "HEM Every Hit", "Repair", 1, kParams, {2.6f, 0x3EB8FFFF, kFace}};

    double sr_ = 48000.0;
    std::array<AtomicParam, kNumParams> p_{AtomicParam(7000, 2000, 16000), AtomicParam(8, 0, 24), AtomicParam(9, 3, 18),
                                           AtomicParam(80, 20, 500), AtomicParam(0, 0, 1)};
    std::array<float, kSub> dry_[2]{};
    Biquad det_[2][2];   // detector: two cascaded high-passes per channel (24 dB/oct)
    Biquad shelf_[2];    // the audio path: one high shelf per channel
    float lastHz_ = -1.0f, lastRelMs_ = -1.0f;
    float env_ = 0.0f, floorDb_ = kMinDb, hitPeakDb_ = kMinDb, troughDb_ = kMinDb, cutDb_ = 0.0f, lastCut_ = 0.0f;
    float envAtt_ = 0.0f, envRel_ = 0.0f, floorFall_ = 0.0f, floorRise_ = 0.0f, attN_ = 24.0f, relN_ = 3840.0f;
    std::uint32_t hitN_ = 0, maxHitN_ = 24000;
    bool hit_ = false;
    std::atomic<float> grDb_{0.0f};
    std::atomic<float> lvlPeak_{0.0f}, cutPeak_{0.0f};
    std::atomic<std::uint32_t> hitCount_{0};
    void publish(float env, float cut, std::uint32_t hits) noexcept {
        grDb_.store(-cut, std::memory_order_relaxed);
        float o = lvlPeak_.load(std::memory_order_relaxed);
        while (env > o && !lvlPeak_.compare_exchange_weak(o, env, std::memory_order_acq_rel)) {}
        o = cutPeak_.load(std::memory_order_relaxed);
        while (cut > o && !cutPeak_.compare_exchange_weak(o, cut, std::memory_order_acq_rel)) {}
        if (hits) hitCount_.fetch_add(hits, std::memory_order_acq_rel);
    }
};

} // namespace hc
