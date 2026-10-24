// =============================================================================
//  src/platform/asio_output.cpp — ASIO backend (see asio_output.hpp).
//  Built with the Steinberg ASIO SDK when HC_HAS_ASIO_SDK is defined; otherwise a stub that
//  reports ASIO as unavailable.
// =============================================================================
#if defined(_WIN32)
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include "hc/platform/asio_output.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hc {

// ---- installed drivers: HKLM\SOFTWARE\ASIO\<name> { CLSID, Description } (64-bit view) ----
std::vector<AsioDriverName> listAsioDrivers() {
    std::vector<AsioDriverName> out;
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        char name[256];
        DWORD len = sizeof name;
        const LSTATUS st = RegEnumKeyExA(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        AsioDriverName d;
        d.name = name;
        char desc[256] = {};
        DWORD bytes = sizeof desc;
        if (RegGetValueA(root, name, "Description", RRF_RT_REG_SZ, nullptr, desc, &bytes) == ERROR_SUCCESS) d.description = desc;
        out.push_back(std::move(d));
    }
    RegCloseKey(root);
    return out;
}

} // namespace hc

#if defined(HC_HAS_ASIO_SDK)
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include <atomic>

extern AsioDrivers* asioDrivers;   // Steinberg SDK (host/asiodrivers.cpp)
bool loadAsioDriver(char* name);

namespace hc {

bool asioAvailable() { return true; }

struct AsioOutput::Impl {
    long frames = 0;
    long numOut = 0;
    ASIOBufferInfo buf[2] = {};
    ASIOSampleType type[2] = {ASIOSTFloat32LSB, ASIOSTFloat32LSB};
    bool postOutput = false, created = false, loaded = false;
    std::vector<float> L, R;
    std::uint32_t maxBlock = 4096;
    RenderCallback cb = nullptr;
    void* user = nullptr;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t>* glitches = nullptr;

    static void fromFloat(ASIOSampleType t, const float* src, void* dst, long n) noexcept {
        auto clip = [](float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); };
        switch (t) {
        case ASIOSTFloat32LSB: std::memcpy(dst, src, static_cast<std::size_t>(n) * sizeof(float)); break;
        case ASIOSTFloat64LSB: { auto* d = static_cast<double*>(dst); for (long i = 0; i < n; ++i) d[i] = src[i]; break; }
        case ASIOSTInt32LSB: { auto* d = static_cast<std::int32_t*>(dst); for (long i = 0; i < n; ++i) d[i] = static_cast<std::int32_t>(static_cast<double>(clip(src[i])) * 2147483647.0); break; }
        case ASIOSTInt16LSB: { auto* d = static_cast<std::int16_t*>(dst); for (long i = 0; i < n; ++i) d[i] = static_cast<std::int16_t>(clip(src[i]) * 32767.0f); break; }
        case ASIOSTInt24LSB: {
            auto* d = static_cast<std::uint8_t*>(dst);
            for (long i = 0; i < n; ++i) {
                const std::int32_t v = static_cast<std::int32_t>(clip(src[i]) * 8388607.0f);
                d[3 * i] = static_cast<std::uint8_t>(v); d[3 * i + 1] = static_cast<std::uint8_t>(v >> 8); d[3 * i + 2] = static_cast<std::uint8_t>(v >> 16);
            }
            break;
        }
        default: break;
        }
    }
    static bool supported(ASIOSampleType t) noexcept {
        return t == ASIOSTFloat32LSB || t == ASIOSTFloat64LSB || t == ASIOSTInt32LSB || t == ASIOSTInt16LSB || t == ASIOSTInt24LSB;
    }
    // Driver thread.
    void render(long index) noexcept {
        float* out[2] = {L.data(), R.data()};
        for (long done = 0; done < frames;) {
            const std::uint32_t n = static_cast<std::uint32_t>(std::min<long>(frames - done, static_cast<long>(maxBlock)));
            cb(user, out[0] + done, out[1] + done, n);
            done += static_cast<long>(n);
        }
        for (long c = 0; c < numOut; ++c) fromFloat(type[c], out[c], buf[c].buffers[index], frames);
        if (postOutput) ASIOOutputReady();
    }
};

namespace {
std::atomic<AsioOutput::Impl*> gActive{nullptr};
void onSwitch(long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->render(index);
}
ASIOTime* onSwitchTime(ASIOTime* t, long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->render(index);
    return t;
}
void onRate(ASIOSampleRate) {}
long onMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioOverload || value == kAsioSupportsTimeInfo) ? 1L : 0L;
    case kAsioEngineVersion: return 2L;
    case kAsioOverload:
        if (auto* p = gActive.load(std::memory_order_acquire); p && p->glitches) p->glitches->fetch_add(1, std::memory_order_relaxed);
        return 1L;
    case kAsioResetRequest: return 1L;
    case kAsioSupportsTimeInfo: return 1L;
    default: return 0L;
    }
}
ASIOCallbacks gCallbacks{&onSwitch, &onRate, &onMessage, &onSwitchTime};
} // namespace

AsioOutput::AsioOutput() : impl_(std::make_unique<Impl>()) {}
AsioOutput::~AsioOutput() { close(); }

std::string AsioOutput::open(const Config& cfg) {
    close();
    Impl& s = *impl_;
    Impl* none = nullptr;
    if (!gActive.compare_exchange_strong(none, &s)) return "another ASIO stream is open";
    auto fail = [this](const char* stage, long code) {
        close();
        char b[160];
        std::snprintf(b, sizeof b, "ASIO: %s failed (%ld)", stage, code);
        return std::string(b);
    };
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the driver is a COM object (S_FALSE if already done)
    std::vector<char> name(cfg.driver.begin(), cfg.driver.end());
    name.push_back('\0');
    if (!loadAsioDriver(name.data())) return fail("loading the driver", 0);
    s.loaded = true;
    ASIODriverInfo di{};
    di.asioVersion = 2;
    di.sysRef = cfg.window ? cfg.window : GetDesktopWindow();
    ASIOError e = ASIOInit(&di);
    if (e != ASE_OK) return fail("starting the driver", e);
    if ((e = ASIOCanSampleRate(cfg.sampleRate)) != ASE_OK) return fail("this sample rate", e);
    ASIOSampleRate cur = 0;
    if (ASIOGetSampleRate(&cur) != ASE_OK || cur != cfg.sampleRate)
        if ((e = ASIOSetSampleRate(cfg.sampleRate)) != ASE_OK) return fail("setting the sample rate", e);
    long ins = 0, outs = 0;
    if ((e = ASIOGetChannels(&ins, &outs)) != ASE_OK) return fail("reading channels", e);
    if (outs < 1) return fail("finding an output", 0);
    long mn = 0, mx = 0, pref = 0, gran = 0;
    if ((e = ASIOGetBufferSize(&mn, &mx, &pref, &gran)) != ASE_OK) return fail("reading the buffer size", e);
    s.numOut = std::min<long>(outs, 2);
    for (long c = 0; c < s.numOut; ++c) { s.buf[c].isInput = ASIOFalse; s.buf[c].channelNum = c; }
    if ((e = ASIOCreateBuffers(s.buf, s.numOut, pref, &gCallbacks)) != ASE_OK) return fail("creating buffers", e);
    s.created = true;
    for (long c = 0; c < s.numOut; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c; ci.isInput = ASIOFalse;
        if ((e = ASIOGetChannelInfo(&ci)) != ASE_OK) return fail("reading the sample format", e);
        if (!Impl::supported(ci.type)) return fail("an unsupported sample format", ci.type);
        s.type[c] = ci.type;
    }
    s.frames = pref;
    s.maxBlock = cfg.maxBlock ? cfg.maxBlock : 4096;
    s.L.assign(static_cast<std::size_t>(pref), 0.0f);
    s.R.assign(static_cast<std::size_t>(pref), 0.0f);
    s.postOutput = ASIOOutputReady() == ASE_OK;
    s.glitches = &glitches_;
    long inLat = 0, outLat = 0;
    ASIOGetLatencies(&inLat, &outLat);
    info_ = DeviceInfo{};
    info_.sampleRate = cfg.sampleRate;
    info_.periodFrames = static_cast<std::uint32_t>(pref);
    info_.bufferFrames = static_cast<std::uint32_t>(pref);
    info_.channels = static_cast<std::uint32_t>(s.numOut);
    info_.outputLatencyMs = 1000.0 * static_cast<double>(outLat) / cfg.sampleRate;
    char b[256];
    std::snprintf(b, sizeof b, "ASIO: %s | %.0f Hz | buffer %ld frames | ~%.1f ms output latency", cfg.driver.c_str(), cfg.sampleRate, pref,
                  info_.outputLatencyMs);
    info_.description = b;
    return {};
}

std::string AsioOutput::start(RenderCallback cb, void* user) {
    Impl& s = *impl_;
    if (!s.created) return "ASIO: not open";
    s.cb = cb; s.user = user;
    s.running.store(true, std::memory_order_release);
    if (const ASIOError e = ASIOStart(); e != ASE_OK) {
        s.running.store(false);
        char b[80];
        std::snprintf(b, sizeof b, "ASIO: start failed (%ld)", static_cast<long>(e));
        return b;
    }
    return {};
}

void AsioOutput::stop() {
    Impl& s = *impl_;
    if (s.running.exchange(false)) ASIOStop();
}

void AsioOutput::close() {
    stop();
    Impl& s = *impl_;
    if (s.created) { ASIODisposeBuffers(); s.created = false; }
    if (s.loaded) { ASIOExit(); s.loaded = false; }
    Impl* self = &s;
    gActive.compare_exchange_strong(self, nullptr);
}

} // namespace hc

#else // ------------------------------------------------------------------ no SDK in this build
namespace hc {
bool asioAvailable() { return false; }
struct AsioOutput::Impl {};
AsioOutput::AsioOutput() : impl_(std::make_unique<Impl>()) {}
AsioOutput::~AsioOutput() = default;
std::string AsioOutput::open(const Config&) { return "ASIO isn't in this build"; }
std::string AsioOutput::start(RenderCallback, void*) { return "ASIO isn't in this build"; }
void AsioOutput::stop() {}
void AsioOutput::close() {}
} // namespace hc
#endif
#endif // _WIN32
