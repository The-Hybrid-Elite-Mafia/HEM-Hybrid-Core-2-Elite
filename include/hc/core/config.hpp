// =============================================================================
//  THE HYBRID CORE 2.0 — Hybrid Elite Mafia Studio Suite
//  core/config.hpp — compile-time constants shared by every micro-engine.
// =============================================================================
#pragma once
#include <cstddef>
#include <cstdint>

namespace hc {

// The engine holds exactly 13 stems. This is a hard architectural constant:
// every per-stem array in the engine is a std::array<..., kNumStems>, so the
// audio thread never allocates and never branches on a dynamic stem count.
inline constexpr std::size_t kNumStems = 13;

// Stems and buses are stereo, planar (L buffer, R buffer). Mono sources are
// duplicated to both channels at load time.
inline constexpr std::size_t kNumChannels = 2;

// Largest block the audio callback will ever hand us. WASAPI/ASIO periods are
// typically 64–1024 frames; offline rendering uses 512.
inline constexpr std::size_t kMaxBlock = 4096;

// 64-byte alignment = one cache line = one AVX-512 register. AVX2 needs 32,
// but aligning to 64 also kills false sharing between adjacent buffers.
inline constexpr std::size_t kSimdAlign = 64;
inline constexpr std::size_t kCacheLine = 64;

// Maximum processors in one stem / master chain.
inline constexpr std::size_t kMaxChainSlots = 16;

} // namespace hc
