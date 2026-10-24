// =============================================================================
//  dsp/transient.hpp — HEM TRANSIENT: level-independent transient designer.
//
//  Detector on the linked stereo peak x = max(|L|, |R|):
//    fast  = follower(attack 0.2 ms, release 50 ms)
//    slow  = follower(attack 25 ms PUNCH / 8 ms SPEED, release 50 ms)
//    t     = 20·log10(fast / slow)   ≥ 0 at onsets, ≈ 0 elsewhere
//    long  = follower(attack 1 ms, release 400 ms)
//    short = follower(attack 1 ms, release 40 ms)
//    s     = 20·log10(long / short)  ≥ 0 in decaying tails
//  Gain (dB) = ATTACK · min(1, t / 6) + SUSTAIN · min(1, s / 12)
//  Because both measures are RATIOS of envelopes, the shaping is the same
//  at −30 dBFS or −3 dBFS — no threshold to set.
//  Gain is smoothed (0.3 ms) to avoid clicks; MIX blends dry/processed;
//  GAIN is the output level. Zero latency.
//  Display history (one point per 128 samples) goes through an SpscRing.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/lockfree.hpp"
#include "hc/dsp/simd.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hc {

struct TransientViz {
    float peak = 0.0f;     // input peak over the chunk (linear)
    float attackDb = 0.0f; // applied attack gain
    float sustainDb = 0.0f;
};

class TransientShaper {
public:
    void prepare(double sr) {
        sr_ = sr;
        aFast_ = c(0.2); rFast_ = c(50.0);
        rSlow_ = c(50.0);
        aSus_ = c(1.0); rLong_ = c(400.0); rShort_ = c(40.0);
        smooth_ = c(0.3);
        c25_ = c(25.0); c8_ = c(8.0);
        reset();
    }
    void reset() noexcept { fast_ = slow_ = lng_ = shrt_ = 1e-6f; g_ = 1.0f; chunkN_ = 0; chunkPk_ = 0; chunkA_ = chunkS_ = 0; }

    // attackDb/sustainDb in −15..+15, speed: 0 PUNCH, 1 SPEED, mix 0..1, outDb.
    void process(AudioBlock& b, float attackDb, float sustainDb, int speed, float mix, float outDb,
                 SpscRing<TransientViz, 2048>* viz) noexcept {
        const float aSlow = speed ? c8_ : c25_;
        const float out = simd::db_to_gain(outDb);
        float* L = b.ch[0];
        float* R = b.numChannels > 1 ? b.ch[1] : b.ch[0];
        for (std::uint32_t i = 0; i < b.numFrames; ++i) {
            const float x = std::max(std::fabs(L[i]), std::fabs(R[i])) + 1e-9f;
            fast_ = x + (fast_ - x) * (x > fast_ ? aFast_ : rFast_);
            slow_ = x + (slow_ - x) * (x > slow_ ? aSlow : rSlow_);
            lng_ = x + (lng_ - x) * (x > lng_ ? aSus_ : rLong_);
            shrt_ = x + (shrt_ - x) * (x > shrt_ ? aSus_ : rShort_);
            const float t = 20.0f * std::log10(fast_ / slow_);
            const float s = 20.0f * std::log10(lng_ / shrt_);
            const float aDb = attackDb * std::clamp(t / 6.0f, 0.0f, 1.0f);
            const float sDb = sustainDb * std::clamp(s / 12.0f, 0.0f, 1.0f);
            const float target = simd::db_to_gain(aDb + sDb);
            g_ = target + (g_ - target) * smooth_;
            const float k = (mix * g_ + (1.0f - mix)) * out;
            L[i] *= k;
            if (b.numChannels > 1) R[i] *= k;
            // display decimation
            chunkPk_ = std::max(chunkPk_, x);
            if (std::fabs(aDb) > std::fabs(chunkA_)) chunkA_ = aDb;
            if (std::fabs(sDb) > std::fabs(chunkS_)) chunkS_ = sDb;
            if (++chunkN_ == 128) {
                if (viz) viz->push(TransientViz{chunkPk_, chunkA_, chunkS_});
                chunkN_ = 0; chunkPk_ = 0; chunkA_ = chunkS_ = 0;
            }
        }
        lastAttack_ = chunkA_; lastSustain_ = chunkS_;
    }
    bool active() const noexcept { return std::fabs(lastAttack_) > 0.5f || std::fabs(lastSustain_) > 0.5f; }

private:
    float c(double ms) const { return static_cast<float>(std::exp(-1.0 / (sr_ * ms * 0.001))); }
    double sr_ = 48000.0;
    float aFast_ = 0, rFast_ = 0, rSlow_ = 0, aSus_ = 0, rLong_ = 0, rShort_ = 0, smooth_ = 0, c25_ = 0, c8_ = 0;
    float fast_ = 0, slow_ = 0, lng_ = 0, shrt_ = 0, g_ = 1.0f;
    std::uint32_t chunkN_ = 0;
    float chunkPk_ = 0, chunkA_ = 0, chunkS_ = 0, lastAttack_ = 0, lastSustain_ = 0;
};

} // namespace hc
