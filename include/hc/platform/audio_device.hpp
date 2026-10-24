// =============================================================================
//  platform/audio_device.hpp — backend-neutral device contract.
//
//  The engine never talks to WASAPI or ASIO directly. A backend owns its
//  real-time thread and calls RenderCallback with planar float L/R once per
//  device period. WasapiOutput implements this today; an ASIO backend
//  implements the same interface (see blueprint §4.3 — the ASIO SDK is
//  Steinberg-licensed and must be downloaded by the developer; it is not
//  reproduced here).
// =============================================================================
#pragma once
#include <cstdint>
#include <string>

namespace hc {

using RenderCallback = void (*)(void* user, float* L, float* R, std::uint32_t frames) noexcept;

struct DeviceInfo {
    double sampleRate = 0;
    std::uint32_t periodFrames = 0;     // frames per callback
    std::uint32_t bufferFrames = 0;     // total device buffer
    std::uint32_t channels = 0;
    double outputLatencyMs = 0;
    std::string description;            // e.g. "WASAPI exclusive, float32, 48000 Hz, 128 frames"
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    virtual std::string start(RenderCallback cb, void* user) = 0;   // "" on success
    virtual void stop() = 0;
    virtual const DeviceInfo& info() const = 0;
    virtual std::uint64_t glitchCount() const = 0;                  // late/failed periods
};

} // namespace hc
