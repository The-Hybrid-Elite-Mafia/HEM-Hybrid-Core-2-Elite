// =============================================================================
//  repair/resonance.hpp — HEM RESONANCE: dynamic harsh-resonance suppressor.
//
//  Generated audio often carries narrow, piercing peaks (ringing 2–6 kHz
//  "honk", whistly vocal formants, metallic hats) that move around. A static
//  EQ can't chase them. Per STFT frame this engine:
//    1. measures linked stereo power per bin, P[k] (dB)
//    2. builds a smooth spectral envelope E[k] = mean of P over ±1/6 octave
//    3. excess  x[k] = P[k] − E[k]  — a resonance sticks out of its neighbours
//    4. cut     c[k] = −min(REDUCTION, max(0, x − threshold)) · focus(k)
//         threshold = 12 − 0.1·SENSITIVITY  (0 % → 12 dB, 100 % → 2 dB)
//         focus     = raised-cosine window centred on FREQUENCY, BANDWIDTH
//                     octaves wide (only that region is treated)
//    5. smooths c across 3 bins and in time (attack/release from the mode
//       selector: SOFT / NORMAL / FAST)
//    6. COMPENSATION: per-bin long-term (500 ms) energy makeup. 0 % keeps
//       the average cut (acts like a dynamic + static EQ); 100 % restores the
//       long-term level so ONLY the moving peaks are removed.
//  The same gain is applied to L and R (no stereo image shift).
//  Display data (128 log bands, 20 Hz–20 kHz) is published through a
//  TripleBuffer for the faceplate screen.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/lockfree.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/dsp/stft.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace hc {

struct ResonanceSettings {
    AtomicParam sensitivity{50.0f, 0.0f, 100.0f};    // %
    AtomicParam frequency{3500.0f, 200.0f, 16000.0f}; // Hz, focus centre
    AtomicParam bandwidth{5.0f, 0.5f, 8.0f};          // octaves
    AtomicParam reduction{6.0f, 0.0f, 18.0f};         // dB, max cut
    AtomicParam compensation{50.0f, 0.0f, 100.0f};    // %
    AtomicParam mode{1.0f, 0.0f, 2.0f};               // 0 SOFT, 1 NORMAL, 2 FAST
};

struct ResonanceView {
    static constexpr std::size_t kBands = 128;
    float levelDb[kBands] = {};   // spectrum (dB re full-scale sine)
    float cutDb[kBands] = {};     // applied gain change, ≤ 0 mostly
};

class ResonanceKernel final : public ISpectralKernel {
public:
    explicit ResonanceKernel(ResonanceSettings& s) : s_(s) {}

    void prepare(double sr, std::size_t fftSize, std::size_t hop) {
        sr_ = sr; bins_ = fftSize / 2 + 1; fps_ = sr / static_cast<double>(hop);
        for (auto* b : {&db_, &pw_, &ps_, &prefix_, &raw_, &gain_, &ltIn_, &ltOut_, &lo_, &hi_}) b->resize(bins_ + 1);
        const double binHz = sr / static_cast<double>(fftSize);
        const double r = std::pow(2.0, 1.0 / 6.0);
        for (std::size_t k = 0; k < bins_; ++k) {
            const double lo = std::floor(static_cast<double>(k) / r), hi = std::ceil(static_cast<double>(k) * r);
            lo_[k] = static_cast<float>(std::max(0.0, std::min(lo, static_cast<double>(k) - 6.0)));
            hi_[k] = static_cast<float>(std::min(static_cast<double>(bins_ - 1), std::max(hi, static_cast<double>(k) + 6.0)));
        }
        double wsum = 0.0;
        for (std::size_t i = 0; i < fftSize; ++i)
            wsum += std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(fftSize)));
        ref_ = (wsum / 2.0) * (wsum / 2.0);
        gatePow_ = static_cast<float>(ref_ * 1e-9);
        gateDb_ = static_cast<float>(simd::power_to_db(ref_ * 1e-9));
        // Band edges for the display (log spaced 20 Hz – 20 kHz).
        for (std::size_t b = 0; b <= ResonanceView::kBands; ++b) {
            const double f = 20.0 * std::pow(1000.0, static_cast<double>(b) / ResonanceView::kBands);
            bandEdge_[b] = static_cast<std::uint32_t>(std::clamp(f / binHz, 1.0, static_cast<double>(bins_ - 1)));
        }
        binHz_ = binHz;
        ltCoef_ = coef(500.0);
        detCoef_ = coef(40.0);
        reset();
    }
    void reset() noexcept {
        for (std::size_t k = 0; k <= bins_; ++k) { gain_[k] = 0.0f; ltIn_[k] = ltOut_[k] = 0.0f; ps_[k] = 0.0f; }
    }

    void processFrame(SpectralFrame& f) noexcept override {
        const bool stereo = f.numChannels > 1;
        float* Lr = f.re[0]; float* Li = f.im[0];
        float* Rr = stereo ? f.re[1] : f.re[0];
        float* Ri = stereo ? f.im[1] : f.im[0];
        const std::size_t B = f.bins;
        const float sens = s_.sensitivity.target(), fc = s_.frequency.target(), bw = s_.bandwidth.target();
        const float red = s_.reduction.target(), comp = s_.compensation.target() * 0.01f;
        const int mode = static_cast<int>(std::lround(s_.mode.target()));
        const float att = coef(mode == 0 ? 25.0 : (mode == 1 ? 8.0 : 2.0));
        const float rel = coef(mode == 0 ? 220.0 : (mode == 1 ? 110.0 : 45.0));
        const float thr = 12.0f - 0.1f * sens;

        // 1–2. power; detection uses a time-smoothed copy (~30 ms) because a
        // resonance is SUSTAINED while noise bins flicker frame to frame.
        // The envelope is a linear-power mean (a dB mean sits ~2.5 dB under
        // the true noise level and would flag ordinary noise as resonant).
        prefix_[0] = 0.0f;
        for (std::size_t k = 0; k < B; ++k) {
            const float p = 0.5f * (Lr[k] * Lr[k] + Li[k] * Li[k] + Rr[k] * Rr[k] + Ri[k] * Ri[k]);
            pw_[k] = p;
            ps_[k] = p + (ps_[k] - p) * detCoef_;
            db_[k] = static_cast<float>(simd::power_to_db(static_cast<double>(ps_[k]) + 1e-20));
            prefix_[k + 1] = prefix_[k] + ps_[k];
        }
        // 3–4. excess → raw cut
        const float halfBw = 0.5f * bw;
        for (std::size_t k = 0; k < B; ++k) {
            raw_[k] = 0.0f;
            const double fHz = static_cast<double>(k) * binHz_;
            if (k == 0 || fHz < 20.0) continue;
            const float oct = static_cast<float>(std::log2(fHz / fc));
            if (std::fabs(oct) > halfBw) continue;
            const float w = 0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * oct / halfBw);
            const auto lo = static_cast<std::size_t>(lo_[k]), hi = static_cast<std::size_t>(hi_[k]);
            // Reference level = the neighbourhood EXCLUDING the bin's own
            // ±2-bin main lobe, so a strong ring can't raise its own reference.
            const std::size_t c0 = k >= 2 ? k - 2 : 0, c1 = std::min(B - 1, k + 2);
            const float own = prefix_[c1 + 1] - prefix_[c0];
            const float cnt = static_cast<float>((hi - lo + 1) - (c1 - c0 + 1));
            const float envPow = cnt > 0.0f ? (prefix_[hi + 1] - prefix_[lo] - own) / cnt : ps_[k];
            const float env = static_cast<float>(simd::power_to_db(static_cast<double>(envPow) + 1e-20));
            if (db_[k] < gateDb_) continue;                       // −90 dB gate: leave noise alone
            const float x = db_[k] - env;
            raw_[k] = -std::min(red, std::max(0.0f, x - thr)) * w;
        }
        // 5. smoothing in frequency (3-tap) and time (attack/release)
        for (std::size_t k = 0; k < B; ++k) {
            const float a = raw_[k > 0 ? k - 1 : k], c = raw_[k + 1 < B ? k + 1 : k];
            const float t = 0.25f * a + 0.5f * raw_[k] + 0.25f * c;
            float& g = gain_[k];
            g = t + (g - t) * (t < g ? att : rel);
        }
        // 6. compensation + apply
        for (std::size_t k = 0; k < B; ++k) {
            const float pin = pw_[k];
            const float gl = simd::db_to_gain(gain_[k]);
            ltIn_[k] = pin + (ltIn_[k] - pin) * ltCoef_;
            const float pout = pin * gl * gl;
            ltOut_[k] = pout + (ltOut_[k] - pout) * ltCoef_;
            float mk = 0.0f;
            if (comp > 0.0f && ltOut_[k] > gatePow_ && ltIn_[k] > 0.0f)
                mk = comp * std::clamp(static_cast<float>(simd::power_to_db(ltIn_[k] / ltOut_[k])), 0.0f, 6.0f);
            const float total = gain_[k] + mk;
            raw_[k] = total;                         // reuse as "applied" for the display
            const float g = simd::db_to_gain(total);
            Lr[k] *= g; Li[k] *= g;
            if (stereo) { Rr[k] *= g; Ri[k] *= g; }
        }
        publishView(B);
    }

    TripleBuffer<ResonanceView>& view() noexcept { return view_; }

private:
    float coef(double ms) const { return static_cast<float>(std::exp(-1.0 / (fps_ * ms * 0.001))); }

    void publishView(std::size_t B) noexcept {
        ResonanceView& v = view_.write_buffer();
        const float refDb = static_cast<float>(simd::power_to_db(ref_));
        for (std::size_t b = 0; b < ResonanceView::kBands; ++b) {
            std::uint32_t k0 = bandEdge_[b], k1 = std::max(bandEdge_[b + 1], k0 + 1);
            if (k1 > B) k1 = static_cast<std::uint32_t>(B);
            float lv = -200.0f, ct = 0.0f; int n = 0;
            for (std::uint32_t k = k0; k < k1; ++k) { lv = std::max(lv, db_[k]); ct += raw_[k]; ++n; }
            v.levelDb[b] = lv - refDb;
            v.cutDb[b] = n ? ct / static_cast<float>(n) : 0.0f;
        }
        view_.publish();
    }

    ResonanceSettings& s_;
    double sr_ = 48000.0, fps_ = 93.75, binHz_ = 23.4, ref_ = 1.0;
    std::size_t bins_ = 0;
    float ltCoef_ = 0.0f, detCoef_ = 0.0f, gatePow_ = 0.0f, gateDb_ = -200.0f;
    AlignedBuffer<float> db_, pw_, ps_, prefix_, raw_, gain_, ltIn_, ltOut_, lo_, hi_;
    std::uint32_t bandEdge_[ResonanceView::kBands + 1] = {};
    TripleBuffer<ResonanceView> view_;
};

} // namespace hc
