// =============================================================================
//  core/lockfree.hpp — the only three ways data crosses a thread boundary.
//
//  RULE: the audio thread never takes a mutex, never allocates, never frees,
//  never does I/O, never calls into the OS except to signal an event.
//
//    SpscRing<T,N>   : bounded FIFO, one producer + one consumer (commands,
//                      sample taps for the analysis thread, retired objects).
//    TripleBuffer<T> : "latest value wins" snapshot (meters → GPU UI).
//    AtomicParam     : UI writes a target, audio thread smooths toward it.
// =============================================================================
#pragma once
#include "hc/core/config.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hc {

// -----------------------------------------------------------------------------
// SpscRing — Lamport queue with cached indices. head_ and tail_ live on
// separate cache lines so producer and consumer never false-share.
// acquire/release ordering publishes the slot contents with the index.
// -----------------------------------------------------------------------------
template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr std::size_t kMask = Capacity - 1;

public:
    bool push(const T& v) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tailCache_ == Capacity) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (h - tailCache_ == Capacity) return false; // full — caller decides (drop/retry)
        }
        buf_[h & kMask] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == headCache_) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (t == headCache_) return false; // empty
        }
        out = buf_[t & kMask];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Bulk write for sample taps. Returns frames actually written.
    std::size_t push_n(const T* src, std::size_t n) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t freeSlots = Capacity - (h - t);
        if (n > freeSlots) n = freeSlots;
        for (std::size_t i = 0; i < n; ++i) buf_[(h + i) & kMask] = src[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    std::size_t pop_n(T* dst, std::size_t n) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t avail = h - t;
        if (n > avail) n = avail;
        for (std::size_t i = 0; i < n; ++i) dst[i] = buf_[(t + i) & kMask];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    std::size_t size_approx() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tailCache_ = 0;                       // producer-private
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t headCache_ = 0;                       // consumer-private
    alignas(kCacheLine) std::array<T, Capacity> buf_{};
};

// -----------------------------------------------------------------------------
// TripleBuffer — wait-free single-writer / single-reader snapshot exchange.
// Writer always has a private back buffer; reader always has a private front
// buffer; the middle slot is swapped atomically. Neither side ever waits, and
// the reader always sees the most recent complete snapshot.
// State byte: bits 0–1 = middle index, bit 2 = "fresh data" flag.
// -----------------------------------------------------------------------------
template <class T>
class TripleBuffer {
public:
    T& write_buffer() noexcept { return buf_[back_]; }
    void publish() noexcept {
        const std::uint8_t prev = middle_.exchange(static_cast<std::uint8_t>(back_ | kFresh), std::memory_order_acq_rel);
        back_ = prev & kIdx;
    }
    // Returns true if a new snapshot was swapped in.
    bool update() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kFresh) == 0) return false;
        const std::uint8_t prev = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = prev & kIdx;
        return true;
    }
    const T& read_buffer() const noexcept { return buf_[front_]; }

private:
    static constexpr std::uint8_t kIdx = 0x3, kFresh = 0x4;
    T buf_[3]{};
    alignas(kCacheLine) std::atomic<std::uint8_t> middle_{1};
    alignas(kCacheLine) std::uint8_t back_ = 0;
    alignas(kCacheLine) std::uint8_t front_ = 2;
};

// -----------------------------------------------------------------------------
// AtomicParam — UI thread calls set(); audio thread calls next() per sample
// (or block_value() per block) and gets a one-pole smoothed value, so knob
// moves never produce zipper noise.
// -----------------------------------------------------------------------------
class AtomicParam {
public:
    AtomicParam() = default;
    AtomicParam(float init, float minV, float maxV) : target_(init), current_(init), min_(minV), max_(maxV) {}

    // NON-RT: (re)configure range and value before the processor is published.
    void configure(float init, float minV, float maxV) noexcept {
        min_ = minV; max_ = maxV; target_.store(init, std::memory_order_relaxed); current_ = init;
    }

    void set(float v) noexcept { target_.store(v < min_ ? min_ : (v > max_ ? max_ : v), std::memory_order_relaxed); }
    float target() const noexcept { return target_.load(std::memory_order_relaxed); }

    // Non-RT: smoothing time constant.
    void prepare(double sampleRate, double smoothingMs = 20.0) noexcept {
        coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * smoothingMs * 0.001)));
        current_ = target();
    }
    void snap() noexcept { current_ = target(); }

    float next() noexcept {
        current_ += (target_.load(std::memory_order_relaxed) - current_) * coeff_;
        return current_;
    }
    // Advance the smoother by n samples in O(1) and return the end value.
    // Used for parameters that only need block-rate updates (EQ coefficients).
    float advance(std::uint32_t n) noexcept {
        const float t = target_.load(std::memory_order_relaxed);
        const float k = std::pow(1.0f - coeff_, static_cast<float>(n));
        current_ = t + (current_ - t) * k;
        return current_;
    }
    float current() const noexcept { return current_; }
    float minValue() const noexcept { return min_; }
    float maxValue() const noexcept { return max_; }

private:
    std::atomic<float> target_{0.0f};
    float current_ = 0.0f;
    float coeff_ = 1.0f;
    float min_ = -1e30f, max_ = 1e30f;
};

} // namespace hc
