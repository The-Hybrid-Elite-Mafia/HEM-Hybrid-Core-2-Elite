// =============================================================================
//  engine/stem_tools.hpp — HEM Gate and HEM De-esser (per-stem clean-up tools).
//
//  hc.gate   Noise gate / expander. The detector listens through a key high-pass (so a kick or bass
//            bleed doesn't hold the gate open on a snare or hat stem), opens above Threshold, stays
//            open for Hold, and closes with hysteresis (4 dB below Threshold) so it doesn't chatter.
//            Closed = Range dB down (−80 dB ≈ silence, −10 dB = gentle). Attack / Release smooth the
//            gain. No latency.
//
//  hc.deess  De-esser / hat tamer. A detector listens to everything above Frequency; when that
//            band's peak level goes over Threshold, a high shelf at Frequency is pulled down
//            (4:1, at most Max Cut dB, updated every 16 samples). With nothing over Threshold the
//            audio passes untouched (bit-exact). Listen = hear only what is being removed. No latency.
//
//  Both: prepare() allocates; process() never does (RT-safe).
// =============================================================================
#pragma once
#include "hc/dsp/biquad.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/engine/processor.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace hc {

// ----------------------------------------------------------------------------- Gate
class GateProcessor final : public IProcessor {
public:
    enum : std::size_t { kThresh, kRange, kAttack, kHold, kRelease, kKeyHpf, kNumParams };
    static constexpr float kHysteresisDb = 4.0f;

    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t maxBlock) override {
        sr_ = sr;
        key_.assign(std::max<std::uint32_t>(maxBlock, 1), 0.0f);
        lastKeyHz_ = -1.0f;
        updateKey();
        reset();
    }
    void reset() noexcept override {
        for (auto& f : keyF_) f.reset();
        env_ = 0.0f; gain_ = 1.0f; holdLeft_ = 0; open_ = false;
    }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) {   // off: pass the audio untouched, but keep the level bar alive so Threshold can be set first
            float pk = 0.0f;
            for (std::uint32_t c = 0; c < b.numChannels; ++c)
                for (std::uint32_t i = 0; i < b.numFrames; ++i) pk = std::max(pk, std::fabs(b.ch[c][i]));
            const float decay = std::pow(coef(10.0f), static_cast<float>(b.numFrames));
            env_ = std::max(pk, env_ * decay);
            levelDb_.store(simd::gain_to_db(env_), std::memory_order_relaxed);
            openFlag_.store(true, std::memory_order_relaxed); grDb_.store(0.0f, std::memory_order_relaxed);
            return;
        }
        updateKey();
        const float thrDb = p_[kThresh].target();
        const float closeDb = thrDb - kHysteresisDb;
        const float rangeGain = simd::db_to_gain(p_[kRange].target());
        const float att = coef(p_[kAttack].target()), rel = coef(p_[kRelease].target());
        const std::uint32_t holdN = static_cast<std::uint32_t>(p_[kHold].target() * 0.001 * sr_);
        const float envRel = coef(10.0f);   // detector release (peak follower)
        float* L = b.ch[0];
        float* R = b.numChannels > 1 ? b.ch[1] : b.ch[0];
        std::uint32_t done = 0;
        while (done < b.numFrames) {
            const std::uint32_t n = std::min<std::uint32_t>(b.numFrames - done, static_cast<std::uint32_t>(key_.size()));
            float* k = key_.data();
            for (std::uint32_t i = 0; i < n; ++i) k[i] = 0.5f * (L[done + i] + R[done + i]);
            for (auto& f : keyF_) f.process(k, n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const float a = std::fabs(k[i]);
                env_ = a > env_ ? a : env_ * envRel + a * (1.0f - envRel);
                const float eDb = simd::gain_to_db(env_);
                if (eDb > thrDb) { open_ = true; holdLeft_ = holdN; }
                else if (open_ && eDb < closeDb) {
                    if (holdLeft_ > 0) --holdLeft_;
                    else open_ = false;
                }
                const float want = open_ ? 1.0f : rangeGain;
                const float c = want > gain_ ? att : rel;
                gain_ = want + (gain_ - want) * c;
                L[done + i] *= gain_;
                if (R != L) R[done + i] *= gain_;
            }
            done += n;
        }
        openFlag_.store(open_, std::memory_order_relaxed);
        grDb_.store(simd::gain_to_db(gain_), std::memory_order_relaxed);
        levelDb_.store(simd::gain_to_db(env_), std::memory_order_relaxed);
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    float meterValue() const noexcept override { return grDb_.load(std::memory_order_relaxed); }
    bool isOpen() const noexcept { return openFlag_.load(std::memory_order_relaxed); }
    float keyLevelDb() const noexcept { return levelDb_.load(std::memory_order_relaxed); }

private:
    float coef(float ms) const noexcept { return static_cast<float>(std::exp(-1.0 / (sr_ * std::max(0.05f, ms) * 0.001))); }
    void updateKey() noexcept {
        const float hz = p_[kKeyHpf].target();
        if (hz == lastKeyHz_) return;
        lastKeyHz_ = hz;
        const BiquadCoeffs c = designBiquad(FilterType::HighPass, sr_, hz, 0.707, 0.0);
        for (auto& f : keyF_) f.setCoeffs(c);
    }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"thresh", "Threshold", "dB", -80, 0, -40, Taper::Decibel}, {"range", "Range", "dB", -80, 0, -60, Taper::Decibel},
        {"attack", "Attack", "ms", 0.05f, 50, 0.5f, Taper::Log},    {"hold", "Hold", "ms", 0, 500, 40},
        {"release", "Release", "ms", 5, 2000, 120, Taper::Log},    {"keyhpf", "Key HPF", "Hz", 20, 2000, 20, Taper::Log}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kThresh, 0.1f, 0.6f, 0.12f}, {ControlKind::Knob, kRange, 0.27f, 0.6f, 0.12f},
        {ControlKind::Knob, kAttack, 0.44f, 0.6f, 0.1f}, {ControlKind::Knob, kHold, 0.6f, 0.6f, 0.1f},
        {ControlKind::Knob, kRelease, 0.76f, 0.6f, 0.1f}, {ControlKind::Knob, kKeyHpf, 0.92f, 0.6f, 0.1f}};
    static inline const ProcessorInfo kInfo{"hc.gate", "HEM Gate", "Dynamics", 1, kParams, {2.6f, 0x3EB8FFFF, kFace}};

    double sr_ = 48000.0;
    std::array<AtomicParam, kNumParams> p_{AtomicParam(-40, -80, 0), AtomicParam(-60, -80, 0), AtomicParam(0.5f, 0.05f, 50),
                                           AtomicParam(40, 0, 500), AtomicParam(120, 5, 2000), AtomicParam(20, 20, 2000)};
    std::vector<float> key_;
    Biquad keyF_[2];   // 12 dB/oct x2 = 24 dB/oct key high-pass
    float lastKeyHz_ = -1.0f, env_ = 0.0f, gain_ = 1.0f;
    std::uint32_t holdLeft_ = 0;
    bool open_ = false;
    std::atomic<bool> openFlag_{true};
    std::atomic<float> grDb_{0.0f}, levelDb_{-120.0f};
};

// ----------------------------------------------------------------------------- De-esser / hat tamer
class DeEsserProcessor final : public IProcessor {
public:
    enum : std::size_t { kFreq, kThresh, kMaxCut, kListen, kNumParams };
    static constexpr float kRatio = 4.0f;
    static constexpr std::uint32_t kSub = 16;   // the shelf's gain is updated every 16 samples

    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t maxBlock) override {
        sr_ = sr;
        for (auto& d : dry_) d.assign(kSub, 0.0f);
        (void)maxBlock;
        lastHz_ = -1.0f;
        updateDetector();
        att_ = static_cast<float>(std::exp(-1.0 / (sr * 0.0005)));   // 0.5 ms
        rel_ = static_cast<float>(std::exp(-1.0 / (sr * 0.060)));    // 60 ms
        reset();
    }
    void reset() noexcept override {
        for (auto& ch : det_) for (auto& f : ch) f.reset();
        for (auto& f : shelf_) f.reset();
        env_ = 0.0f; lastCut_ = 0.0f;
    }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) { grDb_.store(0.0f, std::memory_order_relaxed); return; }
        updateDetector();
        const float thr = p_[kThresh].target(), maxCut = p_[kMaxCut].target(), hz = p_[kFreq].target();
        const bool listen = p_[kListen].target() > 0.5f;
        const std::uint32_t nch = std::min<std::uint32_t>(b.numChannels, 2);
        float peakGr = 0.0f;
        for (std::uint32_t o = 0; o < b.numFrames; o += kSub) {
            const std::uint32_t n = std::min<std::uint32_t>(kSub, b.numFrames - o);
            // Detector: peak follower on the high band (a copy; the audio itself is never split).
            float cut = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                float a = 0.0f;
                for (std::uint32_t c = 0; c < nch; ++c) {
                    double x = b.ch[c][o + i];
                    for (auto& f : det_[c]) x = f.tick(x);
                    a = std::max(a, static_cast<float>(std::fabs(x)));
                }
                env_ = a > env_ ? a + (env_ - a) * att_ : a + (env_ - a) * rel_;
                const float over = simd::gain_to_db(env_) - thr;
                cut = std::max(cut, over > 0.0f ? std::min(maxCut, over * (1.0f - 1.0f / kRatio)) : 0.0f);
            }
            peakGr = std::max(peakGr, cut);
            if (cut <= 0.0f && lastCut_ <= 0.0f && !listen) {   // nothing to do: leave the audio exactly as it is
                for (std::uint32_t c = 0; c < nch; ++c) for (std::uint32_t i = 0; i < n; ++i) shelf_[c].tick(b.ch[c][o + i]);
                lastCut_ = 0.0f;
                continue;
            }
            // Dynamic high shelf: −cut dB above Frequency. Gain 0 dB is an exact pass-through.
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
        grDb_.store(-peakGr, std::memory_order_relaxed);
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    float meterValue() const noexcept override { return grDb_.load(std::memory_order_relaxed); }

private:
    void updateDetector() noexcept {
        const float hz = p_[kFreq].target();
        if (hz == lastHz_) return;
        lastHz_ = hz;
        const BiquadCoeffs c = designBiquad(FilterType::HighPass, sr_, hz, 0.707, 0.0);
        for (auto& ch : det_) for (auto& f : ch) f.setCoeffs(c);
        lastCut_ = -1.0f;   // force the shelf to be redesigned at the new frequency
    }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"freq", "Frequency", "Hz", 2000, 16000, 6500, Taper::Log}, {"thresh", "Threshold", "dB", -60, 0, -24, Taper::Decibel},
        {"maxcut", "Max Cut", "dB", 0, 24, 12},                      {"listen", "Listen", "", 0, 1, 0}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kFreq, 0.2f, 0.6f, 0.14f}, {ControlKind::Knob, kThresh, 0.45f, 0.6f, 0.14f},
        {ControlKind::Knob, kMaxCut, 0.7f, 0.6f, 0.12f}, {ControlKind::Switch, kListen, 0.9f, 0.6f, 0.05f}};
    static inline const ProcessorInfo kInfo{"hc.deess", "HEM De-esser", "Repair", 1, kParams, {2.6f, 0x3EB8FFFF, kFace}};

    double sr_ = 48000.0;
    std::array<AtomicParam, kNumParams> p_{AtomicParam(6500, 2000, 16000), AtomicParam(-24, -60, 0), AtomicParam(12, 0, 24),
                                           AtomicParam(0, 0, 1)};
    std::vector<float> dry_[2];
    Biquad det_[2][2];   // detector: two cascaded high-passes per channel
    Biquad shelf_[2];    // the audio path: one high shelf per channel
    float lastHz_ = -1.0f, env_ = 0.0f, lastCut_ = 0.0f, att_ = 0.0f, rel_ = 0.0f;
    std::atomic<float> grDb_{0.0f};
};

} // namespace hc
