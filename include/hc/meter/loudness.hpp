// =============================================================================
//  meter/loudness.hpp — Advanced Metering Engine: ITU-R BS.1770-4 / EBU R128.
//
//  SIGNAL PATH (per channel)
//    1. K-weighting = stage 1 "pre-filter" (high shelf, ~+4 dB above ~1.7 kHz,
//       models head diffraction) → stage 2 "RLB" high-pass (~38 Hz).
//       Coefficients are DERIVED for any sample rate from the analog
//       prototype parameters (same derivation used by libebur128/pyloudnorm);
//       at 48 kHz they reproduce the BS.1770 table (checked in tests).
//    2. Mean square per 100 ms sub-block.
//    3. Loudness of a set of blocks: L = −0.691 + 10·log10( Σ_c G_c · z_c ),
//       G_L = G_R = 1.0.
//
//  MEASUREMENTS (EBU R128 / Tech 3341 / Tech 3342)
//    Momentary  M : 400 ms window, updated every 100 ms
//    Short-term S : 3 s window, updated every 100 ms
//    Integrated I : 400 ms blocks, 75% overlap, gated:
//                   absolute gate −70 LUFS, then relative gate −10 LU below
//                   the mean of the absolute-gated blocks.
//    LRA          : short-term values, abs gate −70, rel gate −20 LU,
//                   LRA = P95 − P10.
//
//  UNBOUNDED PROGRAMME LENGTH, FIXED MEMORY: gating blocks go into a
//  histogram (0.01 LU bins, −70…+10 LUFS) storing count AND summed energy per
//  bin, so the integrated mean is exact; only the relative-gate threshold is
//  quantised to 0.01 LU. Every operation on the audio thread is O(1) except a
//  8,000-bin scan at 10 Hz (~80k adds/sec — negligible).
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/dsp/biquad.hpp"
#include "hc/dsp/dynamics.hpp"
#include "hc/dsp/simd.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace hc {

// K-weighting coefficients for any sample rate.
inline void kWeightingCoeffs(double fs, BiquadCoeffs& pre, BiquadCoeffs& rlb) noexcept {
    {   // Stage 1: high-shelf pre-filter
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        pre.b0 = (Vh + Vb * K / Q + K * K) / a0;
        pre.b1 = 2.0 * (K * K - Vh) / a0;
        pre.b2 = (Vh - Vb * K / Q + K * K) / a0;
        pre.a1 = 2.0 * (K * K - 1.0) / a0;
        pre.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {   // Stage 2: RLB high-pass
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        rlb.b0 = 1.0; rlb.b1 = -2.0; rlb.b2 = 1.0;
        rlb.a1 = 2.0 * (K * K - 1.0) / a0;
        rlb.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

// Fixed-memory loudness histogram for gating / percentiles.
class LoudnessHistogram {
public:
    static constexpr double kMin = -70.0, kMax = 10.0, kStep = 0.01;
    static constexpr std::size_t kBins = static_cast<std::size_t>((kMax - kMin) / kStep) + 1;

    void clear() noexcept { count_.assign(kBins, 0); energy_.assign(kBins, 0.0); total_ = 0; }
    LoudnessHistogram() { clear(); }

    // `energy` = weighted mean square of the block (linear). Blocks below the
    // −70 LUFS absolute gate are discarded here.
    void add(double energy) noexcept {
        const double lufs = -0.691 + simd::power_to_db(energy);
        if (lufs < kMin) return;
        const std::size_t b = bin(lufs);
        ++count_[b]; energy_[b] += energy; ++total_;
    }
    std::uint64_t total() const noexcept { return total_; }

    // Mean energy of blocks whose loudness >= threshold LUFS.
    double meanEnergyAbove(double thresholdLufs, std::uint64_t* nOut = nullptr) const noexcept {
        const std::size_t b0 = thresholdLufs <= kMin ? 0 : bin(thresholdLufs);
        double e = 0.0; std::uint64_t n = 0;
        for (std::size_t b = b0; b < kBins; ++b) { e += energy_[b]; n += count_[b]; }
        if (nOut) *nOut = n;
        return n ? e / static_cast<double>(n) : 0.0;
    }
    // Percentile (0..1) of loudness values >= thresholdLufs, returned in LUFS.
    double percentileAbove(double thresholdLufs, double q) const noexcept {
        const std::size_t b0 = thresholdLufs <= kMin ? 0 : bin(thresholdLufs);
        std::uint64_t n = 0;
        for (std::size_t b = b0; b < kBins; ++b) n += count_[b];
        if (n == 0) return -std::numeric_limits<double>::infinity();
        const double target = q * static_cast<double>(n - 1);
        std::uint64_t acc = 0;
        for (std::size_t b = b0; b < kBins; ++b) {
            acc += count_[b];
            if (static_cast<double>(acc) > target) return kMin + kStep * static_cast<double>(b);
        }
        return kMax;
    }

private:
    static std::size_t bin(double lufs) noexcept {
        double i = std::floor((lufs - kMin) / kStep + 1e-9);
        if (i < 0) i = 0;
        if (i > static_cast<double>(kBins - 1)) i = static_cast<double>(kBins - 1);
        return static_cast<std::size_t>(i);
    }
    std::vector<std::uint32_t> count_;
    std::vector<double> energy_;
    std::uint64_t total_ = 0;
};

struct LoudnessReading {
    float momentary = -INFINITY, shortTerm = -INFINITY, integrated = -INFINITY;
    float lra = 0.0f, maxMomentary = -INFINITY, maxShortTerm = -INFINITY;
    float truePeakDb = -INFINITY, samplePeakDb = -INFINITY;
    float plr = 0.0f;   // peak-to-loudness ratio: TP − I  (dynamic range of the master)
    float psr = 0.0f;   // peak-to-short-term ratio: TP(3 s) − S (punch/crest, live)
};

class LoudnessMeter {
public:
    void prepare(double sr) {
        sr_ = sr;
        BiquadCoeffs pre, rlb;
        kWeightingCoeffs(sr, pre, rlb);
        for (auto& f : pre_) f.setCoeffs(pre);
        for (auto& f : rlb_) f.setCoeffs(rlb);
        subBlockLen_ = static_cast<std::uint32_t>(std::lround(sr * 0.1));
        scratch_.resize(kMaxBlock);
        tp_.prepare(sr);
        reset();
    }

    void reset() {
        for (auto& f : pre_) f.reset();
        for (auto& f : rlb_) f.reset();
        sub_.fill(0.0); subTp_.fill(0.0f);
        subCount_ = 0; subPos_ = 0; acc_ = 0.0; accTp_ = 0.0f;
        gating_.clear(); lraHist_.clear();
        r_ = LoudnessReading{};
        tp_.reset();
        samplePeak_ = 0.0f; truePeak_ = 0.0f;
    }

    // RT. Must be called with ≤ kMaxBlock frames.
    void process(const AudioBlock& b) noexcept {
        std::uint32_t done = 0;
        while (done < b.numFrames) {
            const std::uint32_t n = std::min(b.numFrames - done, subBlockLen_ - subPos_);
            double e = 0.0;
            for (std::uint32_t c = 0; c < b.numChannels && c < kNumChannels; ++c) {
                const float* src = b.ch[c] + done;
                samplePeak_ = std::max(samplePeak_, simd::peak_abs(src, n));
                simd::copy(scratch_.data(), src, n);
                pre_[c].process(scratch_.data(), n);
                rlb_[c].process(scratch_.data(), n);
                e += simd::sum_squares(scratch_.data(), n);  // G_c = 1.0 for L/R
            }
            const float* R = b.numChannels > 1 ? b.ch[1] : b.ch[0];
            for (std::uint32_t i = 0; i < n; ++i)
                accTp_ = std::max(accTp_, tp_.pushFrame(b.ch[0][done + i], R[done + i]));
            acc_ += e;
            subPos_ += n; done += n;
            if (subPos_ == subBlockLen_) closeSubBlock();
        }
    }

    const LoudnessReading& reading() const noexcept { return r_; }

private:
    static constexpr std::size_t kHist = 30;   // 30 × 100 ms = 3 s

    void closeSubBlock() noexcept {
        const double ms = acc_ / static_cast<double>(subBlockLen_);
        sub_[subCount_ % kHist] = ms;
        subTp_[subCount_ % kHist] = accTp_;
        truePeak_ = std::max(truePeak_, accTp_);
        ++subCount_;
        acc_ = 0.0; accTp_ = 0.0f; subPos_ = 0;

        auto meanOf = [&](std::size_t k) {
            double s = 0.0;
            for (std::size_t i = 0; i < k; ++i) s += sub_[(subCount_ - 1 - i) % kHist];
            return s / static_cast<double>(k);
        };
        auto toLufs = [](double e) { return static_cast<float>(-0.691 + simd::power_to_db(e)); };

        if (subCount_ >= 4) {
            const double eM = meanOf(4);
            r_.momentary = toLufs(eM);
            r_.maxMomentary = std::max(r_.maxMomentary, r_.momentary);
            gating_.add(eM);                     // 400 ms block, 75 % overlap
            // Integrated: abs gate (−70, applied in add) → relative gate.
            const double ungated = gating_.meanEnergyAbove(-70.0);
            if (ungated > 0.0) {
                const double relGate = -0.691 + simd::power_to_db(ungated) - 10.0;
                const double gated = gating_.meanEnergyAbove(relGate);
                if (gated > 0.0) r_.integrated = toLufs(gated);
            }
        }
        if (subCount_ >= kHist) {
            const double eS = meanOf(kHist);
            r_.shortTerm = toLufs(eS);
            r_.maxShortTerm = std::max(r_.maxShortTerm, r_.shortTerm);
            lraHist_.add(eS);
            const double ungated = lraHist_.meanEnergyAbove(-70.0);
            if (ungated > 0.0) {
                const double relGate = -0.691 + simd::power_to_db(ungated) - 20.0;
                const double lo = lraHist_.percentileAbove(relGate, 0.10);
                const double hi = lraHist_.percentileAbove(relGate, 0.95);
                r_.lra = static_cast<float>(hi - lo);
            }
            float tp3 = 0.0f;
            for (float v : subTp_) tp3 = std::max(tp3, v);
            r_.psr = simd::gain_to_db(tp3) - r_.shortTerm;
        }
        r_.truePeakDb = simd::gain_to_db(truePeak_);
        r_.samplePeakDb = simd::gain_to_db(samplePeak_);
        if (std::isfinite(r_.integrated)) r_.plr = r_.truePeakDb - r_.integrated;
    }

    double sr_ = 48000.0;
    Biquad pre_[kNumChannels], rlb_[kNumChannels];
    TruePeakOversampler tp_;
    AlignedBuffer<float> scratch_;
    std::array<double, kHist> sub_{};
    std::array<float, kHist> subTp_{};
    std::uint64_t subCount_ = 0;
    std::uint32_t subBlockLen_ = 4800, subPos_ = 0;
    double acc_ = 0.0;
    float accTp_ = 0.0f, samplePeak_ = 0.0f, truePeak_ = 0.0f;
    LoudnessHistogram gating_, lraHist_;
    LoudnessReading r_{};
};

} // namespace hc
