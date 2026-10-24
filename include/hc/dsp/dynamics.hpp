// =============================================================================
//  dsp/dynamics.hpp — compressor + true-peak-aware lookahead limiter.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/dsp/simd.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <vector>

namespace hc {

// -----------------------------------------------------------------------------
// Compressor — feed-forward, stereo-linked, log-domain gain computer.
//
//  Detector: peak of |L|,|R| → dB.
//  Gain computer with soft knee W (Giannoulis/Massberg/Reiss 2012):
//      x <  T − W/2        : g = 0
//      |x − T| <= W/2      : g = (1/R − 1)(x − T + W/2)² / (2W)
//      x >  T + W/2        : g = (1/R − 1)(x − T)
//  Ballistics are applied to the gain-reduction signal in dB (branching
//  attack/release smoother) — smooth, program-dependent, no pumping
//  artefacts from smoothing the detector before the knee.
// -----------------------------------------------------------------------------
class Compressor {
public:
    struct Settings {
        float thresholdDb = -18.0f, ratio = 3.0f, kneeDb = 6.0f;
        float attackMs = 10.0f, releaseMs = 120.0f, makeupDb = 0.0f;
    };

    void prepare(double sr) noexcept { sr_ = sr; setSettings(s_); reset(); }
    void reset() noexcept { grDb_ = 0.0f; }

    void setSettings(const Settings& s) noexcept {
        s_ = s;
        att_ = static_cast<float>(std::exp(-1.0 / (sr_ * std::max(0.01f, s.attackMs) * 0.001)));
        rel_ = static_cast<float>(std::exp(-1.0 / (sr_ * std::max(1.0f, s.releaseMs) * 0.001)));
        makeup_ = simd::db_to_gain(s.makeupDb);
    }

    float gainComputerDb(float xDb) const noexcept {
        const float T = s_.thresholdDb, W = s_.kneeDb, slope = 1.0f / s_.ratio - 1.0f;
        const float d = xDb - T;
        if (2.0f * d < -W) return 0.0f;
        if (W > 0.0f && 2.0f * std::fabs(d) <= W) { const float t = d + 0.5f * W; return slope * t * t / (2.0f * W); }
        return slope * d;
    }

    void process(AudioBlock& b) noexcept {
        float* L = b.ch[0];
        float* R = b.numChannels > 1 ? b.ch[1] : b.ch[0];
        for (std::uint32_t i = 0; i < b.numFrames; ++i) {
            const float pk = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float target = gainComputerDb(simd::gain_to_db(pk));    // <= 0
            const float coef = target < grDb_ ? att_ : rel_;
            grDb_ = target + (grDb_ - target) * coef;
            const float g = simd::db_to_gain(grDb_) * makeup_;
            L[i] *= g;
            if (b.numChannels > 1) R[i] *= g;
        }
    }
    float gainReductionDb() const noexcept { return grDb_; }

private:
    Settings s_{};
    double sr_ = 48000.0;
    float att_ = 0, rel_ = 0, makeup_ = 1, grDb_ = 0;
};

// -----------------------------------------------------------------------------
// TruePeakOversampler — polyphase 4x interpolator used by BOTH the true-peak
// meter and the limiter's detector, so the limiter limits what the meter sees.
//
//  Filter: Kaiser-windowed sinc (β = 8), 97 taps at 4x, symmetric/linear
//  phase, cutoff at the ORIGINAL Nyquist. This follows the BS.1770-4 Annex 2 method (4x
//  oversample at 48 kHz, take the max of |y|), but the coefficients are this
//  suite's own design, not the Annex 2 example table. Expect readings within a
//  few hundredths of a dB of other compliant meters, not bit-identical.
//
//  History is stored twice (x[i] and x[i+T]) so every phase is one contiguous
//  AVX2 dot product with no wrap-around branch.
// -----------------------------------------------------------------------------
class TruePeakOversampler {
public:
    // Odd-length symmetric (linear-phase) prototype: factor·24 + 1 taps,
    // centred on an integer so branch 0 reproduces the original samples
    // exactly. Each branch therefore has up to 25 taps (zero-padded to 32
    // slots so every branch is four 8-wide AVX2 FMAs).
    static constexpr std::size_t kHalfSpan = 12;            // base-rate samples each side of centre
    static constexpr std::size_t kSlots = 32;               // padded branch length (≥ 2·kHalfSpan+1)
    static constexpr std::size_t kMaxFactor = 4;

    void prepare(double sr) {
        factor_ = sr < 96000.0 ? 4 : (sr < 192000.0 ? 2 : 1);
        const std::size_t total = 2 * kHalfSpan * factor_ + 1;
        const double centre = static_cast<double>(kHalfSpan * factor_);
        const double beta = 8.0;
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        std::vector<double> h(total);
        for (std::size_t n = 0; n < total; ++n) {
            const double t = (static_cast<double>(n) - centre) / static_cast<double>(factor_);
            const double sinc = (std::fabs(t) < 1e-12) ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            const double r = (static_cast<double>(n) - centre) / centre;
            h[n] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
        }
        for (auto& ph : phase_) std::fill(std::begin(ph), std::end(ph), 0.0f);
        for (std::size_t p = 0; p < factor_; ++p) {
            // Branch p, tap j multiplies x[newest − j]. Normalise each branch to
            // unity DC gain so a DC input never reads above its true level.
            double sum = 0.0;
            for (std::size_t j = 0; j * factor_ + p < total; ++j) sum += h[j * factor_ + p];
            for (std::size_t j = 0; j * factor_ + p < total; ++j)
                phase_[p][kSlots - 1 - j] = static_cast<float>(h[j * factor_ + p] / sum);
        }
        reset();
        delay_ = static_cast<std::uint32_t>(kHalfSpan);
    }
    void reset() noexcept { for (auto& c : hist_) std::fill(std::begin(c), std::end(c), 0.0f); pos_ = 0; }

    // Push one stereo frame; returns max |interpolated| across both channels
    // and all branches (true-peak estimate centred kHalfSpan samples back).
    float pushFrame(float l, float r) noexcept {
        hist_[0][pos_] = hist_[0][pos_ + kSlots] = l;
        hist_[1][pos_] = hist_[1][pos_ + kSlots] = r;
        pos_ = (pos_ + 1) % kSlots;
        float m = 0.0f;
        for (std::size_t c = 0; c < 2; ++c) {
            const float* x = hist_[c] + pos_;                // oldest → newest, contiguous
            for (std::size_t p = 0; p < factor_; ++p)
                m = std::max(m, std::fabs(simd::dot(x, phase_[p], kSlots)));
        }
        return m;
    }
    std::uint32_t delay() const noexcept { return delay_; }   // group delay, base-rate samples
    std::size_t factor() const noexcept { return factor_; }

private:
    alignas(32) float phase_[kMaxFactor][kSlots] = {};
    alignas(32) float hist_[2][2 * kSlots] = {};
    std::size_t pos_ = 0, factor_ = 4;
    std::uint32_t delay_ = 0;
};

// -----------------------------------------------------------------------------
// LookaheadLimiter — brick-wall, true-peak-aware, zero overshoot by design.
//
//  For each input sample n:
//    p[n]   = true-peak estimate (4x oversampled, both channels)
//    r[n]   = min(1, ceiling / p[n])                 required gain
//    h[n]   = min r[n−L+1 .. n]                      sliding-window min (hold)
//    a[n]   = mean h[n−L+1 .. n]                     box smoothing (attack ramp)
//    g[n]   = a[n] if falling, else release-smoothed toward a[n]  (g ≤ a always)
//    y[n]   = x[n − (L−1) − D] · g[n]
//  Proof of no overshoot: every h averaged into a[n] spans the peak at
//  n−(L−1), so a[n] ≤ r(peak). D compensates the oversampler's group delay.
//  Sliding min = monotonic deque on a fixed ring → O(1) amortised, no alloc.
// -----------------------------------------------------------------------------
class LookaheadLimiter {
public:
    void prepare(double sr, float lookaheadMs = 1.5f) {
        sr_ = sr;
        tp_.prepare(sr);
        L_ = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::lround(sr * lookaheadMs * 0.001)));
        const std::uint32_t delayLen = L_ - 1 + tp_.delay();
        delayN_ = delayLen + 1;
        dl_[0].resize(delayN_); dl_[1].resize(delayN_);
        boxRing_.resize(L_);
        dqVal_.resize(L_ + 1); dqIdx_.resize(L_ + 1);
        setRelease(80.0f);
        reset();
    }
    void reset() noexcept {
        tp_.reset(); dl_[0].zero(); dl_[1].zero(); boxRing_.zero();
        dlPos_ = 0; boxPos_ = 0; boxSum_ = static_cast<double>(L_); // ring starts full of 1.0
        for (std::uint32_t i = 0; i < L_; ++i) boxRing_[i] = 1.0f;
        dqHead_ = dqTail_ = 0; n_ = 0; g_ = 1.0f; minGain_ = 1.0f;
    }
    // 0.05 dB internal margin covers the residual between the detector's 4x
    // grid and the continuous-time peak (measured ~0.02 dB on hot noise).
    void setCeilingDb(float db) noexcept { ceiling_ = simd::db_to_gain(db - 0.05f); }
    void setRelease(float ms) noexcept { rel_ = static_cast<float>(std::exp(-1.0 / (sr_ * std::max(1.0f, ms) * 0.001))); }
    std::uint32_t latency() const noexcept { return delayN_ - 1; }
    float gainReductionDb() const noexcept { return simd::gain_to_db(minGain_); }
    void clearGrMeter() noexcept { minGain_ = 1.0f; }

    void process(AudioBlock& b) noexcept {
        float* L = b.ch[0];
        float* R = b.numChannels > 1 ? b.ch[1] : b.ch[0];
        const std::uint32_t cap = L_ + 1;
        for (std::uint32_t i = 0; i < b.numFrames; ++i) {
            const float p = tp_.pushFrame(L[i], R[i]);
            const float req = p > ceiling_ ? ceiling_ / p : 1.0f;

            // --- sliding minimum over last L_ samples (monotonic deque) ---
            // Expire first (window = n-L+1..n), then push, so the ring of
            // capacity L+1 never holds more than L entries.
            while (dqHead_ != dqTail_ && n_ - dqIdx_[dqHead_] >= L_) dqHead_ = (dqHead_ + 1) % cap;
            while (dqHead_ != dqTail_ && dqVal_[(dqTail_ + cap - 1) % cap] >= req) dqTail_ = (dqTail_ + cap - 1) % cap;
            dqVal_[dqTail_] = req; dqIdx_[dqTail_] = n_; dqTail_ = (dqTail_ + 1) % cap;
            const float hold = dqVal_[dqHead_];

            // --- box average over last L_ hold values ---
            boxSum_ += static_cast<double>(hold) - boxRing_[boxPos_];
            boxRing_[boxPos_] = hold;
            boxPos_ = (boxPos_ + 1) % L_;
            const float a = std::min(1.0f, static_cast<float>(boxSum_ / static_cast<double>(L_)));

            // --- release (never above a) ---
            g_ = (a < g_) ? a : a + (g_ - a) * rel_;
            minGain_ = std::min(minGain_, g_);

            // --- delay line ---
            const std::uint32_t rd = (dlPos_ + 1) % delayN_;        // oldest sample
            dl_[0][dlPos_] = L[i]; dl_[1][dlPos_] = R[i];
            const float yl = dl_[0][rd] * g_, yr = dl_[1][rd] * g_;
            dlPos_ = rd;
            L[i] = yl;
            if (b.numChannels > 1) R[i] = yr;
            ++n_;
        }
    }

private:
    double sr_ = 48000.0;
    TruePeakOversampler tp_;
    std::uint32_t L_ = 2, delayN_ = 2, dlPos_ = 0, boxPos_ = 0, dqHead_ = 0, dqTail_ = 0;
    std::uint64_t n_ = 0;
    double boxSum_ = 0.0;
    float ceiling_ = 0.891f, rel_ = 0.999f, g_ = 1.0f, minGain_ = 1.0f;
    AlignedBuffer<float> dl_[2], boxRing_, dqVal_;
    AlignedBuffer<std::uint64_t> dqIdx_;
};

} // namespace hc
