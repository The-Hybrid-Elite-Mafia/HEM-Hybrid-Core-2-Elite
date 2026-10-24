// =============================================================================
//  src/platform/wasapi_output.cpp — WASAPI implementation (Windows only).
//  Link: ole32, avrt (MSVC picks them up from the #pragma comment lines).
// =============================================================================
#if defined(_WIN32)
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include "hc/platform/wasapi_output.hpp"
#include "hc/platform/thread_priority.hpp"
#include "hc/dsp/simd.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER)
  #pragma comment(lib, "ole32.lib")
#endif

namespace hc {
namespace {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

enum class SampleFmt { F32, I32, I24in32, I16 };

// KSDATAFORMAT_SUBTYPE_* GUIDs are the WAVE format tag placed in Data1 of the
// base GUID xxxxxxxx-0000-0010-8000-00AA00389B71 (PCM = 1, IEEE float = 3).
GUID waveSubtype(unsigned short tag) {
    GUID g{};
    g.Data1 = tag; g.Data2 = 0x0000; g.Data3 = 0x0010;
    const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    std::memcpy(g.Data4, tail, 8);
    return g;
}

std::string hrError(const char* what, HRESULT h) {
    char b[160];
    std::snprintf(b, sizeof b, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(h));
    return b;
}

WAVEFORMATEXTENSIBLE makeFormat(SampleFmt f, std::uint32_t sr, std::uint16_t ch) {
    WAVEFORMATEXTENSIBLE w{};
    const std::uint16_t bits = (f == SampleFmt::I16) ? 16 : 32;
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = ch;
    w.Format.nSamplesPerSec = sr;
    w.Format.wBitsPerSample = bits;
    w.Format.nBlockAlign = static_cast<WORD>(ch * bits / 8);
    w.Format.nAvgBytesPerSec = sr * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (f == SampleFmt::I24in32) ? 24 : bits;
    w.dwChannelMask = (ch == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w.SubFormat = waveSubtype(f == SampleFmt::F32 ? 3 : 1);
    return w;
}

bool classify(const WAVEFORMATEX* wf, SampleFmt& out) {
    unsigned short tag = wf->wFormatTag;
    unsigned valid = wf->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        tag = static_cast<unsigned short>(x->SubFormat.Data1);
        valid = x->Samples.wValidBitsPerSample;
    }
    if (tag == 3 && wf->wBitsPerSample == 32) { out = SampleFmt::F32; return true; }
    if (tag == 1 && wf->wBitsPerSample == 16) { out = SampleFmt::I16; return true; }
    if (tag == 1 && wf->wBitsPerSample == 32) { out = valid == 24 ? SampleFmt::I24in32 : SampleFmt::I32; return true; }
    return false;
}

} // namespace

struct WasapiOutput::Impl {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    bool exclusive = false, comInit = false;
    SampleFmt fmt = SampleFmt::F32;
    std::uint16_t channels = 2, blockAlign = 8;
    std::uint32_t bufferFrames = 0, maxBlock = 4096;
    std::vector<float> L, R;

    ~Impl() {
        render.reset(); client.reset(); device.reset();
        if (event) CloseHandle(event);
        if (comInit) CoUninitialize();
    }
    HRESULT activate() {
        return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
    }
    // Interleave + convert one chunk into the device buffer.
    void write(BYTE* dst, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            BYTE* frame = dst + static_cast<std::size_t>(i) * blockAlign;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const float v = c == 0 ? L[i] : (c == 1 ? R[i] : 0.0f);
                const float s = std::clamp(v, -1.0f, 1.0f);
                switch (fmt) {
                case SampleFmt::F32: std::memcpy(frame + c * 4, &v, 4); break;
                case SampleFmt::I16: { const auto q = static_cast<std::int16_t>(std::lrint(s * 32767.0f)); std::memcpy(frame + c * 2, &q, 2); break; }
                case SampleFmt::I24in32: { const auto q = static_cast<std::int32_t>(std::lrint(static_cast<double>(s) * 8388607.0)) << 8; std::memcpy(frame + c * 4, &q, 4); break; }
                case SampleFmt::I32: { const auto q = static_cast<std::int32_t>(std::llrint(static_cast<double>(s) * 2147483647.0)); std::memcpy(frame + c * 4, &q, 4); break; }
                }
            }
        }
    }
};

WasapiOutput::WasapiOutput() = default;
WasapiOutput::~WasapiOutput() { close(); }

std::string WasapiOutput::open(const Config& cfg) {
    close();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.maxBlock = cfg.maxBlock;
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m.comInit = SUCCEEDED(h);   // RPC_E_CHANGED_MODE: caller already set STA; COM still usable

    {
        ComPtr<IMMDeviceEnumerator> en;
        h = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                             reinterpret_cast<void**>(en.put()));
        if (FAILED(h)) return hrError("CoCreateInstance(MMDeviceEnumerator)", h);
        h = en->GetDefaultAudioEndpoint(eRender, eConsole, m.device.put());
        if (FAILED(h)) return hrError("GetDefaultAudioEndpoint", h);
    }
    if (FAILED(h = m.activate())) return hrError("IMMDevice::Activate(IAudioClient)", h);

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(h = m.client->GetMixFormat(&mix))) return hrError("GetMixFormat", h);
    const std::uint32_t mixRate = mix->nSamplesPerSec;
    const std::uint32_t sr = cfg.sampleRate > 0 ? static_cast<std::uint32_t>(cfg.sampleRate) : mixRate;
    Mode mode = cfg.mode;
    if (mode == Mode::SharedLowLatency && sr != mixRate) mode = Mode::SharedAutoConvert;

    m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::string err;
    char desc[200] = {};
    std::uint32_t period = 0;

    if (mode == Mode::Exclusive) {
        REFERENCE_TIME defP = 0, minP = 0;
        m.client->GetDevicePeriod(&defP, &minP);
        const SampleFmt order[] = {SampleFmt::F32, SampleFmt::I32, SampleFmt::I24in32, SampleFmt::I16};
        bool found = false;
        WAVEFORMATEXTENSIBLE w{};
        for (SampleFmt f : order) {
            w = makeFormat(f, sr, 2);
            if (m.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) { m.fmt = f; found = true; break; }
        }
        if (!found) { CoTaskMemFree(mix); return "device rejects every exclusive-mode stereo format at " + std::to_string(sr) + " Hz"; }
        h = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, minP, &w.Format, nullptr);
        if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // Documented recovery: query the aligned size, re-activate, retry.
            UINT32 frames = 0;
            m.client->GetBufferSize(&frames);
            if (FAILED(h = m.activate())) { CoTaskMemFree(mix); return hrError("re-Activate after alignment", h); }
            minP = static_cast<REFERENCE_TIME>(10000.0 * 1000.0 * frames / sr + 0.5);
            h = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, minP, &w.Format, nullptr);
        }
        if (FAILED(h)) { CoTaskMemFree(mix); return hrError("IAudioClient::Initialize(exclusive)", h); }
        m.exclusive = true; m.channels = 2; m.blockAlign = w.Format.nBlockAlign;
        const char* names[] = {"float32", "int32", "int24-in-32", "int16"};
        std::snprintf(desc, sizeof desc, "WASAPI exclusive, %s", names[static_cast<int>(m.fmt)]);
    } else if (mode == Mode::SharedLowLatency) {
        if (!classify(mix, m.fmt)) { CoTaskMemFree(mix); return "unsupported shared mix format"; }
        m.channels = mix->nChannels; m.blockAlign = mix->nBlockAlign;
        bool done = false;
        IAudioClient3* c3 = nullptr;
        if (SUCCEEDED(m.client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&c3))) && c3) {
            UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &defF, &fundF, &minF, &maxF)) &&
                SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minF, mix, nullptr))) {
                done = true; period = minF;
                std::snprintf(desc, sizeof desc, "WASAPI shared low-latency (IAudioClient3), engine period %u frames", minF);
            }
            c3->Release();
        }
        if (!done) {
            if (FAILED(h = m.activate())) { CoTaskMemFree(mix); return hrError("re-Activate for shared fallback", h); }
            h = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
            if (FAILED(h)) { CoTaskMemFree(mix); return hrError("IAudioClient::Initialize(shared)", h); }
            std::snprintf(desc, sizeof desc, "WASAPI shared (standard period)");
        }
    } else { // SharedAutoConvert
        WAVEFORMATEXTENSIBLE w = makeFormat(SampleFmt::F32, sr, 2);
        m.fmt = SampleFmt::F32; m.channels = 2; m.blockAlign = w.Format.nBlockAlign;
        h = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 0, 0, &w.Format, nullptr);
        if (FAILED(h)) { CoTaskMemFree(mix); return hrError("IAudioClient::Initialize(shared auto-convert)", h); }
        std::snprintf(desc, sizeof desc, "WASAPI shared + Windows SRC (%u Hz -> %u Hz device)", sr, mixRate);
    }
    CoTaskMemFree(mix);

    if (FAILED(h = m.client->SetEventHandle(m.event))) return hrError("SetEventHandle", h);
    UINT32 buf = 0;
    if (FAILED(h = m.client->GetBufferSize(&buf))) return hrError("GetBufferSize", h);
    m.bufferFrames = buf;
    if (FAILED(h = m.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(m.render.put()))))
        return hrError("GetService(IAudioRenderClient)", h);

    // Pre-roll one buffer of silence so the first event doesn't underrun.
    BYTE* data = nullptr;
    if (SUCCEEDED(m.render->GetBuffer(buf, &data))) m.render->ReleaseBuffer(buf, AUDCLNT_BUFFERFLAGS_SILENT);

    REFERENCE_TIME lat = 0;
    m.client->GetStreamLatency(&lat);
    if (period == 0) {
        REFERENCE_TIME defP = 0, minP = 0;
        m.client->GetDevicePeriod(&defP, &minP);
        period = m.exclusive ? buf : static_cast<std::uint32_t>(std::lround(static_cast<double>(defP) * sr / 1.0e7));
    }
    m.L.assign(m.maxBlock, 0.0f); m.R.assign(m.maxBlock, 0.0f);

    info_.sampleRate = sr;
    info_.bufferFrames = buf;
    info_.periodFrames = period;
    info_.channels = m.channels;
    info_.outputLatencyMs = static_cast<double>(lat) / 10000.0 + 1000.0 * buf / sr;
    char full[320];
    std::snprintf(full, sizeof full, "%s | %u Hz | buffer %u frames | ~%.1f ms output latency", desc, sr, buf, info_.outputLatencyMs);
    info_.description = full;
    return err;
}

std::string WasapiOutput::start(RenderCallback cb, void* user) {
    if (!impl_ || !impl_->client) return "device not open";
    if (running_.load()) return "already running";
    cb_ = cb; user_ = user;
    running_.store(true);
    thread_ = std::thread([this] { renderThread(); });
    return {};
}

void WasapiOutput::stop() {
    if (!running_.exchange(false)) return;
    if (impl_ && impl_->event) SetEvent(impl_->event);   // unblock the wait
    if (thread_.joinable()) thread_.join();
}

void WasapiOutput::close() {
    stop();
    impl_.reset();
}

void WasapiOutput::renderThread() {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        platform::RealtimeThreadScope rt;
        simd::ScopedFlushDenormals ftz;
        Impl& m = *impl_;
        m.client->Start();
        while (running_.load(std::memory_order_acquire)) {
            const DWORD w = WaitForSingleObject(m.event, 200);
            if (!running_.load(std::memory_order_acquire)) break;
            if (w != WAIT_OBJECT_0) { glitches_.fetch_add(1, std::memory_order_relaxed); continue; }
            UINT32 avail = m.bufferFrames;
            if (!m.exclusive) {
                UINT32 pad = 0;
                if (FAILED(m.client->GetCurrentPadding(&pad))) { glitches_.fetch_add(1, std::memory_order_relaxed); continue; }
                avail = m.bufferFrames - pad;
            }
            if (avail == 0) continue;
            BYTE* data = nullptr;
            const HRESULT gh = m.render->GetBuffer(avail, &data);
            if (gh == AUDCLNT_E_DEVICE_INVALIDATED) { running_.store(false); break; }   // device unplugged
            if (FAILED(gh)) { glitches_.fetch_add(1, std::memory_order_relaxed); continue; }
            std::uint32_t done = 0;
            while (done < avail) {
                const std::uint32_t n = std::min<std::uint32_t>(avail - done, m.maxBlock);
                cb_(user_, m.L.data(), m.R.data(), n);
                m.write(data + static_cast<std::size_t>(done) * m.blockAlign, n);
                done += n;
            }
            m.render->ReleaseBuffer(avail, 0);
        }
        m.client->Stop();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
}

} // namespace hc
#endif // _WIN32
