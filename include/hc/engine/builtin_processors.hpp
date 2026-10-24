// =============================================================================
//  engine/builtin_processors.hpp — the first native HEM plugins.
//    hc.eq6       6-band parametric (HPF, low shelf, 2 bells, high shelf, LPF)
//    hc.comp      stereo-linked soft-knee compressor
//    hc.artifact  HF Artifact Destroyer (STFT, see repair/artifact_engine.hpp)
//    hc.limiter   true-peak lookahead brick-wall limiter (master)
// =============================================================================
#pragma once
#include "hc/dsp/biquad.hpp"
#include "hc/dsp/dynamics.hpp"
#include "hc/dsp/stft.hpp"
#include "hc/engine/processor.hpp"
#include "hc/repair/artifact_engine.hpp"
#include "hc/repair/resonance.hpp"
#include "hc/dsp/transient.hpp"
#include <array>

namespace hc {

// ----------------------------------------------------------------------------- EQ
class Eq6 final : public IProcessor {
public:
    static constexpr std::size_t kBands = 6;
    // Param layout: band b → [3b]=freq, [3b+1]=gain dB, [3b+2]=Q ; [18]=output dB
    static constexpr std::size_t kNumParams = kBands * 3 + 1;

    Eq6() {
        const float f[kBands] = {30, 100, 400, 2500, 10000, 20000};
        for (std::size_t b = 0; b < kBands; ++b) {
            p_[3 * b].configure(f[b], 20.0f, 22000.0f);
            p_[3 * b + 1].configure(0.0f, -24.0f, 24.0f);
            p_[3 * b + 2].configure(0.707f, 0.1f, 18.0f);
        }
        p_[18].configure(0.0f, -24.0f, 24.0f);
    }
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override {
        sr_ = sr;
        for (auto& p : p_) p.prepare(sr, 30.0);
        last_.fill(-1e9f);
        updateCoeffs(true);
        reset();
    }
    void reset() noexcept override { for (auto& ch : f_) for (auto& s : ch) s.reset(); }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) return;
        for (auto& p : p_) p.advance(b.numFrames);
        updateCoeffs(false);
        for (std::uint32_t c = 0; c < b.numChannels; ++c)
            for (std::size_t s = 0; s < kBands; ++s)
                if (active_[s]) f_[c][s].process(b.ch[c], b.numFrames);
        const float g = simd::db_to_gain(p_[18].current());
        if (g != 1.0f) for (std::uint32_t c = 0; c < b.numChannels; ++c) simd::scale(b.ch[c], g, b.numFrames);
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }

private:
    static constexpr FilterType kTypes[kBands] = {FilterType::HighPass, FilterType::LowShelf, FilterType::Bell,
                                                  FilterType::Bell, FilterType::HighShelf, FilterType::LowPass};
    void updateCoeffs(bool force) noexcept {
        for (std::size_t s = 0; s < kBands; ++s) {
            const float fr = p_[3 * s].current(), gn = p_[3 * s + 1].current(), q = p_[3 * s + 2].current();
            if (!force && fr == last_[3 * s] && gn == last_[3 * s + 1] && q == last_[3 * s + 2]) continue;
            last_[3 * s] = fr; last_[3 * s + 1] = gn; last_[3 * s + 2] = q;
            const BiquadCoeffs c = designBiquad(kTypes[s], sr_, fr, q, gn);
            for (auto& ch : f_) ch[s].setCoeffs(c);
            // Bells/shelves at 0 dB are identity: skip them. HPF at ≤20 Hz and LPF
            // at ≥ 20 kHz are treated as "off".
            const bool passFilter = kTypes[s] == FilterType::HighPass || kTypes[s] == FilterType::LowPass;
            active_[s] = passFilter ? (kTypes[s] == FilterType::HighPass ? fr > 20.5f : fr < 19999.0f)
                                    : std::fabs(gn) > 0.01f;
        }
    }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"hpf_f", "HPF", "Hz", 20, 22000, 30, Taper::Log},     {"hpf_g", "-", "dB", -24, 24, 0},    {"hpf_q", "HPF Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"ls_f", "Low Shelf", "Hz", 20, 22000, 100, Taper::Log}, {"ls_g", "LS Gain", "dB", -24, 24, 0}, {"ls_q", "LS Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"b1_f", "Bell 1", "Hz", 20, 22000, 400, Taper::Log},  {"b1_g", "B1 Gain", "dB", -24, 24, 0}, {"b1_q", "B1 Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"b2_f", "Bell 2", "Hz", 20, 22000, 2500, Taper::Log}, {"b2_g", "B2 Gain", "dB", -24, 24, 0}, {"b2_q", "B2 Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"hs_f", "High Shelf", "Hz", 20, 22000, 10000, Taper::Log}, {"hs_g", "HS Gain", "dB", -24, 24, 0}, {"hs_q", "HS Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"lpf_f", "LPF", "Hz", 20, 22000, 20000, Taper::Log}, {"lpf_g", "-", "dB", -24, 24, 0},   {"lpf_q", "LPF Q", "", 0.1f, 18, 0.707f, Taper::Log},
        {"out", "Output", "dB", -24, 24, 0, Taper::Decibel}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::SpectrumView, -1, 0.5f, 0.30f, 0.9f},
        {ControlKind::Knob, 0, 0.08f, 0.75f, 0.10f}, {ControlKind::Knob, 3, 0.22f, 0.75f, 0.10f},
        {ControlKind::Knob, 4, 0.22f, 0.92f, 0.07f}, {ControlKind::Knob, 6, 0.38f, 0.75f, 0.10f},
        {ControlKind::Knob, 7, 0.38f, 0.92f, 0.07f}, {ControlKind::Knob, 9, 0.54f, 0.75f, 0.10f},
        {ControlKind::Knob, 10, 0.54f, 0.92f, 0.07f}, {ControlKind::Knob, 12, 0.70f, 0.75f, 0.10f},
        {ControlKind::Knob, 13, 0.70f, 0.92f, 0.07f}, {ControlKind::Knob, 15, 0.84f, 0.75f, 0.10f},
        {ControlKind::Fader, 18, 0.95f, 0.70f, 0.30f}};
    static inline const ProcessorInfo kInfo{"hc.eq6", "HEM EQ-6", "EQ", 1, kParams, {2.4f, 0xC0FF3EFF, kFace}};

    double sr_ = 48000.0;
    std::array<AtomicParam, kNumParams> p_;
    std::array<float, kNumParams> last_{};
    std::array<bool, kBands> active_{};
    Biquad f_[kNumChannels][kBands];
};

// ----------------------------------------------------------------------------- Compressor
class CompProcessor final : public IProcessor {
public:
    enum : std::size_t { kThresh, kRatio, kKnee, kAttack, kRelease, kMakeup, kNumParams };
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override { c_.prepare(sr); pushSettings(); }
    void reset() noexcept override { c_.reset(); }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) return;
        pushSettings();
        c_.process(b);
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    float meterValue() const noexcept override { return c_.gainReductionDb(); }

private:
    void pushSettings() noexcept {
        Compressor::Settings s{p_[kThresh].target(), p_[kRatio].target(), p_[kKnee].target(),
                               p_[kAttack].target(), p_[kRelease].target(), p_[kMakeup].target()};
        c_.setSettings(s);
    }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"thresh", "Threshold", "dB", -60, 0, -18, Taper::Decibel}, {"ratio", "Ratio", ":1", 1, 20, 3, Taper::Log},
        {"knee", "Knee", "dB", 0, 24, 6},                            {"attack", "Attack", "ms", 0.05f, 200, 10, Taper::Log},
        {"release", "Release", "ms", 5, 2000, 120, Taper::Log},     {"makeup", "Makeup", "dB", -12, 24, 0, Taper::Decibel}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kThresh, 0.12f, 0.5f, 0.16f}, {ControlKind::Knob, kRatio, 0.30f, 0.5f, 0.12f},
        {ControlKind::Knob, kKnee, 0.45f, 0.5f, 0.10f},   {ControlKind::Knob, kAttack, 0.60f, 0.5f, 0.10f},
        {ControlKind::Knob, kRelease, 0.75f, 0.5f, 0.10f}, {ControlKind::Knob, kMakeup, 0.90f, 0.5f, 0.12f},
        {ControlKind::GainReductionMeter, -1, 0.5f, 0.12f, 0.8f}};
    static inline const ProcessorInfo kInfo{"hc.comp", "HEM Comp", "Dynamics", 1, kParams, {3.0f, 0xC0FF3EFF, kFace}};
    std::array<AtomicParam, kNumParams> p_{AtomicParam(-18, -60, 0), AtomicParam(3, 1, 20), AtomicParam(6, 0, 24),
                                           AtomicParam(10, 0.05f, 200), AtomicParam(120, 5, 2000), AtomicParam(0, -12, 24)};
    Compressor c_;
};

// ----------------------------------------------------------------------------- Artifact Destroyer
class ArtifactProcessor final : public IProcessor {
public:
    enum : std::size_t { kShimmer, kShimTol, kSib, kSibThr, kPhase, kSplit, kFloor, kRes, kDecay, kNumParams };
    ArtifactProcessor() : kernel_(settings_, telemetry_) { settings_.sibilanceAmount.set(0.0f); }
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override {
        // ~21 ms frames / ~5.3 ms hop at any rate: HF repair needs time
        // resolution far more than frequency resolution (bins are 47 Hz wide
        // at 48 kHz — irrelevant above 7 kHz). Latency = 1024 @ 48 k.
        // RESOLUTION selector: 1024 / 2048 / 4096 at 44.1–48 kHz (doubled
        // above 60 kHz). Changing it rebuilds the chain off the audio thread.
        const int res = static_cast<int>(std::lround(settings_.resolution.target()));
        std::size_t n = std::size_t{1024} << std::clamp(res, 0, 2);
        if (sr > 60000.0) n *= 2;
        stft_.prepare(sr, n, 4);
        kernel_.prepare(sr, n, stft_.hop());
    }
    void reset() noexcept override { stft_.reset(); kernel_.reset(); }
    void process(AudioBlock& b) noexcept override {
        // Bypass keeps the STFT running with neutral settings so latency (and
        // therefore stem alignment) never changes when you A/B.
        if (bypassed()) { bypassKernel_.dummy = 0; stft_.process(b, bypassKernel_); return; }
        stft_.process(b, kernel_);
    }
    std::uint32_t latency() const noexcept override { return static_cast<std::uint32_t>(stft_.latency()); }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override {
        switch (i) {
        case kShimmer: return settings_.shimmerAmount;   case kShimTol: return settings_.shimmerTolDb;
        case kSib: return settings_.sibilanceAmount;     case kSibThr: return settings_.sibilanceThreshDb;
        case kPhase: return settings_.phaseAmount;       case kSplit: return settings_.splitHz;
        case kFloor: return settings_.floorDb;           case kRes: return settings_.resolution;
        default: return settings_.decayMs;
        }
    }
    float meterValue() const noexcept override { return telemetry_.shimmerCutDb.load(std::memory_order_relaxed); }
    const ArtifactTelemetry& telemetry() const noexcept { return telemetry_; }

private:
    struct PassKernel final : ISpectralKernel { int dummy = 0; void processFrame(SpectralFrame&) noexcept override {} };
    static constexpr ParamDesc kParams[kNumParams] = {
        {"shimmer", "Shimmer", "", 0, 1, 0.6f}, {"shim_tol", "Tolerance", "dB", 0, 12, 1.5f},
        {"sib", "De-Ess", "", 0, 1, 0.0f},      {"sib_thr", "Ess Thresh", "dB", -30, 12, 0},
        {"phase", "Phase Fix", "", 0, 1, 0.3f}, {"split", "HF Split", "Hz", 2000, 16000, 7000, Taper::Log},
        {"floor", "Max Cut", "dB", -40, 0, -15},
        {"res", "STFT Resolution", "", 0, 2, 0},       {"decay", "Decay", "ms", 5, 200, 10, Taper::Log}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::SpectrumView, -1, 0.5f, 0.28f, 0.9f},
        {ControlKind::Knob, kShimmer, 0.10f, 0.78f, 0.12f}, {ControlKind::Knob, kShimTol, 0.24f, 0.78f, 0.09f},
        {ControlKind::Knob, kSib, 0.38f, 0.78f, 0.12f},     {ControlKind::Knob, kSibThr, 0.52f, 0.78f, 0.09f},
        {ControlKind::Knob, kPhase, 0.66f, 0.78f, 0.12f},   {ControlKind::Knob, kSplit, 0.80f, 0.78f, 0.09f},
        {ControlKind::Knob, kFloor, 0.92f, 0.78f, 0.09f},   {ControlKind::GainReductionMeter, -1, 0.5f, 0.56f, 0.8f}};
    static inline const ProcessorInfo kInfo{"hc.artifact", "HEM Artifact Destroyer", "Repair", 1, kParams, {2.6f, 0xFF3E6CFF, kFace}};

    ArtifactSettings settings_;
    ArtifactTelemetry telemetry_;
    ArtifactKernel kernel_;
    PassKernel bypassKernel_;
    StftEngine stft_;
};

// ----------------------------------------------------------------------------- Limiter
class LimiterProcessor final : public IProcessor {
public:
    enum : std::size_t { kDrive, kCeiling, kRelease, kNumParams };
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override {
        lim_.prepare(sr, 1.5f);
        p_[kDrive].prepare(sr, 30.0);
        push();
    }
    void reset() noexcept override { lim_.reset(); }
    void process(AudioBlock& b) noexcept override {
        push();
        const float g0 = simd::db_to_gain(p_[kDrive].current());
        const float g1 = simd::db_to_gain(p_[kDrive].advance(b.numFrames));
        for (std::uint32_t c = 0; c < b.numChannels; ++c) {
            // in-place ramp: x *= g(t)
            const float dg = (g1 - g0) / static_cast<float>(b.numFrames ? b.numFrames : 1);
            for (std::uint32_t i = 0; i < b.numFrames; ++i) b.ch[c][i] *= g0 + dg * static_cast<float>(i);
        }
        // Bypass = ceiling raised to +60 dB: the limiter becomes a pure delay,
        // so latency (and stem/master alignment) is identical in both states.
        if (bypassed()) lim_.setCeilingDb(60.0f);
        lim_.process(b);
    }
    std::uint32_t latency() const noexcept override { return lim_.latency(); }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    float meterValue() const noexcept override { return lim_.gainReductionDb(); }
    LookaheadLimiter& limiter() noexcept { return lim_; }

private:
    void push() noexcept { lim_.setCeilingDb(p_[kCeiling].target()); lim_.setRelease(p_[kRelease].target()); }
    static constexpr ParamDesc kParams[kNumParams] = {
        {"drive", "Drive", "dB", 0, 24, 0, Taper::Decibel},
        {"ceiling", "Ceiling", "dBTP", -6, 0, -1, Taper::Decibel},
        {"release", "Release", "ms", 5, 1000, 80, Taper::Log}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kDrive, 0.2f, 0.55f, 0.22f}, {ControlKind::Knob, kCeiling, 0.5f, 0.55f, 0.16f},
        {ControlKind::Knob, kRelease, 0.8f, 0.55f, 0.16f}, {ControlKind::GainReductionMeter, -1, 0.5f, 0.12f, 0.8f}};
    static inline const ProcessorInfo kInfo{"hc.limiter", "HEM True-Peak Limiter", "Master", 1, kParams, {2.4f, 0x3EB8FFFF, kFace}};
    std::array<AtomicParam, kNumParams> p_{AtomicParam(0, 0, 24), AtomicParam(-1, -6, 0), AtomicParam(80, 5, 1000)};
    LookaheadLimiter lim_;
};

// ----------------------------------------------------------------------------- Resonance
class ResonanceProcessor final : public IProcessor {
public:
    enum : std::size_t { kSensitivity, kFrequency, kBandwidth, kReduction, kCompensation, kMode, kNumParams };
    ResonanceProcessor() : kernel_(settings_) {}
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override {
        const std::size_t n = sr > 60000.0 ? 4096 : 2048;
        stft_.prepare(sr, n, 4);
        kernel_.prepare(sr, n, stft_.hop());
    }
    void reset() noexcept override { stft_.reset(); kernel_.reset(); }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) { stft_.process(b, pass_); return; }   // latency stays constant
        stft_.process(b, kernel_);
    }
    std::uint32_t latency() const noexcept override { return static_cast<std::uint32_t>(stft_.latency()); }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override {
        switch (i) {
        case kSensitivity: return settings_.sensitivity; case kFrequency: return settings_.frequency;
        case kBandwidth: return settings_.bandwidth;     case kReduction: return settings_.reduction;
        case kCompensation: return settings_.compensation; default: return settings_.mode;
        }
    }
    TripleBuffer<ResonanceView>& view() noexcept { return kernel_.view(); }

private:
    struct PassKernel final : ISpectralKernel { void processFrame(SpectralFrame&) noexcept override {} };
    static constexpr ParamDesc kParams[kNumParams] = {
        {"sens", "Sensitivity", "%", 0, 100, 50},        {"freq", "Frequency", "Hz", 200, 16000, 3500, Taper::Log},
        {"bw", "Bandwidth", "oct", 0.5f, 8, 5},          {"red", "Reduction", "dB", 0, 18, 6},
        {"comp", "Compensation", "%", 0, 100, 50},       {"mode", "Mode", "", 0, 2, 1}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::SpectrumView, -1, 0.5f, 0.3f, 0.9f},
        {ControlKind::Knob, kSensitivity, 0.1f, 0.8f, 0.1f}, {ControlKind::Knob, kFrequency, 0.3f, 0.8f, 0.1f},
        {ControlKind::Knob, kBandwidth, 0.5f, 0.8f, 0.1f},   {ControlKind::Knob, kReduction, 0.7f, 0.8f, 0.1f},
        {ControlKind::Knob, kCompensation, 0.9f, 0.8f, 0.1f}};
    static inline const ProcessorInfo kInfo{"hc.resonance", "HEM Resonance", "Repair", 1, kParams, {2.6f, 0x3EB8FFFF, kFace}};
    ResonanceSettings settings_;
    ResonanceKernel kernel_;
    PassKernel pass_;
    StftEngine stft_;
};

// ----------------------------------------------------------------------------- Transient
class TransientProcessor final : public IProcessor {
public:
    enum : std::size_t { kAttack, kSustain, kSpeed, kGain, kMix, kNumParams };
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override { sh_.prepare(sr); }
    void reset() noexcept override { sh_.reset(); }
    void process(AudioBlock& b) noexcept override {
        if (bypassed()) return;
        sh_.process(b, p_[kAttack].target(), p_[kSustain].target(), p_[kSpeed].target() > 0.5f ? 1 : 0,
                    p_[kMix].target() * 0.01f, p_[kGain].target(), &viz_);
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t i) noexcept override { return p_[i < kNumParams ? i : 0]; }
    SpscRing<TransientViz, 2048>& viz() noexcept { return viz_; }
    bool active() const noexcept { return sh_.active(); }

private:
    static constexpr ParamDesc kParams[kNumParams] = {
        {"attack", "Attack", "dB", -15, 15, 0},  {"sustain", "Sustain", "dB", -15, 15, 0},
        {"speed", "Punch/Speed", "", 0, 1, 0},   {"gain", "Gain", "dB", -12, 12, 0, Taper::Decibel},
        {"mix", "Mix", "%", 0, 100, 100}};
    static constexpr FaceplateControl kFace[] = {
        {ControlKind::Knob, kAttack, 0.1f, 0.8f, 0.12f}, {ControlKind::Knob, kSustain, 0.3f, 0.8f, 0.12f},
        {ControlKind::Switch, kSpeed, 0.5f, 0.8f, 0.05f}, {ControlKind::Knob, kGain, 0.7f, 0.8f, 0.12f},
        {ControlKind::Knob, kMix, 0.9f, 0.8f, 0.12f}};
    static inline const ProcessorInfo kInfo{"hc.transient", "HEM Transient", "Dynamics", 1, kParams, {2.6f, 0x3EB8FFFF, kFace}};
    std::array<AtomicParam, kNumParams> p_{AtomicParam(0, -15, 15), AtomicParam(0, -15, 15), AtomicParam(0, 0, 1),
                                           AtomicParam(0, -12, 12), AtomicParam(100, 0, 100)};
    TransientShaper sh_;
    SpscRing<TransientViz, 2048> viz_;
};

// ----------------------------------------------------------------------------- Width
class WidthProcessor final : public IProcessor {
public:
    enum : std::size_t { kWidth, kNumParams };
    const ProcessorInfo& info() const noexcept override { return kInfo; }
    void prepare(double sr, std::uint32_t) override { p_[0].prepare(sr, 30.0); }
    void reset() noexcept override {}
    void process(AudioBlock& b) noexcept override {
        if (bypassed() || b.numChannels < 2) return;
        // M/S: width 0 % = mono, 100 % = unchanged, 200 % = side doubled.
        const float w0 = p_[0].current() * 0.01f;
        const float w1 = p_[0].advance(b.numFrames) * 0.01f;
        const float dw = (w1 - w0) / static_cast<float>(b.numFrames ? b.numFrames : 1);
        float* L = b.ch[0]; float* R = b.ch[1];
        for (std::uint32_t i = 0; i < b.numFrames; ++i) {
            const float w = w0 + dw * static_cast<float>(i);
            const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]) * w;
            L[i] = m + s; R[i] = m - s;
        }
    }
    std::size_t numParams() const noexcept override { return kNumParams; }
    AtomicParam& param(std::size_t) noexcept override { return p_[0]; }

private:
    static constexpr ParamDesc kParams[kNumParams] = {{"width", "Width", "%", 0, 200, 100}};
    static constexpr FaceplateControl kFace[] = {{ControlKind::Knob, kWidth, 0.5f, 0.5f, 0.3f}};
    static inline const ProcessorInfo kInfo{"hc.width", "HEM Width", "Master", 1, kParams, {1.0f, 0x3EB8FFFF, kFace}};
    std::array<AtomicParam, kNumParams> p_{AtomicParam(100, 0, 200)};
};

inline ProcessorRegistry makeBuiltinRegistry() {
    ProcessorRegistry r;
    r.add("hc.eq6", [] { return std::unique_ptr<IProcessor>(new Eq6()); });
    r.add("hc.comp", [] { return std::unique_ptr<IProcessor>(new CompProcessor()); });
    r.add("hc.artifact", [] { return std::unique_ptr<IProcessor>(new ArtifactProcessor()); });
    r.add("hc.limiter", [] { return std::unique_ptr<IProcessor>(new LimiterProcessor()); });
    r.add("hc.resonance", [] { return std::unique_ptr<IProcessor>(new ResonanceProcessor()); });
    r.add("hc.transient", [] { return std::unique_ptr<IProcessor>(new TransientProcessor()); });
    r.add("hc.width", [] { return std::unique_ptr<IProcessor>(new WidthProcessor()); });
    return r;
}

} // namespace hc
