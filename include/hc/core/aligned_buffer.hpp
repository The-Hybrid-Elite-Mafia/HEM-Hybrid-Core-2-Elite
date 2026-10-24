// =============================================================================
//  core/aligned_buffer.hpp — cache-line aligned, move-only POD buffer.
//  Allocation happens ONLY in prepare() on a non-real-time thread.
// =============================================================================
#pragma once
#include "hc/core/config.hpp"
#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace hc {

template <class T>
class AlignedBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "AlignedBuffer holds POD audio data only");

public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(std::size_t n) { resize(n); }
    ~AlignedBuffer() { release(); }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& o) noexcept : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) { release(); p_ = std::exchange(o.p_, nullptr); n_ = std::exchange(o.n_, 0); }
        return *this;
    }

    // Non-RT. Reallocates and zero-fills.
    void resize(std::size_t n) {
        release();
        if (n == 0) return;
        p_ = static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kSimdAlign}));
        n_ = n;
        std::fill_n(p_, n_, T{});
    }
    void zero() noexcept { if (p_) std::fill_n(p_, n_, T{}); }

    T*       data() noexcept { return p_; }
    const T* data() const noexcept { return p_; }
    std::size_t size() const noexcept { return n_; }
    T&       operator[](std::size_t i) noexcept { return p_[i]; }
    const T& operator[](std::size_t i) const noexcept { return p_[i]; }

private:
    void release() noexcept {
        if (p_) ::operator delete(p_, std::align_val_t{kSimdAlign});
        p_ = nullptr; n_ = 0;
    }
    T* p_ = nullptr;
    std::size_t n_ = 0;
};

// Planar multichannel block owned by the engine (one per stem + master).
struct AudioBuffer {
    AlignedBuffer<float> ch[kNumChannels];
    void prepare(std::size_t frames) { for (auto& c : ch) c.resize(frames); }
    void zero() noexcept { for (auto& c : ch) c.zero(); }
};

// Non-owning view handed to processors on the audio thread.
struct AudioBlock {
    float* ch[kNumChannels] = {};
    std::uint32_t numChannels = kNumChannels;
    std::uint32_t numFrames = 0;
};

} // namespace hc
