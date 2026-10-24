// =============================================================================
//  meter/spectrum.hpp — high-resolution spectrum analyser (Analysis Engine).
//
//  The audio thread does NOT run these FFTs. It pushes a mono/mid copy of
//  each stem into a lock-free SampleTap (memcpy cost only). A separate,
//  normal-priority Analysis thread drains the taps and runs the analysers.
//  If the analysis thread falls behind, taps drop samples — the meters glitch,
//  the AUDIO never does. That is the whole point of the split.
//
//  Resolution: 8192-point FFT → 5.86 Hz bins at 48 kHz, enough to resolve
//  individual low-E harmonics. 4-term Blackman–Harris window: −92 dB
//  sidelobes, so a mastered kick at 0 dBFS does not smear leakage across the
//  high band you're trying to inspect for artefacts.
//  Hop 1024 → ~47 frames/s per stem.
//  Ballistics: exponential averaging + peak-hold with linear dB fall.
//  Display smoothing (1/N-octave) is done on the GPU side per pixel column.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/lockfree.hpp"
#include "hc/dsp/fft.hpp"
#include "hc/dsp/simd.hpp"
#include <cmath>
#include <numbers>

namespace hc {

using SampleTap = SpscRing<float, 1u << 16>;   // ~1.36 s mono @48 kHz per stem

struct SpectrumFrame {
    static constexpr std::size_t kFft = 8192;
    static constexpr std::size_t kBins = kFft / 2 + 1;
    float averageDb[kBins];
    float peakHoldDb[kBins];
    float binHz = 0.0f;
    std::uint64_t serial = 0;
};

class SpectrumAnalyzer {
public:
    static constexpr std::size_t kFft = SpectrumFrame::kFft;
    static constexpr std::size_t kBins = SpectrumFrame::kBins;
    static constexpr std::size_t kHop = 1024;

    // Non-RT (analysis thread).
    void prepare(double sr, float avgTimeMs = 300.0f, float peakFallDbPerSec = 12.0f) {
        sr_ = sr;
        fft_.prepare(kFft);
        win_.resize(kFft); ring_.resize(kFft); frame_.resize(kFft);
        re_.resize(kBins); im_.resize(kBins); pow_.resize(kBins);
        avg_.resize(kBins); peak_.resize(kBins);
        constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
        double wsum = 0.0;
        for (std::size_t i = 0; i < kFft; ++i) {
            const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(kFft);
            win_[i] = static_cast<float>(a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) - a3 * std::cos(3 * x));
            wsum += win_[i];
        }
        // Calibrate so a full-scale sine reads 0 dB at its bin peak:
        // |X[k]| = A·Σw/2  →  power scale = (2/Σw)².
        powScale_ = static_cast<float>((2.0 / wsum) * (2.0 / wsum));
        const double framesPerSec = sr / static_cast<double>(kHop);
        avgCoef_ = static_cast<float>(std::exp(-1.0 / (framesPerSec * avgTimeMs * 0.001)));
        peakFall_ = static_cast<float>(peakFallDbPerSec / framesPerSec);
        for (std::size_t k = 0; k < kBins; ++k) { avg_[k] = -140.0f; peak_[k] = -140.0f; }
        ringPos_ = 0; sinceHop_ = 0; serial_ = 0;
    }

    // Feed samples; returns true if at least one new frame was produced.
    bool feed(const float* x, std::size_t n) noexcept {
        bool produced = false;
        for (std::size_t i = 0; i < n; ++i) {
            ring_[ringPos_] = x[i];
            ringPos_ = (ringPos_ + 1) & (kFft - 1);
            if (++sinceHop_ == kHop) { sinceHop_ = 0; analyse(); produced = true; }
        }
        return produced;
    }

    // Drain a tap and analyse. Call from the analysis thread.
    bool drain(SampleTap& tap) noexcept {
        float tmp[1024];
        bool produced = false;
        std::size_t got;
        while ((got = tap.pop_n(tmp, 1024)) > 0) produced |= feed(tmp, got);
        return produced;
    }

    void snapshot(SpectrumFrame& out) const noexcept {
        simd::copy(out.averageDb, avg_.data(), kBins);
        simd::copy(out.peakHoldDb, peak_.data(), kBins);
        out.binHz = static_cast<float>(sr_ / static_cast<double>(kFft));
        out.serial = serial_;
    }

    // Raw access for the artefact diagnostics.
    const float* averageDb() const noexcept { return avg_.data(); }
    double sampleRate() const noexcept { return sr_; }

private:
    void analyse() noexcept {
        // Unroll ring into time order, oldest first, and window.
        const std::size_t first = kFft - ringPos_;
        simd::mul(frame_.data(), ring_.data() + ringPos_, win_.data(), first);
        simd::mul(frame_.data() + first, ring_.data(), win_.data() + first, ringPos_);
        fft_.forward(frame_.data(), re_.data(), im_.data());
        simd::complex_power(pow_.data(), re_.data(), im_.data(), kBins);
        for (std::size_t k = 0; k < kBins; ++k) {
            const float db = static_cast<float>(simd::power_to_db(static_cast<double>(pow_[k]) * powScale_ + 1e-14));
            avg_[k] = db + (avg_[k] - db) * avgCoef_;
            peak_[k] = std::max(db, peak_[k] - peakFall_);
        }
        ++serial_;
    }

    double sr_ = 48000.0;
    RealFFT fft_;
    AlignedBuffer<float> win_, ring_, frame_, re_, im_, pow_, avg_, peak_;
    std::size_t ringPos_ = 0, sinceHop_ = 0;
    float powScale_ = 1.0f, avgCoef_ = 0.9f, peakFall_ = 0.25f;
    std::uint64_t serial_ = 0;
};

} // namespace hc
