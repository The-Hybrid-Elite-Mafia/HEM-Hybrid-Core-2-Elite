// =============================================================================
//  platform/asio_output.hpp — ASIO output backend (Windows), same contract as WasapiOutput.
//
//  Talks to an installed ASIO driver (e.g. "Steinberg built-in ASIO Driver") through Steinberg's
//  ASIO SDK, used under its GPLv3 option. The SDK is never committed: the build downloads it
//  (see .github/workflows/release.yml) and defines HC_HAS_ASIO_SDK. Without it, asioAvailable()
//  is false and open() returns an error, so the app simply offers WASAPI only.
//
//  The driver owns the audio thread and calls back once per buffer; the callback converts the
//  engine's planar float L/R into the driver's sample format for the first two outputs (any
//  other outputs get silence). Larger buffers than the engine's block size are split.
//  ASIO is one stream per process.
// =============================================================================
#pragma once
#include "hc/platform/audio_device.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hc {

struct AsioDriverName {
    std::string name;          // registry key name (what the driver is loaded by)
    std::string description;
};

bool asioAvailable();                              // true when built with the ASIO SDK
std::vector<AsioDriverName> listAsioDrivers();     // installed 64-bit ASIO drivers (HKLM\SOFTWARE\ASIO)

class AsioOutput final : public IAudioOutput {
public:
    struct Config {
        std::string driver;            // as in listAsioDrivers()
        double sampleRate = 48000;     // the driver is switched to this rate
        std::uint32_t maxBlock = 4096; // engine block cap
        void* window = nullptr;        // HWND the driver may use for its own dialogs
    };
    AsioOutput();
    ~AsioOutput() override;

    std::string open(const Config& cfg);   // "" on success
    std::string start(RenderCallback cb, void* user) override;
    void stop() override;
    void close();
    const DeviceInfo& info() const override { return info_; }
    std::uint64_t glitchCount() const override { return glitches_.load(std::memory_order_relaxed); }

    struct Impl;   // public so the C-style driver callbacks can reach it

private:
    std::unique_ptr<Impl> impl_;
    DeviceInfo info_;
    std::atomic<std::uint64_t> glitches_{0};
};

} // namespace hc
