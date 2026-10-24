// =============================================================================
//  platform/wasapi_output.hpp — bare-metal WASAPI render backend (Windows).
//
//  MODES
//   Exclusive (event-driven): owns the endpoint, bypasses the Windows mixer
//     and its resampler entirely. Period = device minimum (typically 3 ms /
//     144 frames, as low as ~1.3 ms on good hardware). Tries float32, then
//     32-bit int, 24-in-32, then 16-bit at the requested rate.
//   Shared low-latency (IAudioClient3, Win10+): uses the audio engine's
//     minimum period (often 10 ms default → ~2.7 ms min on supported drivers).
//   Shared auto-convert: when the session's sample rate differs from the
//     device mix rate, Windows' SRC converts (AUTOCONVERTPCM). Highest
//     latency, but plays anything.
//
//  THREAD: one MMCSS "Pro Audio" render thread, woken by the WASAPI event.
//  The callback writes planar float; this layer interleaves/converts.
// =============================================================================
#pragma once
#include "hc/platform/audio_device.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hc {

class WasapiOutput final : public IAudioOutput {
public:
    enum class Mode { Exclusive, SharedLowLatency, SharedAutoConvert };
    struct Config {
        Mode mode = Mode::SharedLowLatency;
        double sampleRate = 0;          // 0 = device's native mix rate
        std::uint32_t maxBlock = 4096;  // engine block cap; larger periods are split
    };

    WasapiOutput();
    ~WasapiOutput() override;

    // Opens the default render endpoint. Returns "" on success.
    std::string open(const Config& cfg);
    std::string start(RenderCallback cb, void* user) override;
    void stop() override;
    void close();
    const DeviceInfo& info() const override { return info_; }
    std::uint64_t glitchCount() const override { return glitches_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    void renderThread();
    std::unique_ptr<Impl> impl_;
    DeviceInfo info_;
    RenderCallback cb_ = nullptr;
    void* user_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> glitches_{0};
};

} // namespace hc
