// =============================================================================
//  engine/stem_engine.hpp — Real-Time Audio Engine core: 13 stems → master.
//
//  PER CALLBACK (audio thread, the "conductor"):
//    1. Swap in any pending chains published by the UI (atomic exchange);
//       retired chains go to a graveyard ring for the UI thread to delete.
//       If any latency changed, recompute plugin delay compensation (PDC).
//    2. Fork: 13 stem jobs on the WorkerPool. Each job, fully independent:
//         read source @ playhead → stem chain → PDC delay → stem meters
//         → spectrum tap → (fader/pan ramps computed but not summed yet)
//    3. Join, then sum the 13 stems into the master bus on the conductor —
//       single-threaded, deterministic summing order (no atomics on audio).
//    4. Master chain → master meters → device buffer.
//
//  No allocation, no locks, no I/O anywhere in process().
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/config.hpp"
#include "hc/core/lockfree.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/engine/processor.hpp"
#include "hc/engine/worker_pool.hpp"
#include "hc/meter/loudness.hpp"
#include "hc/meter/spectrum.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

namespace hc {

struct StemMeterSnapshot {
    LoudnessReading loudness;
    float peakDb[kNumChannels] = {-INFINITY, -INFINITY};
    float faderDb = 0.0f;
    float tpDb[kNumChannels] = {-INFINITY, -INFINITY};   // per-channel true peak of the last block (master only)
    bool clipped = false;
};

struct StemSource {
    const float* ch[kNumChannels] = {};   // owned by the session loader, immutable while playing
    std::uint64_t length = 0;
};

class StemEngine {
public:
    static constexpr std::uint32_t kMaxPdc = 16384;
    static constexpr std::size_t kMaster = kNumStems;   // chain slot index for the master bus

    struct Stem {
        AudioBuffer buf;
        AlignedBuffer<float> mono;                 // tap scratch
        AlignedBuffer<float> pdc[kNumChannels];
        std::uint32_t pdcLen = 0, pdcPos = 0;
        ProcessorChain* chain = nullptr;
        std::atomic<ProcessorChain*> pending{nullptr};
        AtomicParam faderDb{0.0f, -96.0f, 12.0f};
        AtomicParam pan{0.0f, -1.0f, 1.0f};
        std::atomic<bool> mute{false}, solo{false};
        float lastGain[kNumChannels] = {1, 1};
        float blockGain[kNumChannels] = {1, 1};
        LoudnessMeter meter;
        TripleBuffer<StemMeterSnapshot> snapshot;
        SampleTap tap;
        StemSource source;
    };

    // NON-RT ------------------------------------------------------------------
    // StemEngine is large (13 sample taps ≈ 3.4 MB): always heap-allocate it.
    void prepare(double sr, std::uint32_t maxBlock, unsigned workers, WorkerPool::ThreadInit init = nullptr) {
        sr_ = sr; maxBlock_ = maxBlock;
        for (auto& s : stems_) {
            s.buf.prepare(maxBlock);
            s.mono.resize(maxBlock);
            for (auto& d : s.pdc) d.resize(kMaxPdc);
            s.faderDb.prepare(sr); s.pan.prepare(sr);
            s.meter.prepare(sr);
        }
        master_.prepare(maxBlock);
        masterMono_.resize(maxBlock);
        masterMeter_.prepare(sr);
        tpL_.prepare(sr); tpR_.prepare(sr);
        pool_.start(workers, init);
    }
    void shutdown() { pool_.stop(); collectGarbage(); }

    // Publish a fully prepared chain for stem i (0..12) or kMaster. Returns
    // false if a previous publish has not been picked up yet (retry later).
    bool publishChain(std::size_t slot, std::unique_ptr<ProcessorChain> c) {
        c->prepare(sr_, maxBlock_);
        std::atomic<ProcessorChain*>& p = slot == kMaster ? masterPending_ : stems_[slot].pending;
        ProcessorChain* expected = nullptr;
        ProcessorChain* raw = c.get();
        if (!p.compare_exchange_strong(expected, raw, std::memory_order_acq_rel)) return false;
        c.release();
        return true;
    }
    // Call periodically from the UI thread.
    void collectGarbage() { ProcessorChain* c; while (graveyard_.pop(c)) delete c; }
    ~StemEngine() {
        pool_.stop(); collectGarbage();
        for (auto& s : stems_) { delete s.chain; delete s.pending.load(); }
        delete masterChain_; delete masterPending_.load();
    }

    // Must only be called while stopped (the loader owns the memory).
    void setSource(std::size_t stem, const float* L, const float* R, std::uint64_t len) noexcept {
        stems_[stem].source = StemSource{{L, R ? R : L}, len};
        length_ = 0;
        for (auto& s : stems_) length_ = std::max(length_, s.source.length);
    }

    // ANY thread --------------------------------------------------------------
    void play() noexcept { playing_.store(true, std::memory_order_release); }
    void stop() noexcept { playing_.store(false, std::memory_order_release); }
    void seek(std::uint64_t frame) noexcept { seekTarget_.store(frame, std::memory_order_release); }
    // Loop [start, end) in source frames. The wrap is sample-accurate inside the block (no gap, no click;
    // plugin chains keep running across it). Applies only while the playhead is before `end`, so playing
    // from past the loop just plays on. Start and end are packed in one atomic so the audio thread never
    // sees half an update.
    void setLoop(bool on, std::uint64_t start, std::uint64_t end) noexcept {
        if (end <= start) on = false;
        start = std::min<std::uint64_t>(start, 0xFFFFFFFFull);
        end = std::min<std::uint64_t>(end, 0xFFFFFFFFull);
        loopRange_.store((start << 32) | end, std::memory_order_relaxed);
        loopOn_.store(on, std::memory_order_release);
    }
    bool loopOn() const noexcept { return loopOn_.load(std::memory_order_acquire); }
    std::uint64_t loopStart() const noexcept { return loopRange_.load(std::memory_order_relaxed) >> 32; }
    std::uint64_t loopEnd() const noexcept { return loopRange_.load(std::memory_order_relaxed) & 0xFFFFFFFFull; }
    bool isPlaying() const noexcept { return playing_.load(std::memory_order_acquire); }
    void requestMeterReset() noexcept { meterResetReq_.store(true, std::memory_order_release); }
    std::uint64_t playhead() const noexcept { return playhead_.load(std::memory_order_acquire); }
    std::uint64_t length() const noexcept { return length_; }
    std::uint32_t totalLatency() const noexcept { return totalLatency_.load(std::memory_order_relaxed); }
    Stem& stem(std::size_t i) noexcept { return stems_[i]; }
    TripleBuffer<StemMeterSnapshot>& masterSnapshot() noexcept { return masterSnap_; }
    SampleTap& masterTap() noexcept { return masterTap_; }
    double sampleRate() const noexcept { return sr_; }
    const LoudnessMeter& masterMeter() const noexcept { return masterMeter_; }   // offline use only

    // RT ------------------------------------------------------------------------
    // Renders n frames (≤ maxBlock) into outL/outR.
    void process(float* outL, float* outR, std::uint32_t n) noexcept {
        swapChains();
        const std::uint64_t seek = seekTarget_.exchange(UINT64_MAX, std::memory_order_acq_rel);
        if (seek != UINT64_MAX) { playhead_.store(seek, std::memory_order_release); resetChains(); resetMeters(); }
        if (meterResetReq_.exchange(false, std::memory_order_acq_rel)) resetMeters();

        block_ = n;
        live_ = playing_.load(std::memory_order_acquire);
        pos_ = live_ ? playhead_.load(std::memory_order_relaxed) : 0;
        {
            const std::uint64_t lr = loopRange_.load(std::memory_order_relaxed);
            lpS_ = lr >> 32; lpE_ = lr & 0xFFFFFFFFull;
            lpOn_ = live_ && loopOn_.load(std::memory_order_acquire) && lpE_ > lpS_ && pos_ < lpE_;
        }
        anySolo_ = false;
        for (auto& s : stems_) anySolo_ |= s.solo.load(std::memory_order_relaxed);

        pool_.run(&StemEngine::stemJob, this, kNumStems);

        // Deterministic summing on the conductor thread.
        master_.zero();
        for (auto& s : stems_) {
            for (std::size_t c = 0; c < kNumChannels; ++c) {
                simd::mac_ramp(master_.ch[c].data(), s.buf.ch[c].data(), s.lastGain[c], s.blockGain[c], n);
                s.lastGain[c] = s.blockGain[c];
            }
        }

        AudioBlock mb{{master_.ch[0].data(), master_.ch[1].data()}, kNumChannels, n};
        if (masterChain_) masterChain_->process(mb);
        masterMeter_.process(mb);
        {   // per-channel true peak for the master's L/R meters
            float pl = 0.0f, pr = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                pl = std::max(pl, tpL_.pushFrame(mb.ch[0][i], mb.ch[0][i]));
                pr = std::max(pr, tpR_.pushFrame(mb.ch[1][i], mb.ch[1][i]));
            }
            StemMeterSnapshot& w = masterSnap_.write_buffer();
            w.tpDb[0] = simd::gain_to_db(pl); w.tpDb[1] = simd::gain_to_db(pr);
        }
        publishMeter(masterSnap_, masterMeter_, mb, 0.0f);
        // Master spectrum tap (mid).
        float* mono = masterMono_.data();
        for (std::uint32_t i = 0; i < n; ++i) mono[i] = 0.5f * (mb.ch[0][i] + mb.ch[1][i]);
        masterTap_.push_n(mono, n);

        simd::copy(outL, mb.ch[0], n);
        simd::copy(outR, mb.ch[1], n);
        if (live_) playhead_.store(advance(pos_, n), std::memory_order_release);
    }

private:
    static void stemJob(void* ctx, std::size_t i) noexcept {
        simd::ScopedFlushDenormals ftz;
        static_cast<StemEngine*>(ctx)->processStem(i);
    }

    // Where the playhead lands after n frames from p (same run logic as the source read).
    std::uint64_t advance(std::uint64_t p, std::uint32_t n) const noexcept {
        if (!lpOn_) return p + n;
        std::uint32_t done = 0;
        while (done < n) {
            std::uint32_t run = n - done;
            if (p < lpE_) run = static_cast<std::uint32_t>(std::min<std::uint64_t>(run, lpE_ - p));
            done += run; p += run;
            if (p >= lpE_) p = lpS_;
        }
        return p;
    }

    void processStem(std::size_t idx) noexcept {
        Stem& s = stems_[idx];
        const std::uint32_t n = block_;
        // 1. Source read (zero past the end / when stopped).
        for (std::size_t c = 0; c < kNumChannels; ++c) {
            float* dst = s.buf.ch[c].data();
            const float* src = s.source.ch[c];
            if (!live_ || !src) { simd::clear(dst, n); continue; }
            // Read in runs: one run normally, more when the loop end falls inside this block.
            std::uint64_t p = pos_;
            std::uint32_t done = 0;
            while (done < n) {
                std::uint32_t run = n - done;
                if (lpOn_ && p < lpE_) run = static_cast<std::uint32_t>(std::min<std::uint64_t>(run, lpE_ - p));
                std::uint32_t avail = 0;
                if (p < s.source.length) avail = static_cast<std::uint32_t>(std::min<std::uint64_t>(run, s.source.length - p));
                if (avail) simd::copy(dst + done, src + p, avail);
                if (avail < run) simd::clear(dst + done + avail, run - avail);
                done += run;
                p += run;
                if (lpOn_ && p >= lpE_) p = lpS_;
            }
        }
        AudioBlock b{{s.buf.ch[0].data(), s.buf.ch[1].data()}, kNumChannels, n};
        // 2. Chain.
        if (s.chain) s.chain->process(b);
        // 3. PDC delay so every stem arrives at the master time-aligned.
        if (s.pdcLen) {
            for (std::size_t c = 0; c < kNumChannels; ++c) {
                float* d = s.pdc[c].data(); float* x = b.ch[c];
                std::uint32_t p = s.pdcPos;
                for (std::uint32_t i = 0; i < n; ++i) {
                    const float y = d[p]; d[p] = x[i]; x[i] = y;
                    if (++p == s.pdcLen) p = 0;
                }
            }
            s.pdcPos = static_cast<std::uint32_t>((s.pdcPos + n) % s.pdcLen);
        }
        // 4. Fader / pan (balance law) → block-end gains for ramped summing.
        const float fader = s.faderDb.advance(n);
        const float pan = s.pan.advance(n);
        const bool audible = !s.mute.load(std::memory_order_relaxed) && (!anySolo_ || s.solo.load(std::memory_order_relaxed));
        const float g = audible ? simd::db_to_gain(fader) : 0.0f;
        s.blockGain[0] = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
        s.blockGain[1] = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
        // 5. Meters (post-chain, pre-fader — what the stem IS, not where it sits).
        s.meter.process(b);
        publishMeter(s.snapshot, s.meter, b, fader);
        // 6. Spectrum tap (mid signal) for the analysis thread.
        float* mono = s.mono.data();
        for (std::uint32_t i = 0; i < n; ++i) mono[i] = 0.5f * (b.ch[0][i] + b.ch[1][i]);
        s.tap.push_n(mono, n);   // drops if analysis thread is behind — never blocks
    }

    static void publishMeter(TripleBuffer<StemMeterSnapshot>& tb, const LoudnessMeter& m, const AudioBlock& b, float faderDb) noexcept {
        StemMeterSnapshot& w = tb.write_buffer();
        w.loudness = m.reading();
        for (std::uint32_t c = 0; c < kNumChannels; ++c) {
            const float pk = simd::peak_abs(b.ch[c], b.numFrames);
            w.peakDb[c] = simd::gain_to_db(pk);
            w.clipped = w.clipped || pk >= 1.0f;
        }
        w.faderDb = faderDb;
        tb.publish();
    }

    void swapChains() noexcept {
        bool changed = false;
        for (auto& s : stems_) {
            if (ProcessorChain* p = s.pending.exchange(nullptr, std::memory_order_acq_rel)) {
                if (s.chain && !graveyard_.push(s.chain)) { /* graveyard full: leak-safe fallback = keep old alive */ s.pending.store(p); continue; }
                s.chain = p; changed = true;
            }
        }
        if (ProcessorChain* p = masterPending_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (!masterChain_ || graveyard_.push(masterChain_)) { masterChain_ = p; changed = true; }
            else masterPending_.store(p);
        }
        if (changed) recomputePdc();
    }

    void recomputePdc() noexcept {
        std::uint32_t maxLat = 0;
        for (auto& s : stems_) maxLat = std::max(maxLat, s.chain ? s.chain->latency() : 0u);
        for (auto& s : stems_) {
            const std::uint32_t want = std::min<std::uint32_t>(kMaxPdc, maxLat - (s.chain ? s.chain->latency() : 0u));
            if (want != s.pdcLen) {
                s.pdcLen = want; s.pdcPos = 0;
                for (auto& d : s.pdc) simd::clear(d.data(), kMaxPdc);
            }
        }
        totalLatency_.store(maxLat + (masterChain_ ? masterChain_->latency() : 0u), std::memory_order_relaxed);
    }

    // Restart integrated loudness / LRA / true-peak hold (stems + master).
    void resetMeters() noexcept {
        for (auto& s : stems_) { s.meter.reset(); }
        masterMeter_.reset();
    }

    void resetChains() noexcept {
        for (auto& s : stems_) {
            if (s.chain) s.chain->reset();
            for (auto& d : s.pdc) simd::clear(d.data(), kMaxPdc);
            s.pdcPos = 0;
        }
        if (masterChain_) masterChain_->reset();
    }

    double sr_ = 48000.0;
    std::uint32_t maxBlock_ = 512;
    std::array<Stem, kNumStems> stems_;
    AudioBuffer master_;
    AlignedBuffer<float> masterMono_;
    ProcessorChain* masterChain_ = nullptr;
    std::atomic<ProcessorChain*> masterPending_{nullptr};
    LoudnessMeter masterMeter_;
    TruePeakOversampler tpL_, tpR_;
    TripleBuffer<StemMeterSnapshot> masterSnap_;
    SampleTap masterTap_;
    SpscRing<ProcessorChain*, 64> graveyard_;
    WorkerPool pool_;
    std::atomic<bool> playing_{false}, meterResetReq_{false};
    std::atomic<std::uint64_t> playhead_{0}, seekTarget_{UINT64_MAX};
    std::atomic<std::uint32_t> totalLatency_{0};
    std::uint64_t length_ = 0, pos_ = 0;
    std::atomic<bool> loopOn_{false};
    std::atomic<std::uint64_t> loopRange_{0};
    std::uint64_t lpS_ = 0, lpE_ = 0;   // this block's loop (audio thread)
    bool lpOn_ = false;
    std::uint32_t block_ = 0;
    bool live_ = false, anySolo_ = false;
};

} // namespace hc
