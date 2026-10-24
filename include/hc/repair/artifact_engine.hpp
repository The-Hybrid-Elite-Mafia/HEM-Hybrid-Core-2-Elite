// =============================================================================
//  repair/artifact_engine.hpp — HF Artifact Destroyer Engine.
//
//  WHAT GENERATIVE AUDIO GETS WRONG (and what this engine targets)
//  ---------------------------------------------------------------
//  Neural codecs/vocoders reconstruct the top octaves from a compressed latent.
//  The result is high-band energy whose magnitude JITTERS frame to frame and
//  whose phase is weakly tied between channels. Ears hear that as:
//    • "swirl / watery / underwater" cymbals and hats   → spectral jitter
//    • harsh, over-bright synthetic "s/sh/t" on vocals   → sibilance excess
//    • phasey, smeared stereo top end that collapses
//      oddly in mono                                    → incoherent side HF
//  Three cooperating modules, all running on the SAME linked stereo STFT frame
//  (N = 2048 @ 44.1/48 k, 75 % overlap, sqrt-Hann — see stft.hpp):
//
//  1) SHIMMER SMOOTHER (bins above `splitHz`)
//     Track a per-bin envelope E[k] in dB with a ~120 ms one-pole.
//     Deviation d[k] = P[k]dB − E[k]. Real HF content (a hat decay, a
//     cymbal ride) follows its envelope; codec jitter shows up as rapid
//     positive spikes above it. Gain target: −amount·max(0, d − tol), floored.
//     Only spikes are pulled DOWN — we never boost, so no new artefacts.
//     Transient guard: broadband positive spectral flux over 2–12 kHz above
//     a threshold marks a real hit (snare/hat onset) and releases all
//     reduction for that frame, so drums keep their snap.
//     Anti-musical-noise: per-bin gains are smoothed across 3 neighbouring
//     bins and in time (instant attack, 10 ms release), and the 4-frame
//     overlap-add further averages every gain decision.
//     Energy-preserving makeup (500 ms per bin) keeps long-term HF level, so
//     the module removes jitter without darkening the top end.
//
//  2) SPECTRAL DE-ESSER (4.5–10 kHz vs 300 Hz–3 kHz body)
//     ratio = 10·log10(E_sib / E_body). When ratio > threshold, reduction =
//     min(excess·amount, 12 dB), attack ~1 frame / release ~60 ms. Applied
//     per bin weighted by how far each bin sits above the band's local
//     spectral envelope — the harsh peaks take the cut, the air between
//     them is left alone. That's the difference vs. a broadband de-esser.
//
//  3) STEREO PHASE REPAIR (bins above `splitHz`)
//     Smoothed cross-spectra (~100 ms): Sxx, Syy, Sxy = ⟨L·R*⟩.
//     Magnitude-squared coherence γ² = |Sxy|² / (Sxx·Syy) ∈ [0,1].
//     Work in M/S: M = (L+R)/2, S = (L−R)/2. Side gain g_s = lerp(1, γ², amount).
//     Incoherent (phasey) HF side energy is reduced; coherent width is kept.
//     TRADE-OFF (stated plainly): genuinely decorrelated HF — wide reverb
//     tails, stereo shakers — also has low γ². Keep `phaseAmount` modest on
//     reverb-heavy stems, and A/B in mono.
//
//  NOT IMPLEMENTED HERE ON PURPOSE: stripping AI-provenance watermarks or
//  fingerprints. See the blueprint doc, §7.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/lockfree.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/dsp/stft.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>

namespace hc {

struct ArtifactSettings {
    AtomicParam shimmerAmount{0.6f, 0.0f, 1.0f};   // 0 = off
    AtomicParam shimmerTolDb{1.5f, 0.0f, 12.0f};   // dB above envelope tolerated
    AtomicParam sibilanceAmount{0.5f, 0.0f, 1.0f};
    AtomicParam sibilanceThreshDb{0.0f, -30.0f, 12.0f};  // E_sib/E_body threshold
    AtomicParam phaseAmount{0.3f, 0.0f, 1.0f};
    AtomicParam splitHz{7000.0f, 2000.0f, 16000.0f};
    AtomicParam floorDb{-15.0f, -40.0f, 0.0f};      // maximum per-bin reduction
};

// Diagnostics published to the UI / reports (relaxed atomics, audio writes).
struct ArtifactTelemetry {
    std::atomic<float> hfJitterDb{0};        // mean |d| over live HF bins (pre-repair)
    std::atomic<float> hfLiveFraction{0};    // share of HF bins above the −90 dB gate
    std::atomic<float> shimmerCutDb{0};      // long-term HF energy change, dB (≈0 = tonal balance kept)
    std::atomic<float> sibilanceCutDb{0};    // current de-ess reduction
    std::atomic<float> hfCoherence{1};       // mean γ² above split
    std::atomic<std::uint32_t> transientFrames{0};
    std::atomic<std::uint32_t> sibilantFrames{0};
    std::atomic<std::uint32_t> frames{0};
};

class ArtifactKernel final : public ISpectralKernel {
public:
    ArtifactKernel(ArtifactSettings& s, ArtifactTelemetry& t) : s_(s), t_(t) {}

    void prepare(double sr, std::size_t fftSize, std::size_t hop) {
        bins_ = fftSize / 2 + 1;
        // Reference: bin power of a full-scale sine through the sqrt-Hann
        // analysis window, |X| = Σw/2. Gates are set relative to it so the
        // engine never "repairs" dither / quantisation noise in empty bands.
        double wsum = 0.0;
        for (std::size_t i = 0; i < fftSize; ++i)
            wsum += std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(fftSize)));
        const double ref = (wsum / 2.0) * (wsum / 2.0);
        binGatePow_ = static_cast<float>(ref * 1e-9);    // −90 dB re full-scale sine
        sibGatePow_ = static_cast<float>(ref * 1e-8);    // −80 dB mean level in the ess band
        const double fps = sr / static_cast<double>(hop);
        auto coef = [&](double ms) { return static_cast<float>(std::exp(-1.0 / (fps * ms * 0.001))); };
        envCoef_ = coef(120.0);
        gainRel_ = coef(10.0);   // swept 10–40 ms: 10 ms gives the most jitter reduction (see blueprint §6.1)
        sibAtt_ = coef(5.0);
        sibRel_ = coef(60.0);
        xCoef_ = coef(100.0);
        ltCoef_ = coef(500.0);
        for (auto& g : gHist_) { g.resize(bins_); for (std::size_t k = 0; k < bins_; ++k) g[k] = 1.0f; }
        gHistPos_ = 0;
        for (auto* b : {&envDb_, &prevDb_, &shimGainDb_, &rawGain_, &mask_, &sxx_, &syy_, &sxyRe_, &sxyIm_, &midPow_, &sidePow_, &ltIn_, &ltOut_, &makeupDb_})
            b->resize(bins_);
        reset();
    }
    void reset() noexcept {
        for (std::size_t k = 0; k < bins_; ++k) {
            envDb_[k] = -120.0f; prevDb_[k] = -120.0f; shimGainDb_[k] = 0.0f;
            sxx_[k] = syy_[k] = 1e-12f; sxyRe_[k] = 1e-12f; sxyIm_[k] = 0.0f;
            ltIn_[k] = ltOut_[k] = 0.0f;
            for (auto& g : gHist_) g[k] = 1.0f;
        }
        sibGainDb_ = 0.0f;
    }

    void processFrame(SpectralFrame& f) noexcept override {
        const bool stereo = f.numChannels > 1;
        float* Lr = f.re[0]; float* Li = f.im[0];
        float* Rr = stereo ? f.re[1] : f.re[0];
        float* Ri = stereo ? f.im[1] : f.im[0];
        const std::size_t B = f.bins;
        auto binOf = [&](float hz) { return std::min(B - 1, static_cast<std::size_t>(hz / f.binHz)); };

        const float shimAmt = s_.shimmerAmount.target(), shimTol = s_.shimmerTolDb.target();
        const float sibAmt = s_.sibilanceAmount.target(), sibThr = s_.sibilanceThreshDb.target();
        const float phAmt = s_.phaseAmount.target(), floorDb = s_.floorDb.target();
        const std::size_t kSplit = binOf(s_.splitHz.target());

        // --- Mid/side power ---------------------------------------------------
        for (std::size_t k = 0; k < B; ++k) {
            const float mr = 0.5f * (Lr[k] + Rr[k]), mi = 0.5f * (Li[k] + Ri[k]);
            const float sr = 0.5f * (Lr[k] - Rr[k]), si = 0.5f * (Li[k] - Ri[k]);
            midPow_[k] = mr * mr + mi * mi;
            sidePow_[k] = sr * sr + si * si;
        }

        // --- Transient guard: positive spectral flux 2–12 kHz -----------------
        const std::size_t f0 = binOf(2000.0f), f1 = binOf(12000.0f);
        double flux = 0.0;
        for (std::size_t k = f0; k < f1; ++k) {
            const float db = static_cast<float>(simd::power_to_db(midPow_[k] + sidePow_[k] + 1e-20));
            flux += std::max(0.0f, db - prevDb_[k]);
            prevDb_[k] = db;
        }
        flux /= static_cast<double>(std::max<std::size_t>(1, f1 - f0));
        const bool transient = flux > 6.0;          // mean +6 dB rise across the band in one hop
        if (transient) t_.transientFrames.fetch_add(1, std::memory_order_relaxed);

        // --- 1) Shimmer smoother ---------------------------------------------
        double jitterAcc = 0.0, cutAcc = 0.0;
        std::size_t liveBins = 0;
        for (std::size_t k = 0; k < B; ++k) rawGain_[k] = 0.0f;
        for (std::size_t k = kSplit; k < B; ++k) {
            const float p = midPow_[k] + sidePow_[k];
            const float db = static_cast<float>(simd::power_to_db(p + 1e-20));
            const float d = db - envDb_[k];
            envDb_[k] = db + (envDb_[k] - db) * envCoef_;
            if (p < binGatePow_) continue;                 // below −90 dB: leave it alone
            ++liveBins;
            jitterAcc += std::fabs(d);
            rawGain_[k] = transient ? 0.0f : std::max(floorDb, -shimAmt * std::max(0.0f, d - shimTol));
        }
        // 3-bin frequency smoothing + attack/release in time (dB domain).
        for (std::size_t k = kSplit; k < B; ++k) {
            const float a = rawGain_[k > kSplit ? k - 1 : k], c = rawGain_[k + 1 < B ? k + 1 : k];
            const float target = 0.25f * a + 0.5f * rawGain_[k] + 0.25f * c;
            float& g = shimGainDb_[k];
            g = (target < g) ? target : target + (g - target) * gainRel_;
            cutAcc += g;
        }
        for (std::size_t k = 0; k < kSplit; ++k) shimGainDb_[k] *= gainRel_;   // decay to 0 dB if split moves up

        // Energy-preserving makeup (~500 ms): the smoother should flatten the
        // jitter, not darken the top. Long-term in/out power per bin gives a
        // makeup of sqrt(in/out), capped at +6 dB, so average HF energy (the
        // tonal balance you mixed for) is preserved while the frame-to-frame
        // spikes stay flattened.
        // Out-power is measured with the smoother gain only (no makeup), so the
        // makeup term never feeds back into its own estimate. Crucially, with
        // 75 % overlap every output sample is the window-weighted SUM of 4
        // frames, so the gain the listener actually gets is ≈ the mean of the
        // last 4 frame gains (amplitude), not the current one. Estimating out
        // power from that OLA-effective gain is what makes the makeup land
        // on target (using the per-frame gain under-compensates by ~2 dB on
        // heavily jittered noise — measured).
        gHistPos_ = (gHistPos_ + 1) % kOlaFrames;
        float* gNow = gHist_[gHistPos_].data();
        for (std::size_t k = 0; k < B; ++k) makeupDb_[k] = 0.0f;
        double eIn = 0.0, eOut = 0.0;
        for (std::size_t k = kSplit; k < B; ++k) {
            const float p = midPow_[k] + sidePow_[k];
            gNow[k] = simd::db_to_gain(shimGainDb_[k]);
            float gEff = 0.0f;
            for (std::size_t h = 0; h < kOlaFrames; ++h) gEff += gHist_[h][k];
            gEff *= 1.0f / static_cast<float>(kOlaFrames);
            ltIn_[k] = p + (ltIn_[k] - p) * ltCoef_;
            const float po = p * gEff * gEff;
            ltOut_[k] = po + (ltOut_[k] - po) * ltCoef_;
            if (ltIn_[k] > binGatePow_ && ltOut_[k] > 0.0f) {
                // Amplitude makeup in dB == power ratio in dB (sqrt in linear = same dB).
                makeupDb_[k] = std::clamp(static_cast<float>(simd::power_to_db(ltIn_[k] / ltOut_[k])), 0.0f, 6.0f);
                eIn += ltIn_[k];
                eOut += ltOut_[k] * simd::db_to_gain(2.0f * makeupDb_[k]);
            }
        }
        // Telemetry: long-term HF ENERGY change (dB) after smoothing + makeup.
        cutAcc = eIn > 0.0 ? simd::power_to_db(eOut / eIn) * static_cast<double>(std::max<std::size_t>(1, B - kSplit)) : 0.0;

        // --- 2) Spectral de-esser --------------------------------------------
        const std::size_t s0 = binOf(4500.0f), s1 = binOf(10000.0f), b0 = binOf(300.0f), b1 = binOf(3000.0f);
        double eSib = 0.0, eBody = 0.0;
        for (std::size_t k = s0; k < s1; ++k) eSib += midPow_[k];
        for (std::size_t k = b0; k < b1; ++k) eBody += midPow_[k];
        const float ratioDb = static_cast<float>(simd::power_to_db(eSib + 1e-20) - simd::power_to_db(eBody + 1e-20));
        const bool sibAudible = eSib / static_cast<double>(std::max<std::size_t>(1, s1 - s0)) > sibGatePow_;
        const float sibTarget = sibAudible ? -std::min(12.0f, std::max(0.0f, ratioDb - sibThr) * sibAmt * 2.0f) : 0.0f;
        sibGainDb_ = sibTarget + (sibGainDb_ - sibTarget) * (sibTarget < sibGainDb_ ? sibAtt_ : sibRel_);
        if (sibTarget < -0.5f) t_.sibilantFrames.fetch_add(1, std::memory_order_relaxed);
        const float sibBandMeanDb = static_cast<float>(simd::power_to_db(eSib / static_cast<double>(std::max<std::size_t>(1, s1 - s0)) + 1e-20));

        // --- Build the final magnitude mask -----------------------------------
        for (std::size_t k = 0; k < B; ++k) {
            float gDb = shimGainDb_[k] + makeupDb_[k];
            if (k >= s0 && k < s1 && sibGainDb_ < 0.0f) {
                // Peaks above the band mean take the full cut; others half.
                const float above = static_cast<float>(simd::power_to_db(midPow_[k] + 1e-20)) - sibBandMeanDb;
                const float w = above > 0.0f ? 1.0f : 0.5f;
                gDb += sibGainDb_ * w;
            }
            mask_[k] = simd::db_to_gain(std::max(gDb, floorDb - 12.0f));
        }

        // --- 3) Stereo phase repair + resynthesis in M/S ----------------------
        double cohAcc = 0.0;
        for (std::size_t k = 0; k < B; ++k) {
            float gSide = 1.0f;
            if (stereo && k >= kSplit) {
                const float pxx = Lr[k] * Lr[k] + Li[k] * Li[k];
                const float pyy = Rr[k] * Rr[k] + Ri[k] * Ri[k];
                const float cr = Lr[k] * Rr[k] + Li[k] * Ri[k];    // Re{L·R*}
                const float ci = Li[k] * Rr[k] - Lr[k] * Ri[k];    // Im{L·R*}
                sxx_[k] = pxx + (sxx_[k] - pxx) * xCoef_;
                syy_[k] = pyy + (syy_[k] - pyy) * xCoef_;
                sxyRe_[k] = cr + (sxyRe_[k] - cr) * xCoef_;
                sxyIm_[k] = ci + (sxyIm_[k] - ci) * xCoef_;
                const float den = sxx_[k] * syy_[k] + 1e-30f;
                const float coh = std::min(1.0f, (sxyRe_[k] * sxyRe_[k] + sxyIm_[k] * sxyIm_[k]) / den);
                cohAcc += coh;
                gSide = std::max(simd::db_to_gain(floorDb), 1.0f + phAmt * (coh - 1.0f));
            }
            const float g = mask_[k];
            const float mr = 0.5f * (Lr[k] + Rr[k]) * g, mi = 0.5f * (Li[k] + Ri[k]) * g;
            const float sr = 0.5f * (Lr[k] - Rr[k]) * g * gSide, si = 0.5f * (Li[k] - Ri[k]) * g * gSide;
            Lr[k] = mr + sr; Li[k] = mi + si;
            if (stereo) { Rr[k] = mr - sr; Ri[k] = mi - si; }
        }

        // --- Telemetry ---------------------------------------------------------
        const double nHf = static_cast<double>(std::max<std::size_t>(1, B - kSplit));
        const double nLive = static_cast<double>(std::max<std::size_t>(1, liveBins));
        t_.hfJitterDb.store(static_cast<float>(jitterAcc / nLive), std::memory_order_relaxed);
        t_.hfLiveFraction.store(static_cast<float>(static_cast<double>(liveBins) / nHf), std::memory_order_relaxed);
        t_.shimmerCutDb.store(static_cast<float>(cutAcc / nHf), std::memory_order_relaxed);
        t_.sibilanceCutDb.store(sibGainDb_, std::memory_order_relaxed);
        if (stereo) t_.hfCoherence.store(static_cast<float>(cohAcc / nHf), std::memory_order_relaxed);
        t_.frames.fetch_add(1, std::memory_order_relaxed);
    }

private:
    ArtifactSettings& s_;
    ArtifactTelemetry& t_;
    std::size_t bins_ = 0;
    static constexpr std::size_t kOlaFrames = 4;   // = overlap factor of the STFT
    AlignedBuffer<float> gHist_[kOlaFrames];
    std::size_t gHistPos_ = 0;
    float binGatePow_ = 0, sibGatePow_ = 0, ltCoef_ = 0;
    float envCoef_ = 0, gainRel_ = 0, sibAtt_ = 0, sibRel_ = 0, xCoef_ = 0, sibGainDb_ = 0;
    AlignedBuffer<float> envDb_, prevDb_, shimGainDb_, rawGain_, mask_, sxx_, syy_, sxyRe_, sxyIm_, midPow_, sidePow_, ltIn_, ltOut_, makeupDb_;
};

} // namespace hc
