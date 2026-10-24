// =============================================================================
//  engine/worker_pool.hpp — real-time fork/join for the 13 stem chains.
//
//  The audio callback thread is the "conductor": it publishes a job batch,
//  works on it itself, and spins until every job is done. Workers grab job
//  indices with a lock-free CAS. Nothing here ever blocks the conductor:
//    • no mutex, no condition variable on the audio path
//    • the only kernel call on the audio path is one futex/WakeByAddress
//      notify per callback (non-blocking) to wake parked workers.
//
//  STATE WORD (64-bit atomic):  [ generation:32 | count:16 | next:16 ]
//  Packing generation + count + next into ONE atomic means a worker that is
//  late from the previous batch can never steal an index from the new batch:
//  its CAS fails the moment the generation changes. Job pointer/context are
//  published by the release-store of the state word and read only after a
//  successful acquire-CAS.
//
//  Workers spin ~20 µs (pause loop) before parking on an atomic wait. At a
//  128-frame/48 kHz callback (2.67 ms) they re-wake every period, so the
//  spin window usually catches the next batch without a context switch.
// =============================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#if defined(__SSE2__) || defined(_M_X64)
  #include <immintrin.h>
  #define HC_PAUSE() _mm_pause()
#else
  #define HC_PAUSE() std::this_thread::yield()
#endif

namespace hc {

class WorkerPool {
public:
    using Job = void (*)(void* ctx, std::size_t index) noexcept;
    using ThreadInit = void (*)(unsigned workerIndex);

    WorkerPool() = default;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool() { stop(); }

    // NON-RT.
    void start(unsigned numWorkers, ThreadInit init = nullptr) {
        stop();
        running_.store(true, std::memory_order_release);
        for (unsigned i = 0; i < numWorkers; ++i)
            threads_.emplace_back([this, i, init] { if (init) init(i); loop(); });
    }
    void stop() {
        if (!running_.exchange(false)) { threads_.clear(); return; }
        wake_.fetch_add(1, std::memory_order_release);
        wake_.notify_all();
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
    }
    unsigned workerCount() const noexcept { return static_cast<unsigned>(threads_.size()); }

    // RT. Runs job(ctx, 0..count-1) across workers + calling thread; returns
    // when all have completed. count ≤ 65535.
    void run(Job job, void* ctx, std::size_t count) noexcept {
        if (count == 0) return;
        job_ = job; ctx_ = ctx;
        done_.store(0, std::memory_order_relaxed);
        const std::uint64_t gen = (state_.load(std::memory_order_relaxed) >> 32) + 1;
        state_.store((gen << 32) | (static_cast<std::uint64_t>(count) << 16), std::memory_order_release);
        if (!threads_.empty()) {
            wake_.fetch_add(1, std::memory_order_release);
            wake_.notify_all();
        }
        work(static_cast<std::uint32_t>(gen));
        while (done_.load(std::memory_order_acquire) < count) HC_PAUSE();
    }

private:
    // Claim and execute indices of generation `gen` until exhausted.
    void work(std::uint32_t gen) noexcept {
        std::uint64_t s = state_.load(std::memory_order_acquire);
        for (;;) {
            if (static_cast<std::uint32_t>(s >> 32) != gen) return;
            const std::uint64_t count = (s >> 16) & 0xFFFF, next = s & 0xFFFF;
            if (next >= count) return;
            if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                job_(ctx_, static_cast<std::size_t>(next));
                done_.fetch_add(1, std::memory_order_acq_rel);
                s = state_.load(std::memory_order_acquire);
            }
        }
    }

    void loop() noexcept {
        std::uint32_t seenGen = static_cast<std::uint32_t>(state_.load(std::memory_order_acquire) >> 32);
        while (running_.load(std::memory_order_acquire)) {
            const std::uint32_t w = wake_.load(std::memory_order_acquire);
            const std::uint32_t gen = static_cast<std::uint32_t>(state_.load(std::memory_order_acquire) >> 32);
            if (gen != seenGen) { seenGen = gen; work(gen); continue; }
            bool found = false;
            for (int i = 0; i < 4000; ++i) {                // ≈ 20 µs of pause on modern cores
                HC_PAUSE();
                if (static_cast<std::uint32_t>(state_.load(std::memory_order_acquire) >> 32) != seenGen) { found = true; break; }
            }
            if (!found) wake_.wait(w, std::memory_order_acquire);
        }
    }

    alignas(64) std::atomic<std::uint64_t> state_{0};
    alignas(64) std::atomic<std::size_t> done_{0};
    alignas(64) std::atomic<std::uint32_t> wake_{0};
    alignas(64) std::atomic<bool> running_{false};
    Job job_ = nullptr;
    void* ctx_ = nullptr;
    std::vector<std::thread> threads_;
};

} // namespace hc
