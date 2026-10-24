// =============================================================================
//  dsp/fft.hpp — Math & DSP Engine: scratch-built FFT (no FFTW, no pffft).
//
//  Layout: SPLIT complex (separate re[] / im[] arrays). Interleaved complex
//  forces shuffles inside every butterfly; split format lets one AVX2 register
//  hold 8 real parts and another 8 imaginary parts, so a complex multiply is
//  2 FMAs + 2 MULs with zero shuffles.
//
//  Algorithm: iterative radix-2 decimation-in-time.
//    1. Bit-reversal permutation (precomputed swap table).
//    2. log2(M) butterfly stages. Stage twiddles are stored CONTIGUOUSLY per
//       stage, so in stages with half-span >= 8 the inner loop is pure 8-wide
//       loads → FMA → stores. Early stages (span 1,2,4) run scalar; they're
//       3 of ~10 stages and have trivial twiddles.
//
//  Real FFT of size N is computed with ONE complex FFT of size N/2 (pack even
//  samples into re, odd into im) plus an O(N) split step — ~2x faster than a
//  full complex transform of real data.
//
//  Scaling: forward is unnormalised; inverse divides by size, so
//  inverse(forward(x)) == x.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/dsp/simd.hpp"
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hc {

class ComplexFFT {
public:
    ComplexFFT() = default;
    explicit ComplexFFT(std::size_t m) { prepare(m); }

    // Non-RT. m must be a power of two >= 2.
    void prepare(std::size_t m) {
        if (m < 2 || (m & (m - 1)) != 0) throw std::invalid_argument("ComplexFFT size must be power of two >= 2");
        m_ = m;
        log2m_ = 0;
        while ((std::size_t{1} << log2m_) < m) ++log2m_;

        // Bit-reversal swap pairs (only i < rev(i) to avoid double swapping).
        swaps_.clear();
        for (std::size_t i = 0; i < m; ++i) {
            std::size_t r = 0;
            for (unsigned b = 0; b < log2m_; ++b) r |= ((i >> b) & 1u) << (log2m_ - 1 - b);
            if (i < r) swaps_.emplace_back(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(r));
        }

        // Contiguous per-stage twiddles W_len^j = exp(-2πi j / len), j < len/2.
        // Total storage = 1 + 2 + 4 + ... + m/2 = m - 1.
        twr_.resize(m);
        twi_.resize(m);
        stageOffset_.assign(log2m_ + 1, 0);
        std::size_t off = 0;
        for (unsigned s = 1; s <= log2m_; ++s) {
            const std::size_t len = std::size_t{1} << s, half = len >> 1;
            stageOffset_[s] = off;
            for (std::size_t j = 0; j < half; ++j) {
                const double a = -2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(len);
                twr_[off + j] = static_cast<float>(std::cos(a));
                twi_[off + j] = static_cast<float>(std::sin(a));
            }
            off += half;
        }
    }

    std::size_t size() const noexcept { return m_; }

    // In-place forward transform, X[k] = Σ x[n] e^{-2πikn/M}.
    void forward(float* re, float* im) const noexcept { transform(re, im); }

    // In-place inverse, scaled by 1/M. Uses the swap identity:
    //   IDFT(x) = swap(DFT(swap(x))) / M,  swap(a+ib) = b+ia
    // so the forward kernel is reused with re/im pointers exchanged.
    void inverse(float* re, float* im) const noexcept {
        transform(im, re);
        const float s = 1.0f / static_cast<float>(m_);
        simd::scale(re, s, m_);
        simd::scale(im, s, m_);
    }

private:
    void transform(float* re, float* im) const noexcept {
        for (const auto& [a, b] : swaps_) {
            std::swap(re[a], re[b]);
            std::swap(im[a], im[b]);
        }
        for (unsigned s = 1; s <= log2m_; ++s) {
            const std::size_t len = std::size_t{1} << s, half = len >> 1;
            const float* wr = twr_.data() + stageOffset_[s];
            const float* wi = twi_.data() + stageOffset_[s];
            for (std::size_t i = 0; i < m_; i += len) {
                float* ur = re + i; float* ui = im + i;
                float* vr = re + i + half; float* vi = im + i + half;
                std::size_t j = 0;
#if HC_AVX2
                for (; j + 8 <= half; j += 8) {
                    const __m256 cwr = _mm256_loadu_ps(wr + j), cwi = _mm256_loadu_ps(wi + j);
                    const __m256 xr = _mm256_loadu_ps(vr + j), xi = _mm256_loadu_ps(vi + j);
                    // t = v * w (complex)
                    const __m256 tr = simd::fmsub(xr, cwr, _mm256_mul_ps(xi, cwi));
                    const __m256 ti = simd::fmadd(xr, cwi, _mm256_mul_ps(xi, cwr));
                    const __m256 ar = _mm256_loadu_ps(ur + j), ai = _mm256_loadu_ps(ui + j);
                    _mm256_storeu_ps(ur + j, _mm256_add_ps(ar, tr));
                    _mm256_storeu_ps(ui + j, _mm256_add_ps(ai, ti));
                    _mm256_storeu_ps(vr + j, _mm256_sub_ps(ar, tr));
                    _mm256_storeu_ps(vi + j, _mm256_sub_ps(ai, ti));
                }
#endif
                for (; j < half; ++j) {
                    const float tr = vr[j] * wr[j] - vi[j] * wi[j];
                    const float ti = vr[j] * wi[j] + vi[j] * wr[j];
                    const float ar = ur[j], ai = ui[j];
                    ur[j] = ar + tr; ui[j] = ai + ti;
                    vr[j] = ar - tr; vi[j] = ai - ti;
                }
            }
        }
    }

    std::size_t m_ = 0;
    unsigned log2m_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> swaps_;
    std::vector<std::size_t> stageOffset_;
    AlignedBuffer<float> twr_, twi_;
};

// -----------------------------------------------------------------------------
// RealFFT — N real samples ↔ N/2+1 complex bins (split re/im).
//
//  Pack z[n] = x[2n] + i·x[2n+1], Z = FFT_{N/2}(z). With W = e^{-2πik/N}:
//    E[k] = (Z[k] + conj Z[M-k]) / 2          (spectrum of even samples)
//    O[k] = (Z[k] - conj Z[M-k]) / (2i)       (spectrum of odd samples)
//    X[k] = E[k] + W^k · O[k],   k = 0..M     (Z[M] ≡ Z[0])
//  Inverse runs the same algebra backwards:
//    E[k] = (X[k] + conj X[M-k]) / 2
//    O[k] = (X[k] - conj X[M-k]) · conj(W^k) / 2
//    Z[k] = E[k] + i·O[k]  →  IFFT_{N/2}  →  unpack.
// -----------------------------------------------------------------------------
class RealFFT {
public:
    RealFFT() = default;
    explicit RealFFT(std::size_t n) { prepare(n); }

    void prepare(std::size_t n) {
        if (n < 4 || (n & (n - 1)) != 0) throw std::invalid_argument("RealFFT size must be power of two >= 4");
        n_ = n; m_ = n / 2;
        cfft_.prepare(m_);
        zr_.resize(m_); zi_.resize(m_);
        wr_.resize(m_ + 1); wi_.resize(m_ + 1);
        for (std::size_t k = 0; k <= m_; ++k) {
            const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
            wr_[k] = static_cast<float>(std::cos(a));
            wi_[k] = static_cast<float>(std::sin(a));
        }
    }

    std::size_t size() const noexcept { return n_; }
    std::size_t bins() const noexcept { return m_ + 1; }

    // x[n] (n_ samples) → re[k], im[k] (n_/2+1 bins). RT-safe (no allocation).
    void forward(const float* x, float* re, float* im) noexcept {
        float* zr = zr_.data(); float* zi = zi_.data();
        for (std::size_t i = 0; i < m_; ++i) { zr[i] = x[2 * i]; zi[i] = x[2 * i + 1]; }
        cfft_.forward(zr, zi);
        for (std::size_t k = 0; k <= m_; ++k) {
            const std::size_t k1 = (k == m_) ? 0 : k;
            const std::size_t k2 = (k == 0) ? 0 : m_ - k;
            const float a = zr[k1], b = zi[k1], c = zr[k2], d = zi[k2];
            const float er = 0.5f * (a + c), ei = 0.5f * (b - d);
            const float orr = 0.5f * (b + d), oi = -0.5f * (a - c);
            re[k] = er + wr_[k] * orr - wi_[k] * oi;
            im[k] = ei + wr_[k] * oi + wi_[k] * orr;
        }
    }

    // re/im (n_/2+1 bins) → x (n_ samples), exact inverse of forward().
    // DC and Nyquist imaginary parts are ignored (they are 0 for real signals).
    void inverse(const float* re, const float* im, float* x) noexcept {
        float* zr = zr_.data(); float* zi = zi_.data();
        for (std::size_t k = 0; k < m_; ++k) {
            const std::size_t km = m_ - k;
            const float xr = re[k], xi = (k == 0) ? 0.0f : im[k];
            const float yr = re[km], yi = (km == m_) ? 0.0f : -im[km]; // conj X[M-k]
            const float er = 0.5f * (xr + yr), ei = 0.5f * (xi + yi);
            const float dr = 0.5f * (xr - yr), di = 0.5f * (xi - yi);
            // O = d · conj(W^k)
            const float orr = dr * wr_[k] + di * wi_[k];
            const float oi = di * wr_[k] - dr * wi_[k];
            zr[k] = er - oi;   // Z = E + i·O
            zi[k] = ei + orr;
        }
        cfft_.inverse(zr, zi);
        for (std::size_t i = 0; i < m_; ++i) { x[2 * i] = zr[i]; x[2 * i + 1] = zi[i]; }
    }

private:
    std::size_t n_ = 0, m_ = 0;
    ComplexFFT cfft_;
    AlignedBuffer<float> zr_, zi_, wr_, wi_;
};

} // namespace hc
