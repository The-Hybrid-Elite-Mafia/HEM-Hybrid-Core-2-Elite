// =============================================================================
//  dsp/simd.hpp — Math & DSP Engine, vector kernels.
//
//  Every hot loop in the suite funnels through these kernels. AVX2 path does
//  8 floats per instruction (+FMA where available); a scalar tail handles the
//  remainder so callers can pass any length. Built with /arch:AVX2 (MSVC) or
//  -mavx2 -mfma (GCC/Clang). If AVX2 is not enabled at compile time the scalar
//  path is used and the program still runs everywhere.
//
//  NOTE on AVX-512: Intel 12th–14th gen consumer CPUs (incl. the i7-14700HX)
//  do NOT expose AVX-512. AVX2 is the production target; an AVX-512 path would
//  be dead code on those machines and is deliberately not shipped.
// =============================================================================
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#if defined(__AVX2__)
  #include <immintrin.h>
  #define HC_AVX2 1
#else
  #define HC_AVX2 0
#endif
#if defined(__SSE2__) || defined(_M_X64)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#endif

namespace hc::simd {

#if HC_AVX2
inline __m256 fmadd(__m256 a, __m256 b, __m256 c) noexcept {
  #if defined(__FMA__)
    return _mm256_fmadd_ps(a, b, c);
  #else
    return _mm256_add_ps(_mm256_mul_ps(a, b), c);
  #endif
}
inline __m256 fmsub(__m256 a, __m256 b, __m256 c) noexcept { // a*b - c
  #if defined(__FMA__)
    return _mm256_fmsub_ps(a, b, c);
  #else
    return _mm256_sub_ps(_mm256_mul_ps(a, b), c);
  #endif
}
inline float hsum(__m256 v) noexcept {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    __m128 sh = _mm_movehdup_ps(lo);
    __m128 s = _mm_add_ps(lo, sh);
    sh = _mm_movehl_ps(sh, s);
    s = _mm_add_ss(s, sh);
    return _mm_cvtss_f32(s);
}
inline float hmax(__m256 v) noexcept {
    __m128 m = _mm_max_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    m = _mm_max_ps(m, _mm_movehl_ps(m, m));
    m = _mm_max_ss(m, _mm_shuffle_ps(m, m, 1));
    return _mm_cvtss_f32(m);
}
#endif

// dst[i] = 0
inline void clear(float* dst, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    const __m256 z = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, z);
#endif
    for (; i < n; ++i) dst[i] = 0.0f;
}

// dst[i] = src[i]
inline void copy(float* dst, const float* src, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, _mm256_loadu_ps(src + i));
#endif
    for (; i < n; ++i) dst[i] = src[i];
}

// dst[i] *= g
inline void scale(float* dst, float g, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    const __m256 vg = _mm256_set1_ps(g);
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(dst + i), vg));
#endif
    for (; i < n; ++i) dst[i] *= g;
}

// dst[i] += src[i] * g   (the summing-bus kernel)
inline void mac(float* dst, const float* src, float g, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    const __m256 vg = _mm256_set1_ps(g);
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(dst + i, fmadd(_mm256_loadu_ps(src + i), vg, _mm256_loadu_ps(dst + i)));
#endif
    for (; i < n; ++i) dst[i] += src[i] * g;
}

// dst[i] += src[i] * (g0 + i*dg)  — linear gain ramp, click-free fader moves
inline void mac_ramp(float* dst, const float* src, float g0, float g1, std::size_t n) noexcept {
    if (n == 0) return;
    const float dg = (g1 - g0) / static_cast<float>(n);
    std::size_t i = 0;
#if HC_AVX2
    __m256 vg = _mm256_setr_ps(g0, g0 + dg, g0 + 2 * dg, g0 + 3 * dg, g0 + 4 * dg, g0 + 5 * dg, g0 + 6 * dg, g0 + 7 * dg);
    const __m256 vstep = _mm256_set1_ps(8.0f * dg);
    for (; i + 8 <= n; i += 8) {
        _mm256_storeu_ps(dst + i, fmadd(_mm256_loadu_ps(src + i), vg, _mm256_loadu_ps(dst + i)));
        vg = _mm256_add_ps(vg, vstep);
    }
#endif
    for (; i < n; ++i) dst[i] += src[i] * (g0 + dg * static_cast<float>(i));
}

// dst[i] = a[i] * b[i]   (windowing)
inline void mul(float* dst, const float* a, const float* b, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif
    for (; i < n; ++i) dst[i] = a[i] * b[i];
}

// dst[i] += a[i] * b[i]  (overlap-add with synthesis window)
inline void mul_add(float* dst, const float* a, const float* b, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(dst + i, fmadd(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), _mm256_loadu_ps(dst + i)));
#endif
    for (; i < n; ++i) dst[i] += a[i] * b[i];
}

// max |x[i]|
inline float peak_abs(const float* x, std::size_t n) noexcept {
    std::size_t i = 0;
    float m = 0.0f;
#if HC_AVX2
    const __m256 signMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    __m256 vm = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) vm = _mm256_max_ps(vm, _mm256_and_ps(_mm256_loadu_ps(x + i), signMask));
    m = hmax(vm);
#endif
    for (; i < n; ++i) m = std::fmax(m, std::fabs(x[i]));
    return m;
}

// Σ x[i]² accumulated in double (loudness integration needs the headroom).
inline double sum_squares(const float* x, std::size_t n) noexcept {
    std::size_t i = 0;
    double s = 0.0;
#if HC_AVX2
    __m256d acc0 = _mm256_setzero_pd(), acc1 = _mm256_setzero_pd();
    for (; i + 8 <= n; i += 8) {
        const __m256 v = _mm256_loadu_ps(x + i);
        const __m256d lo = _mm256_cvtps_pd(_mm256_castps256_ps128(v));
        const __m256d hi = _mm256_cvtps_pd(_mm256_extractf128_ps(v, 1));
        acc0 = _mm256_add_pd(acc0, _mm256_mul_pd(lo, lo));
        acc1 = _mm256_add_pd(acc1, _mm256_mul_pd(hi, hi));
    }
    alignas(32) double t[4];
    _mm256_store_pd(t, _mm256_add_pd(acc0, acc1));
    s = t[0] + t[1] + t[2] + t[3];
#endif
    for (; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return s;
}

// Σ a[i]*b[i] (FIR dot product)
inline float dot(const float* a, const float* b, std::size_t n) noexcept {
    std::size_t i = 0;
    float s = 0.0f;
#if HC_AVX2
    __m256 acc = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) acc = fmadd(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc);
    s = hsum(acc);
#endif
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}

// power[i] = re² + im²
inline void complex_power(float* power, const float* re, const float* im, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    for (; i + 8 <= n; i += 8) {
        const __m256 r = _mm256_loadu_ps(re + i), q = _mm256_loadu_ps(im + i);
        _mm256_storeu_ps(power + i, fmadd(r, r, _mm256_mul_ps(q, q)));
    }
#endif
    for (; i < n; ++i) power[i] = re[i] * re[i] + im[i] * im[i];
}

// re[i] *= g[i]; im[i] *= g[i]  — apply a real spectral gain mask
inline void apply_mask(float* re, float* im, const float* g, std::size_t n) noexcept {
    std::size_t i = 0;
#if HC_AVX2
    for (; i + 8 <= n; i += 8) {
        const __m256 vg = _mm256_loadu_ps(g + i);
        _mm256_storeu_ps(re + i, _mm256_mul_ps(_mm256_loadu_ps(re + i), vg));
        _mm256_storeu_ps(im + i, _mm256_mul_ps(_mm256_loadu_ps(im + i), vg));
    }
#endif
    for (; i < n; ++i) { re[i] *= g[i]; im[i] *= g[i]; }
}

// -----------------------------------------------------------------------------
// Denormal protection. Decaying IIR tails produce subnormal floats that can
// cost 100x per operation on x86 — the classic "CPU spikes when the song goes
// quiet" bug. Every real-time thread sets FTZ+DAZ at entry.
// -----------------------------------------------------------------------------
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(__SSE2__) || defined(_M_X64)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__SSE2__) || defined(_M_X64)
        _mm_setcsr(saved_);
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    unsigned int saved_ = 0;
};

inline float db_to_gain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gain_to_db(float g) noexcept { return 20.0f * std::log10(g > 1e-30f ? g : 1e-30f); }
inline double power_to_db(double p) noexcept { return 10.0 * std::log10(p > 1e-30 ? p : 1e-30); }

} // namespace hc::simd
