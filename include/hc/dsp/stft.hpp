// =============================================================================
//  dsp/stft.hpp — STFT / Overlap-Add engine (multichannel, linked frames).
//
//  THEORY
//  ------
//  Analysis:  X_m[k] = FFT{ x[n + mH] · w_a[n] },  n = 0..N-1
//  Synthesis: y[n]   = Σ_m  w_s[n - mH] · IFFT{ G_m[k]·X_m[k] }[n - mH]
//
//  With w_a = w_s = sqrt(periodic Hann) and hop H = N/4 (75% overlap), the
//  product w_a·w_s = Hann, and Σ_m Hann(n - mH) = 2 for every n (COLA). We
//  divide by that constant, so with G = 1 the engine is an exact identity
//  delayed by N samples (verified in tests to < -100 dB error).
//
//  Why sqrt-Hann on BOTH sides: any spectral gain G applied between FFT and
//  IFFT creates time-domain aliasing at frame edges. The synthesis window
//  tapers those edges to zero, so modifications never produce clicks.
//
//  Why 75% overlap (not 50%): with gains that change frame-to-frame, 4x
//  redundancy averages the "musical noise" of per-bin gain decisions and gives
//  each bin's gain trajectory 4x the time resolution.
//
//  All channels are framed in lock-step so a kernel can work in mid/side or
//  compute inter-channel coherence on the same frame.
//
//  Latency: exactly N samples (reported to the engine for delay compensation).
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/config.hpp"
#include "hc/dsp/fft.hpp"
#include "hc/dsp/simd.hpp"
#include <cmath>
#include <cstddef>
#include <numbers>

namespace hc {

// One analysed frame, all channels. Kernels modify re/im in place.
struct SpectralFrame {
    float* re[kNumChannels] = {};
    float* im[kNumChannels] = {};
    std::uint32_t numChannels = kNumChannels;
    std::size_t bins = 0;       // N/2 + 1
    std::size_t fftSize = 0;
    double sampleRate = 48000.0;
    float binHz = 0.0f;         // sampleRate / N
};

class ISpectralKernel {
public:
    virtual ~ISpectralKernel() = default;
    virtual void processFrame(SpectralFrame& f) noexcept = 0;
};

class StftEngine {
public:
    // Non-RT.
    void prepare(double sampleRate, std::size_t fftSize, std::size_t overlap = 4) {
        sr_ = sampleRate; n_ = fftSize; hop_ = fftSize / overlap;
        fft_.prepare(n_);
        bins_ = n_ / 2 + 1;
        win_.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n_));
            win_[i] = static_cast<float>(std::sqrt(hann));
        }
        // COLA constant: Σ_m w_a·w_s at any n (computed, not assumed).
        double cola = 0.0;
        for (std::size_t m = 0; m < n_; m += hop_) cola += static_cast<double>(win_[m]) * win_[m];
        olaScale_ = static_cast<float>(1.0 / cola);
        for (std::size_t c = 0; c < kNumChannels; ++c) {
            in_[c].resize(n_); out_[c].resize(n_); re_[c].resize(bins_); im_[c].resize(bins_);
        }
        frameBuf_.resize(n_);
        reset();
    }

    void reset() noexcept {
        for (std::size_t c = 0; c < kNumChannels; ++c) { in_[c].zero(); out_[c].zero(); }
        inPos_ = n_ - hop_;
        outPos_ = 0;
    }

    std::size_t latency() const noexcept { return n_; }
    std::size_t fftSize() const noexcept { return n_; }
    std::size_t hop() const noexcept { return hop_; }

    // RT. Streams `block` through, calling kernel.processFrame() every hop.
    void process(AudioBlock& block, ISpectralKernel& kernel) noexcept {
        std::size_t done = 0;
        const std::size_t total = block.numFrames;
        while (done < total) {
            const std::size_t chunk = std::min(total - done, n_ - inPos_);
            for (std::uint32_t c = 0; c < block.numChannels; ++c) {
                float* io = block.ch[c] + done;
                simd::copy(in_[c].data() + inPos_, io, chunk);   // feed analysis FIFO
                simd::copy(io, out_[c].data() + outPos_, chunk); // emit finished OLA samples
            }
            inPos_ += chunk; outPos_ += chunk; done += chunk;
            if (inPos_ == n_) runFrame(block.numChannels, kernel);
        }
    }

private:
    void runFrame(std::uint32_t numCh, ISpectralKernel& kernel) noexcept {
        SpectralFrame f;
        f.numChannels = numCh; f.bins = bins_; f.fftSize = n_; f.sampleRate = sr_;
        f.binHz = static_cast<float>(sr_ / static_cast<double>(n_));
        for (std::uint32_t c = 0; c < numCh; ++c) {
            simd::mul(frameBuf_.data(), in_[c].data(), win_.data(), n_);   // analysis window
            fft_.forward(frameBuf_.data(), re_[c].data(), im_[c].data());
            f.re[c] = re_[c].data(); f.im[c] = im_[c].data();
        }

        kernel.processFrame(f);

        for (std::uint32_t c = 0; c < numCh; ++c) {
            float* acc = out_[c].data();
            // Retire the hop we just emitted, slide the accumulator, zero the tail.
            std::copy(acc + hop_, acc + n_, acc);
            simd::clear(acc + (n_ - hop_), hop_);
            fft_.inverse(re_[c].data(), im_[c].data(), frameBuf_.data());
            simd::scale(frameBuf_.data(), olaScale_, n_);
            simd::mul_add(acc, frameBuf_.data(), win_.data(), n_);          // synthesis window + OLA
            // Slide input FIFO by one hop.
            float* in = in_[c].data();
            std::copy(in + hop_, in + n_, in);
        }
        inPos_ = n_ - hop_;
        outPos_ = 0;
    }

    double sr_ = 48000.0;
    std::size_t n_ = 0, hop_ = 0, bins_ = 0, inPos_ = 0, outPos_ = 0;
    float olaScale_ = 1.0f;
    RealFFT fft_;
    AlignedBuffer<float> win_, frameBuf_;
    AlignedBuffer<float> in_[kNumChannels], out_[kNumChannels], re_[kNumChannels], im_[kNumChannels];
};

} // namespace hc
