// =============================================================================
//  HYBRID CORE 2 ELITE — the producer desk (Windows x64). Built from THE HYBRID CORE 2.0 (hybrid-core repo).
//  studio/main.cpp — the application.
//
//  The window shows the HEM faceplate art. Every knob, meter, screen and
//  readout painted on it was cut out (studio/skin_layout.py) and is drawn here
//  live, at the exact same spot, wired to the real engine:
//    track rows ........ 13 stems: name, volume (drag), mute, solo, waveform
//    transport ......... menu, rewind, play, stop, record = export master,
//                        time, tempo, master level, analyze, settings, view
//    STFT DESTROYER .... artifact repair (per selected stem) + live spectrogram
//    RESONANCE ......... harsh-resonance suppressor + live cut curve
//    TRANSIENT ......... transient designer + live attack/sustain activity
//    MASTERING ......... width, glue, true-peak limiter, LUFS / TP meters,
//                        loudness target for export
//
//  Threads: UI (this file) · WASAPI render thread (MMCSS) · DSP workers ·
//  export worker. The UI never blocks audio: it reads meters from triple
//  buffers / rings and writes parameters through atomics.
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include "gfx.hpp"
#include "app_util.hpp"
#include "skin_layout.hpp"
#include "desk_layout.hpp"
#include "resource.h"
#include "hc/app/offline_render.hpp"
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include "hc/platform/thread_priority.hpp"
#include "hc/platform/wasapi_output.hpp"
#include "hc/platform/asio_output.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hc;
using namespace hc::gfx;
using namespace hc::studio;
namespace LY = hc::studio::layout;
namespace DK = hc::studio::desk;

namespace {

// ------------------------------------------------------------------ palette
constexpr Color kCyan = 0x3EC8FFFF, kCyanDim = 0x1B5E78FF, kOrange = 0xFF9A3EFF, kGreen = 0x5BE37AFF;
constexpr Color kYellow = 0xE8C547FF, kRed = 0xFF4A4AFF, kText = 0xD8E6EEFF, kDim = 0x7F93A0FF;
constexpr Color kScreen = 0x070B0FFF, kPanel = 0x12171CFF, kWhite = 0xF2F4F6FF;
constexpr const char* kVersion = "2.0.0";

constexpr std::uint32_t kBlock = 1024;
constexpr UINT WM_EXPORT_DONE = WM_APP + 1;

Rect R(const LY::Box& b) { return {b.x0, b.y0, b.x1, b.y1}; }
float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
bool stemColorIsOrange(int i) { return i == 0 || i == 4 || i == 6 || i == 10; }

std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }
std::string timeStr(double sec) {
    if (sec < 0) sec = 0;
    const int m = static_cast<int>(sec / 60.0);
    const double s = sec - 60.0 * m;
    char b[32]; std::snprintf(b, sizeof b, "%02d:%06.3f", m, s);
    return b;
}
std::string lufsStr(float v) { return std::isfinite(v) && v > -70.0f ? fmt("%.1f", v) : std::string("--"); }

// ------------------------------------------------------------------ spectrogram (UI thread)
// Looks for a ring: a narrow peak standing out from its neighbourhood (±1/6 octave) in a power spectrum
// (power per FFT bin, kN/2+1 values). Returns how many dB it stands out (0 = none) and its pitch in `hz`.
float pingSearch(const std::vector<float>& pw, int N, double sr, float& hz) {
    if (pw.size() != static_cast<std::size_t>(N / 2 + 1)) return 0.0f;
    std::vector<float> db(static_cast<std::size_t>(N / 2 + 1));
    float maxDb = -200.0f;
    for (int k = 1; k <= N / 2; ++k) {
        db[static_cast<std::size_t>(k)] = 10.0f * std::log10(pw[static_cast<std::size_t>(k)] + 1e-20f);
        maxDb = std::max(maxDb, db[static_cast<std::size_t>(k)]);
    }
    int best = -1;
    float bestScore = 0.0f;
    std::vector<float> nb;
    nb.reserve(64);
    for (int k = 2; k < N / 2 - 1; ++k) {
        const double f = static_cast<double>(k) * sr / N;
        if (f < 150.0 || f > 12000.0) continue;
        const float v = db[static_cast<std::size_t>(k)];
        if (v < maxDb - 45.0f || v < db[static_cast<std::size_t>(k - 1)] || v < db[static_cast<std::size_t>(k + 1)]) continue;
        const int lo = std::max(1, static_cast<int>(k / 1.1225)), hi = std::min(N / 2, static_cast<int>(k * 1.1225) + 1);
        nb.clear();
        for (int j = lo; j <= hi; ++j) if (std::abs(j - k) > 1) nb.push_back(db[static_cast<std::size_t>(j)]);
        if (nb.size() < 4) continue;
        std::nth_element(nb.begin(), nb.begin() + static_cast<std::ptrdiff_t>(nb.size() / 2), nb.end());
        const float score = v - nb[nb.size() / 2];
        if (score > bestScore) { bestScore = score; best = k; }
    }
    if (best < 0) return 0.0f;
    const float a = db[static_cast<std::size_t>(best - 1)], bb = db[static_cast<std::size_t>(best)], c = db[static_cast<std::size_t>(best + 1)];
    const float den = a - 2 * bb + c;
    const float off = std::fabs(den) > 1e-6f ? 0.5f * (a - c) / den : 0.0f;
    hz = static_cast<float>((best + clampf(off, -0.5f, 0.5f)) * sr / N);
    return bestScore;
}

struct Spectrogram {
    static constexpr int kCols = 224, kRows = 56, kN = 1024, kHop = 512;
    float cell[kCols][kRows] = {};
    int head = 0;
    RealFFT fft;
    std::vector<float> ring, win, frame, re, im;
    std::size_t pos = 0, since = 0;
    double sr = 48000.0;
    int rowBin[kRows + 1] = {};
    float cal = 0.0f;
    // Fine spectra (power per FFT bin): `disp` follows quickly (EQ screen), `avg` over ~1 s (ping finder).
    std::vector<float> disp, avg;
    int frames = 0;
    std::uint64_t col = 0;   // FFT frames made since prepare (one every 512 samples = 10.7 ms)

    // ---- 3D landscape: kLRows rows (row 0 = newest), each the loudest of kLSub FFTs, kLPts log-spaced
    // points 30 Hz .. 20 kHz, in dB (a full-scale sine reads 0 dB). 72 x 3 x 10.7 ms = 2.3 s of history.
    static constexpr int kLRows = 72, kLPts = 112, kLSub = 3;
    float land[kLRows][kLPts] = {};
    std::uint8_t landHarsh[kLRows] = {};   // 1 = a harsh hit happened in this row
    float landHarshHz[kLRows] = {};
    int landHead = 0, landSub = 0;
    float landPend[kLPts] = {};
    std::uint8_t pendHarsh = 0;
    float pendHarshHz = 0;
    std::uint64_t landRows = 0;
    int ptLo[kLPts] = {}, ptHi[kLPts] = {};
    static float landHz(int i) { return 30.0f * std::pow(20000.0f / 30.0f, static_cast<float>(i) / (kLPts - 1)); }
    const float* landRow(int age) const { return land[(landHead - 1 - age + 2 * kLRows) % kLRows]; }
    int landIdx(int age) const { return (landHead - 1 - age + 2 * kLRows) % kLRows; }

    // ---- harsh hits: a short burst in the highs (5-16 kHz) that jumps 9 dB over the level between hits
    // and makes up most of the sound at that moment (tambourine, hats, shakers, a vocal "sss").
    struct Hit { std::uint64_t col; float hz, db; };
    static constexpr int kMaxHits = 512;
    Hit hits[kMaxHits] = {};
    int hitHead = 0, hitCount = 0;
    float hfFloor = -140.0f;
    std::uint64_t lastHitCol = 0;
    float hfNow = -140.0f;
    // ---- ring tracker (the same search as FIND PING, run ~4 times a second on `avg`)
    float ringHz = 0.0f, ringScore = 0.0f;

    void prepare(double rate) {
        sr = rate; fft.prepare(kN);
        ring.assign(kN, 0); frame.assign(kN, 0); re.assign(kN / 2 + 1, 0); im.assign(kN / 2 + 1, 0); win.resize(kN);
        double ws = 0;
        for (int i = 0; i < kN; ++i) { win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / kN)); ws += win[i]; }
        cal = static_cast<float>(-20.0 * std::log10(ws / 2.0));
        for (int r = 0; r <= kRows; ++r) {
            const double f = 40.0 * std::pow(20000.0 / 40.0, static_cast<double>(r) / kRows);
            rowBin[r] = std::clamp(static_cast<int>(f * kN / sr), 1, kN / 2);
        }
        for (int i = 0; i < kLPts; ++i) {   // FFT bins under each landscape point (at least the nearest one)
            const double f0 = i > 0 ? std::sqrt(static_cast<double>(landHz(i - 1)) * landHz(i)) : landHz(0);
            const double f1 = i < kLPts - 1 ? std::sqrt(static_cast<double>(landHz(i)) * landHz(i + 1)) : landHz(i);
            int lo = static_cast<int>(std::ceil(f0 * kN / sr)), hi = static_cast<int>(std::floor(f1 * kN / sr));
            if (hi < lo) lo = hi = static_cast<int>(std::lround(landHz(i) * kN / sr));
            ptLo[i] = std::clamp(lo, 1, kN / 2); ptHi[i] = std::clamp(hi, ptLo[i], kN / 2);
        }
        for (auto& c : cell) for (auto& v : c) v = -140.0f;
        for (auto& r : land) for (auto& v : r) v = -140.0f;
        for (auto& v : landPend) v = -140.0f;
        for (int r = 0; r < kLRows; ++r) { landHarsh[r] = 0; landHarshHz[r] = 0; }
        landHead = landSub = 0; landRows = 0; pendHarsh = 0;
        hitHead = hitCount = 0; hfFloor = -140.0f; lastHitCol = 0; hfNow = -140.0f;
        ringHz = ringScore = 0.0f;
        disp.assign(kN / 2 + 1, 0.0f); avg.assign(kN / 2 + 1, 0.0f); frames = 0; col = 0;
        pos = since = 0; head = 0;
    }
    void feed(const float* x, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            ring[pos] = x[i]; pos = (pos + 1) % kN;
            if (++since == kHop) { since = 0; column(); }
        }
    }
    // Harsh hits in the last `seconds`.
    int hitsWithin(double seconds) const {
        const std::uint64_t span = static_cast<std::uint64_t>(seconds * sr / kHop);
        int n = 0;
        for (int i = 0; i < hitCount; ++i) {
            const Hit& h = hits[(hitHead - 1 - i + kMaxHits) % kMaxHits];
            if (col - h.col > span) break;
            ++n;
        }
        return n;
    }
    // Median frequency of the harsh hits in the last `seconds` (0 = none).
    float hitHzWithin(double seconds) const {
        const std::uint64_t span = static_cast<std::uint64_t>(seconds * sr / kHop);
        std::vector<float> f;
        for (int i = 0; i < hitCount; ++i) {
            const Hit& h = hits[(hitHead - 1 - i + kMaxHits) % kMaxHits];
            if (col - h.col > span) break;
            f.push_back(h.hz);
        }
        if (f.empty()) return 0.0f;
        std::nth_element(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(f.size() / 2), f.end());
        return f[f.size() / 2];
    }
    void column() {
        for (int i = 0; i < kN; ++i) frame[i] = ring[(pos + i) % kN] * win[i];
        fft.forward(frame.data(), re.data(), im.data());
        double hf = 0.0, tot = 0.0, best = 0.0;
        int bestK = 0;
        const int k150 = static_cast<int>(150.0 * kN / sr), k4k = static_cast<int>(4000.0 * kN / sr);
        const int k5k = static_cast<int>(5000.0 * kN / sr), k16k = std::min(kN / 2, static_cast<int>(16000.0 * kN / sr));
        for (int k = 0; k <= kN / 2; ++k) {
            const float pw = re[k] * re[k] + im[k] * im[k];
            disp[k] += (pw - disp[k]) * 0.35f;
            avg[k] += (pw - avg[k]) * 0.012f;
            if (k >= k150) tot += pw;
            if (k >= k5k && k <= k16k) hf += pw;
            if (k >= k4k && k <= k16k && pw > best) { best = pw; bestK = k; }
        }
        ++frames;
        ++col;
        float* c = cell[head];
        for (int r = 0; r < kRows; ++r) {
            float m = 0.0f;
            for (int k = rowBin[r]; k <= std::max(rowBin[r], rowBin[r + 1] - 1); ++k) m = std::max(m, re[k] * re[k] + im[k] * im[k]);
            c[r] = 10.0f * std::log10(m + 1e-20f) + cal;
        }
        head = (head + 1) % kCols;

        // Harsh hits. Floor = the level between hits: falls fast (150 ms), rises slowly (1.5 s).
        const float hfDb = static_cast<float>(10.0 * std::log10(hf + 1e-20)) + cal;
        const float totDb = static_cast<float>(10.0 * std::log10(tot + 1e-20)) + cal;
        hfNow = hfDb;
        const double fps = sr / kHop;
        const float up = static_cast<float>(std::exp(-1.0 / (fps * 1.5))), dn = static_cast<float>(std::exp(-1.0 / (fps * 0.15)));
        const float prevFloor = hfFloor;
        hfFloor = hfDb + (hfFloor - hfDb) * (hfDb > hfFloor ? up : dn);
        bool hit = false;
        if (hfDb > -60.0f && hfDb - prevFloor >= 9.0f && hfDb >= totDb - 6.0f && col - lastHitCol >= 6 && frames > 8) {
            hit = true;
            lastHitCol = col;
            hits[hitHead] = {col, static_cast<float>(bestK * sr / kN), hfDb};
            hitHead = (hitHead + 1) % kMaxHits;
            hitCount = std::min(hitCount + 1, kMaxHits);
        }

        // Landscape row (loudest of kLSub FFTs).
        for (int i = 0; i < kLPts; ++i) {
            float m = 0.0f;
            for (int k = ptLo[i]; k <= ptHi[i]; ++k) m = std::max(m, re[k] * re[k] + im[k] * im[k]);
            landPend[i] = std::max(landPend[i], 10.0f * std::log10(m + 1e-20f) + cal);
        }
        if (hit) { pendHarsh = 1; pendHarshHz = static_cast<float>(bestK * sr / kN); }
        if (++landSub == kLSub) {
            landSub = 0;
            std::copy(landPend, landPend + kLPts, land[landHead]);
            landHarsh[landHead] = pendHarsh; landHarshHz[landHead] = pendHarshHz;
            landHead = (landHead + 1) % kLRows;
            ++landRows;
            for (auto& v : landPend) v = -140.0f;
            pendHarsh = 0;
        }
        // Ring tracker, every 24 FFTs (~0.26 s), after the 1 s average has settled.
        if (col % 24 == 0) {
            float hz = 0.0f;
            const float sc = frames >= 40 ? pingSearch(avg, kN, sr, hz) : 0.0f;
            if (sc >= 8.0f) { ringHz = hz; ringScore = sc; } else ringScore = std::max(0.0f, ringScore - 1.5f);
        }
    }
};

Color heat(float t) {   // spectrogram colour map: black → blue → cyan → yellow → orange → red
    t = clampf(t, 0, 1);
    static const Color stops[] = {0x03060AFF, 0x0B2E6BFF, 0x0F8FBFFF, 0x3FD3C8FF, 0xE9D23BFF, 0xFF8A1EFF, 0xFF3B1EFF};
    const float x = t * 6.0f;
    const int i = std::min(5, static_cast<int>(x));
    return Color::lerp(stops[i], stops[i + 1], x - static_cast<float>(i));
}

// ------------------------------------------------------------------ input
struct Input {
    float mx = 0, my = 0;            // image coordinates
    float devX = 0, devY = 0;        // device pixels
    bool down = false, pressed = false, released = false, dbl = false, rpressed = false;
    float wheel = 0;
    bool shift = false, ctrl = false;
};

// ------------------------------------------------------------------ stem slot
struct Slot {
    bool loaded = false;
    std::wstring path;
    std::string name;
    wav::Audio audio;
    std::vector<float> peaks;
    ArtifactProcessor* ds = nullptr;
    ResonanceProcessor* rs = nullptr;
    TransientProcessor* tr = nullptr;
    Eq6* eq = nullptr;
    CompProcessor* comp = nullptr;
    GateProcessor* gate = nullptr;
    DeEsserProcessor* dess = nullptr;
    StemMeterSnapshot meter;
    float dotLevel = 0;
};

// Suno names every stem "<Song> (<Part>)". When all loaded stems share a
// prefix, rows show only the part ("Bass", "Drums") and the song title is
// used for export / session names.
std::string trimName(std::string s) {
    auto junk = [](char c) { return c == ' ' || c == '(' || c == ')' || c == '-' || c == '_' || c == '[' || c == ']' || c == '.'; };
    while (!s.empty() && junk(s.front())) s.erase(s.begin());
    while (!s.empty() && junk(s.back())) s.pop_back();
    return s;
}
std::string commonPrefix(const std::vector<std::string>& v) {
    if (v.empty()) return {};
    std::string p = v[0];
    for (const auto& s : v) {
        std::size_t i = 0;
        while (i < p.size() && i < s.size() && p[i] == s[i]) ++i;
        p.resize(i);
    }
    // cut back to a word boundary so "Two hundred forty days (B" → "Two hundred forty days ("
    while (!p.empty() && p.back() != ' ' && p.back() != '(' && p.back() != '-' && p.back() != '_') p.pop_back();
    return p;
}

enum class Glue { Clean = 0, Punch = 1, Loud = 2 };

struct TargetPreset { const char* name; double lufs; bool normalize; };
constexpr TargetPreset kPresets[] = {
    {"STREAMING -14", -14.0, true}, {"APPLE -16", -16.0, true}, {"CLUB -9", -9.0, true},
    {"BROADCAST -23", -23.0, true}, {"NO NORMALIZE", -14.0, false}};
constexpr int kNumPresets = 5;

// ======================================================================================
class App {
public:
    bool init(HINSTANCE inst, int show);
    void openFromCommandLine();
    int run();
    LRESULT wndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    // audio / engine
    void startAudio();
    void stopAudio();
    void rebuildEngine(double sr, const Session* fromSession = nullptr);
    ChainState captureStem(int s) const;
    ChainState captureMaster() const;
    void publishStemChain(int s, const ChainState* st);
    void publishMasterChain(const ChainState* st);
    StripState stripOf(int s) const;
    void applyGlue();
    static void audioCallback(void* user, float* L, float* R, std::uint32_t n) noexcept;

    // files
    void loadFiles(const std::vector<std::wstring>& files, int slot = -1);
    void loadFolder();
    void loadStemList(std::vector<std::wstring> files);
    void clearSlot(int s);
    void clearAll();
    void saveSessionAs();
    void openSession();
    void exportMaster();
    void onExportDone();
    int loadedCount() const;
    std::uint64_t songLength() const;

    // ui
    void frame();
    void updateTransform();
    void drawTransport();
    void drawArrange();
    void drawDestroyer();
    void drawResonance();
    void drawTransient();
    void drawMastering();

    void drawOverlays();
    float knob(int id, const LY::Knob& k, float value, float mn, float mx, Taper taper, float def, bool bipolar,
               Color accent, const std::string& valueText, bool dim);
    bool button(int id, const Rect& r);
    enum class Icon { Menu, Rewind, Play, Stop, Record, Loop, Analyze, Settings, View, Left, Right };
    // A real (drawn, not painted) hardware-style button: hover, press and lit states. Returns true on click.
    bool hwButton(int id, const LY::Box& b, Icon icon, bool lit = false, Color litColor = kCyan, float iconScale = 1.0f);
    void drawIcon(Icon icon, const Rect& r, Color c, float iconScale);
    void toast(const std::string& s) { toast_ = s; toastUntil_ = now() + 4.0; }
    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void showMainMenu();
    void showSettingsMenu();
    void showAbout();
    void stemContextMenu(int s);
    Slot& sel() { return slots_[static_cast<std::size_t>(selected_)]; }
    std::string shownName(int i) const;
    std::string songTitle() const;

    HWND hwnd_ = nullptr;
    Renderer gfx_;
    Input in_;
    int activeId_ = 0, hotId_ = 0;
    float dragStartY_ = 0, dragStartX_ = 0, dragStartVal_ = 0;
    bool dragMoved_ = false;
    std::string tooltip_;
    float tipX_ = 0, tipY_ = 0;
    // Sections of the 16:9 faceplate. Controls are written in ORIGINAL art
    // coordinates; each section maps them onto the canvas with an offset.
    // Canvas = the desk picture (2880 x 1620). Each screen section maps its controls (written in the
    // ORIGINAL faceplate-art coordinates) onto one of the six monitors. Overlay = window-sized space for
    // toasts and the screen bar.
    enum class Sec { Canvas, Destroyer, NowPlaying, Resonance, Transient, Console, Mastering, Overlay };
    Sec curSec_ = Sec::Canvas, tipSec_ = Sec::Console;
    void setSection(Sec s);
    // The desk: six monitors. focus_ = the screen filling the window (index into DK::kScreens), -1 = whole desk.
    int focus_ = -1;
    static constexpr float kScreenBarUnits = 52.0f;   // height of the screen bar, in Overlay units
    float secScale_ = 1, secOx_ = 0, secOy_ = 0;   // the current section's transform (device = off + coord * scale)
    float hoverGlow_[DK::kNumScreens] = {};
    Rect viewRect() const;
    void setFocus(int screen);
    void drawScreen(int i);
    void drawDesk();          // desk picture + glass of every monitor (before the screens)
    void drawDeskChrome();    // hover / click on the desk, masks and the screen bar (after the screens)
    void drawNowPlaying();
    void feedSpectrum();   // selected stem's tap → spec_ (every frame: DESTROYER's picture, EQ's spectrum, FIND PING)
    void drawStemPicker(int idBase, float x0, float y0, float h, float maxW = 1.0e9f);
    float tabsX0_ = 0;   // left edge of the last tab row drawn (so the stem picker stops before it)
    // Plug-in tabs on the per-stem monitors: DESTROYER | EQ, RESONANCE | DE-ESSER, TRANSIENT | COMP | GATE.
    int screenTab_[DK::kNumScreens] = {};
    int drawTabs(int screen, float x1, float y0, const char* const* names, int n);
    // The four live-drawn plug-ins (panel-local: ox, oy = the panel's top-left; 719 x 455).
    bool panelFrame(int id, float ox, float oy, const char* name, IProcessor* p);
    float panelKnob(int id, float cx, float cy, float r, float v, float mn, float mx, Taper t, float def, bool bipolar,
                    const char* label, const std::string& value, bool dim, Color accent = kCyan);
    void drawEqPanel(float ox, float oy);
    void drawCompPanel(float ox, float oy);
    void drawGatePanel(float ox, float oy);
    void drawDeEssPanel(float ox, float oy);
    void findPing();
    // De-esser meters, per stem (UI side): the loudest "sss" level / cut since the last frame, with ballistics.
    std::array<float, kNumStems> dessLvl_{}, dessHold_{}, dessCut_{};
    std::array<double, kNumStems> dessHoldT_{}, dessCutT_{};
    double lastPoll_ = 0;
    void pollDeEss();
    // FIND HARSH: frequency from the harsh hits heard in the last 8 s, then 3 s of listening to the
    // de-esser's own detector for THRESHOLD (10 dB under the loudest hit), then switch it on.
    int harshStem_ = -1;
    double harshFrom_ = 0, harshUntil_ = 0;
    float harshMax_ = -200.0f, harshHz_ = 0.0f;
    int harshHits_ = 0;
    void findHarsh();
    void updateFindHarsh();
    // 3D SCOPE (DESTROYER screen, third tab): the selected stem as a lit 3D landscape, harsh hits and rings marked.
    float scopeYaw_ = 0.32f, scopePitch_ = 0.52f, scopeYaw0_ = 0, scopePitch0_ = 0;
    float scopeTop_ = -20.0f;   // auto range: dB at the top of the box (follows the stem's loudest point)
    void drawScope3D(float ox, float oy);
    int eqBand_ = 3;   // EQ band the knobs edit (0 HPF, 1 LOW, 2 BELL 1, 3 BELL 2, 4 HIGH, 5 LPF)
    void toggleFullscreen();
    bool fullscreen_ = false;
    // Loop: [loopA_, loopB_) in source frames; loopB_ == 0 means "the whole song".
    bool loopOn_ = false;
    std::uint64_t loopA_ = 0, loopB_ = 0;
    std::uint64_t loopA() const { return loopB_ > loopA_ ? loopA_ : 0; }
    std::uint64_t loopB() const;
    void toggleLoop();
    void setLoopRegion(std::uint64_t a, std::uint64_t b);
    float loopDragX0_ = 0;
    bool loopDragging_ = false;
    WINDOWPLACEMENT savedPlacement_{};
    float viewScale_ = 1, viewOffX_ = 0, viewOffY_ = 0;

    std::unique_ptr<StemEngine> eng_;
    WasapiOutput dev_;
    AsioOutput asio_;
    bool exclusive_ = false, audioRunning_ = false, nullAudio_ = false;
    bool useAsio_ = false, asioOpen_ = false;   // useAsio_: the choice; asioOpen_: ASIO is what's running now
    std::string asioDriver_;
    std::vector<AsioDriverName> asioList_;
    void loadAudioChoice();
    void saveAudioChoice() const;
    std::thread nullThread_;
    std::atomic<bool> nullRun_{false};
    double sr_ = 48000.0;
    std::string deviceDesc_ = "No audio device";

    std::array<Slot, kNumStems> slots_;
    int selected_ = 0;
    WidthProcessor* width_ = nullptr;
    CompProcessor* glue_ = nullptr;
    LimiterProcessor* lim_ = nullptr;
    Glue glueMode_ = Glue::Clean;
    int preset_ = 0;
    double target_ = -14.0;
    bool normalize_ = true;
    double bpm_ = 120.0;
    bool analyze_ = false;
    std::array<bool, kNumStems> rebuildPending_{};
    std::array<ChainState, kNumStems> rebuildState_{};

    Spectrogram spec_;
    int specFor_ = -1;
    std::vector<TransientViz> trHist_;
    std::size_t trHead_ = 0;
    ResonanceView resView_{};
    StemMeterSnapshot master_{};
    float tpHold_[2] = {-90, -90}, lvl_[2] = {-90, -90};
    double tpHoldT_[2] = {0, 0};

    std::string toast_;
    double toastUntil_ = 0;

    // export
    std::thread exportThread_;
    std::atomic<bool> exporting_{false}, exportCancel_{false};
    std::atomic<double> exportProgress_{0.0};
    RenderResult exportResult_;
    std::wstring exportPath_;
    std::string exportError_;

    std::wstring sessionPath_;
};

App* g_app = nullptr;

LRESULT CALLBACK StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app) return g_app->wndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// ====================================================================================== init
bool App::init(HINSTANCE inst, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"HybridCore2EliteDesk";
    RegisterClassExW(&wc);

    // Size the window to the faceplate's aspect inside the monitor work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const float ww = static_cast<float>(work.right - work.left), wh = static_cast<float>(work.bottom - work.top);
    const Rect v = viewRect();   // the whole desk
    const float s = std::min(ww * 0.96f / v.w(), wh * 0.96f / v.h());
    RECT rc{0, 0, static_cast<LONG>(v.w() * s), static_cast<LONG>(v.h() * s)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"HYBRID CORE 2 ELITE — Hybrid Elite Mafia", WS_OVERLAPPEDWINDOW,
                            work.left + (static_cast<int>(ww) - w) / 2, work.top + (static_cast<int>(wh) - h) / 2, w, h,
                            nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    if (auto e = gfx_.init(hwnd_); !e.empty()) {
        MessageBoxW(hwnd_, widen("The graphics engine couldn't start:\n" + e).c_str(), L"Hybrid Core", MB_ICONERROR);
        return false;
    }
    const void* data = nullptr;
    std::size_t size = 0;
    std::vector<std::uint8_t> rgba;
    int iw = 0, ih = 0;
    if (!loadResource(IDR_SKIN, data, size) || !decodeImage(data, size, rgba, iw, ih)) {
        MessageBoxW(hwnd_, L"The faceplate image is missing from this build.", L"Hybrid Core", MB_ICONERROR);
        return false;
    }
    gfx_.setSkin(rgba.data(), iw, ih);
    if (!loadResource(IDR_DESK, data, size) || !decodeImage(data, size, rgba, iw, ih)) {
        MessageBoxW(hwnd_, L"The desk picture is missing from this build.", L"Hybrid Core", MB_ICONERROR);
        return false;
    }
    gfx_.setBackdrop(rgba.data(), iw, ih);

    trHist_.assign(280, TransientViz{});
    loadAudioChoice();
    rebuildEngine(48000.0);
    ShowWindow(hwnd_, show == SW_SHOWMINIMIZED || show == SW_MINIMIZE ? show : SW_SHOWMAXIMIZED);   // 16:9 faceplate: open maximized
    UpdateWindow(hwnd_);
    toast("Click any screen to work on it  -  drag your stems (or their folder) onto the window to load them");
    return true;
}

int App::run() {
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                exportCancel_ = true;
                if (exportThread_.joinable()) exportThread_.join();
                stopAudio();
                if (eng_) eng_->shutdown();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsIconic(hwnd_)) { Sleep(50); continue; }
        frame();
    }
}

// Files passed on the command line (or via "Open with"): WAV stems are
// loaded into slots; a .hcs session is opened. "--play" starts playback.
void App::openFromCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    std::vector<std::wstring> wavs;
    bool play = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        std::wstring low = a;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low == L"--play") { play = true; continue; }
        if (low.size() > 4 && low.substr(low.size() - 4) == L".hcs") {
            Session sn;
            if (loadSession(a, sn, kNumStems).empty()) {
                double rate = 0.0;
                for (std::size_t k = 0; k < kNumStems; ++k) {
                    if (sn.paths[k].empty()) continue;
                    wav::Audio au;
                    if (!wav::read(std::filesystem::path(sn.paths[k]), au).empty()) continue;
                    if (rate == 0.0) rate = au.sampleRate;
                    if (au.sampleRate != rate) continue;
                    Slot& sl = slots_[k];
                    sl.loaded = true; sl.path = sn.paths[k]; sl.name = stemNameFromPath(sn.paths[k]);
                    sl.audio = std::move(au); sl.peaks = computePeaks(sl.audio, 2048);
                }
                bpm_ = sn.bpm; target_ = sn.targetLufs; normalize_ = sn.normalize; preset_ = std::clamp(sn.preset, 0, kNumPresets - 1);
                loopOn_ = sn.loopOn; loopA_ = sn.loopA; loopB_ = sn.loopB;
                rebuildEngine(rate > 0 ? rate : sr_, &sn);
                sessionPath_ = a;
            }
        } else if (low.size() > 4 && low.substr(low.size() - 4) == L".wav") wavs.push_back(a);
    }
    LocalFree(argv);
    if (!wavs.empty()) loadFiles(wavs);
    if (play && loadedCount()) eng_->play();
}

// ====================================================================================== audio
void App::audioCallback(void* user, float* L, float* R, std::uint32_t n) noexcept {
    static_cast<StemEngine*>(user)->process(L, R, n);
}

void App::startAudio() {
    if (audioRunning_ || !eng_) return;
    if (useAsio_ && !asioDriver_.empty()) {
        AsioOutput::Config ac;
        ac.driver = asioDriver_;
        ac.sampleRate = sr_;
        ac.maxBlock = kBlock;
        ac.window = hwnd_;
        std::string e = asio_.open(ac);
        if (e.empty()) e = asio_.start(&App::audioCallback, eng_.get());
        if (e.empty()) {
            deviceDesc_ = asio_.info().description;
            audioRunning_ = true; nullAudio_ = false; asioOpen_ = true;
            return;
        }
        asio_.close();
        toast(e + " - using Windows audio instead");
    }
    WasapiOutput::Config cfg;
    cfg.mode = exclusive_ ? WasapiOutput::Mode::Exclusive : WasapiOutput::Mode::SharedLowLatency;
    cfg.sampleRate = sr_;
    cfg.maxBlock = kBlock;
    std::string err = dev_.open(cfg);
    if (!err.empty() && exclusive_) {
        toast("Exclusive mode unavailable (" + err + ") — using shared mode");
        exclusive_ = false;
        cfg.mode = WasapiOutput::Mode::SharedLowLatency;
        err = dev_.open(cfg);
    }
    if (err.empty()) err = dev_.start(&App::audioCallback, eng_.get());
    if (err.empty()) {
        deviceDesc_ = dev_.info().description;
        audioRunning_ = true; nullAudio_ = false;
        return;
    }
    // No usable output device: keep the engine clocked in real time so meters,
    // chain swaps and editing still work, and say so.
    deviceDesc_ = "No audio output (" + err + ")";
    toast("No audio device available — editing and export still work");
    nullRun_ = true;
    StemEngine* e = eng_.get();
    nullThread_ = std::thread([this, e] {
        std::vector<float> L(512), R(512);
        auto t = std::chrono::steady_clock::now();
        const auto step = std::chrono::duration<double>(512.0 / sr_);
        while (nullRun_.load()) {
            e->process(L.data(), R.data(), 512);
            t += std::chrono::duration_cast<std::chrono::steady_clock::duration>(step);
            std::this_thread::sleep_until(t);
        }
    });
    audioRunning_ = true; nullAudio_ = true;
}

// The audio choice (Windows shared / exclusive / ASIO + driver) is kept in HKCU\Software\HEM\HybridCore2Elite,
// so it survives restarts and upgrades.
void App::loadAudioChoice() {
    char buf[256] = {};
    DWORD bytes = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\HEM\\HybridCore2Elite", "Audio", RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return;
    const std::string v = buf;
    if (v == "exclusive") exclusive_ = true;
    else if (v.rfind("asio:", 0) == 0 && v.size() > 5) { useAsio_ = true; asioDriver_ = v.substr(5); }
}

void App::saveAudioChoice() const {
    const std::string v = useAsio_ ? "asio:" + asioDriver_ : (exclusive_ ? std::string("exclusive") : std::string("shared"));
    HKEY k = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\HEM\\HybridCore2Elite", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExA(k, "Audio", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>(v.size() + 1));
    RegCloseKey(k);
}

void App::stopAudio() {
    if (!audioRunning_) return;
    if (nullAudio_) { nullRun_ = false; if (nullThread_.joinable()) nullThread_.join(); }
    else if (asioOpen_) asio_.close();
    else dev_.close();
    audioRunning_ = false; asioOpen_ = false;
}

StripState App::stripOf(int s) const {
    StripState st;
    if (!eng_) return st;
    auto& e = const_cast<StemEngine&>(*eng_).stem(static_cast<std::size_t>(s));
    st.faderDb = e.faderDb.target(); st.pan = e.pan.target(); st.mute = e.mute.load(); st.solo = e.solo.load();
    return st;
}

ChainState App::captureStem(int s) const {
    const Slot& sl = slots_[static_cast<std::size_t>(s)];
    ChainState c;
    // Chain order (the order the audio goes through): Gate, Destroyer, Resonance, EQ, De-esser, Comp, Transient.
    if (sl.gate) c.push_back(captureProc(*sl.gate));
    if (sl.ds) c.push_back(captureProc(*sl.ds));
    if (sl.rs) c.push_back(captureProc(*sl.rs));
    if (sl.eq) c.push_back(captureProc(*sl.eq));
    if (sl.dess) c.push_back(captureProc(*sl.dess));
    if (sl.comp) c.push_back(captureProc(*sl.comp));
    if (sl.tr) c.push_back(captureProc(*sl.tr));
    return c;
}

ChainState App::captureMaster() const {
    ChainState c;
    if (width_) c.push_back(captureProc(*width_));
    if (glue_) c.push_back(captureProc(*glue_));
    if (lim_) c.push_back(captureProc(*lim_));
    return c;
}

void App::publishStemChain(int s, const ChainState* st) {
    auto reg = makeBuiltinRegistry();
    auto chain = std::make_unique<ProcessorChain>();
    std::unique_ptr<IProcessor> ds, rs, tr, eq, comp, gate, dess;
    if (st) for (const auto& p : *st) {
        if (p.id == "hc.artifact") ds = restoreProc(reg, p);
        else if (p.id == "hc.resonance") rs = restoreProc(reg, p);
        else if (p.id == "hc.transient") tr = restoreProc(reg, p);
        else if (p.id == "hc.eq6") eq = restoreProc(reg, p);
        else if (p.id == "hc.comp") comp = restoreProc(reg, p);
        else if (p.id == "hc.gate") gate = restoreProc(reg, p);
        else if (p.id == "hc.deess") dess = restoreProc(reg, p);
    }
    if (!ds) ds = reg.create("hc.artifact");
    if (!rs) rs = reg.create("hc.resonance");
    if (!tr) tr = reg.create("hc.transient");
    if (!eq) eq = reg.create("hc.eq6");                                     // flat EQ = no change
    // Gate, De-esser and Comp would change the sound at their default settings, so they start OFF.
    if (!comp) { comp = reg.create("hc.comp"); comp->setBypassed(true); }
    if (!gate) { gate = reg.create("hc.gate"); gate->setBypassed(true); }
    if (!dess) { dess = reg.create("hc.deess"); dess->setBypassed(true); }
    Slot& sl = slots_[static_cast<std::size_t>(s)];
    sl.ds = static_cast<ArtifactProcessor*>(ds.get());
    sl.rs = static_cast<ResonanceProcessor*>(rs.get());
    sl.tr = static_cast<TransientProcessor*>(tr.get());
    sl.eq = static_cast<Eq6*>(eq.get());
    sl.comp = static_cast<CompProcessor*>(comp.get());
    sl.gate = static_cast<GateProcessor*>(gate.get());
    sl.dess = static_cast<DeEsserProcessor*>(dess.get());
    chain->add(std::move(gate)); chain->add(std::move(ds)); chain->add(std::move(rs)); chain->add(std::move(eq));
    chain->add(std::move(dess)); chain->add(std::move(comp)); chain->add(std::move(tr));
    eng_->publishChain(static_cast<std::size_t>(s), std::move(chain));
    if (s == selected_) specFor_ = -1;
}

void App::publishMasterChain(const ChainState* st) {
    auto reg = makeBuiltinRegistry();
    auto chain = std::make_unique<ProcessorChain>();
    std::unique_ptr<IProcessor> w, g, l;
    if (st) for (const auto& p : *st) {
        if (p.id == "hc.width") w = restoreProc(reg, p);
        else if (p.id == "hc.comp") g = restoreProc(reg, p);
        else if (p.id == "hc.limiter") l = restoreProc(reg, p);
    }
    if (!w) w = reg.create("hc.width");
    if (!g) { g = reg.create("hc.comp"); g->setBypassed(true); }
    if (!l) l = reg.create("hc.limiter");
    width_ = static_cast<WidthProcessor*>(w.get());
    glue_ = static_cast<CompProcessor*>(g.get());
    lim_ = static_cast<LimiterProcessor*>(l.get());
    chain->add(std::move(w)); chain->add(std::move(g)); chain->add(std::move(l));
    eng_->publishChain(StemEngine::kMaster, std::move(chain));
}

void App::applyGlue() {
    if (!glue_) return;
    switch (glueMode_) {
    case Glue::Clean: glue_->setBypassed(true); break;
    case Glue::Punch:   // gentle 2:1 bus glue
        glue_->param(CompProcessor::kThresh).set(-12); glue_->param(CompProcessor::kRatio).set(2);
        glue_->param(CompProcessor::kKnee).set(6);     glue_->param(CompProcessor::kAttack).set(30);
        glue_->param(CompProcessor::kRelease).set(150); glue_->param(CompProcessor::kMakeup).set(0);
        glue_->setBypassed(false); break;
    case Glue::Loud:    // firmer 3:1, faster
        glue_->param(CompProcessor::kThresh).set(-16); glue_->param(CompProcessor::kRatio).set(3);
        glue_->param(CompProcessor::kKnee).set(6);     glue_->param(CompProcessor::kAttack).set(10);
        glue_->param(CompProcessor::kRelease).set(100); glue_->param(CompProcessor::kMakeup).set(0);
        glue_->setBypassed(false); break;
    }
}

void App::rebuildEngine(double sr, const Session* fromSession) {
    stopAudio();
    std::array<ChainState, kNumStems> chains;
    std::array<StripState, kNumStems> strips;
    ChainState master;
    bool haveState = false;
    if (fromSession) {
        for (std::size_t i = 0; i < kNumStems; ++i) { chains[i] = fromSession->chains[i]; strips[i] = fromSession->strips[i]; }
        master = fromSession->master;
        haveState = true;
    } else if (eng_) {
        for (std::size_t i = 0; i < kNumStems; ++i) { chains[i] = captureStem(static_cast<int>(i)); strips[i] = stripOf(static_cast<int>(i)); }
        master = captureMaster();
        haveState = true;
    }
    if (eng_) eng_->shutdown();
    eng_ = std::make_unique<StemEngine>();
    for (std::size_t i = 0; i < kNumStems; ++i) {
        auto& st = eng_->stem(i);
        st.faderDb.set(strips[i].faderDb); st.pan.set(strips[i].pan);
        st.mute.store(strips[i].mute); st.solo.store(strips[i].solo);
    }
    sr_ = sr;
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    eng_->prepare(sr, kBlock, std::min(hw - 1, 12u), &platform::initRealtimeWorker);
    for (std::size_t i = 0; i < kNumStems; ++i) {
        Slot& sl = slots_[i];
        if (sl.loaded) eng_->setSource(i, sl.audio.ch[0].data(), sl.audio.ch[1].data(), sl.audio.frames());
        publishStemChain(static_cast<int>(i), haveState && !chains[i].empty() ? &chains[i] : nullptr);
    }
    publishMasterChain(haveState && !master.empty() ? &master : nullptr);
    if (fromSession) glueMode_ = static_cast<Glue>(std::clamp(fromSession->glueMode, 0, 2));
    applyGlue();
    rebuildPending_.fill(false);
    specFor_ = -1;
    startAudio();
}


std::string App::shownName(int i) const {
    const Slot& s = slots_[static_cast<std::size_t>(i)];
    if (!s.loaded) return {};
    std::vector<std::string> names;
    for (const auto& x : slots_) if (x.loaded) names.push_back(x.name);
    if (names.size() < 2) return s.name;
    const std::string p = commonPrefix(names);
    if (p.size() < 4 || p.size() >= s.name.size()) return s.name;
    const std::string part = trimName(s.name.substr(p.size()));
    return part.empty() ? s.name : part;
}

std::string App::songTitle() const {
    std::vector<std::string> names;
    for (const auto& x : slots_) if (x.loaded) names.push_back(x.name);
    if (names.empty()) return "HEM";
    std::string t = names.size() > 1 ? trimName(commonPrefix(names)) : names[0];
    if (names.size() == 1) {   // "Song (Vocals)" → "Song"
        if (const auto b = t.rfind(" ("); b != std::string::npos && t.back() == ')') t = t.substr(0, b);
    }
    t = trimName(t);
    return t.empty() ? std::string("HEM") : t;
}

// ====================================================================================== files
int App::loadedCount() const { int n = 0; for (auto& s : slots_) n += s.loaded ? 1 : 0; return n; }
std::uint64_t App::songLength() const { std::uint64_t n = 0; for (auto& s : slots_) if (s.loaded) n = std::max(n, s.audio.frames()); return n; }

void App::loadFiles(const std::vector<std::wstring>& filesIn, int slot) {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    // A finished master ("... HEM Master ...") is the whole song, not a stem: loading it next to the
    // stems plays the song twice (doubled, phasey, too loud). Skip it, every way of loading.
    std::vector<std::wstring> files;
    std::string masters;
    for (const auto& f : filesIn) {
        if (isMasterFile(f)) masters += "  " + narrow(std::filesystem::path(f).filename().wstring()) + "\n";
        else files.push_back(f);
    }
    if (!masters.empty())
        MessageBoxW(hwnd_, widen("Not loaded - this is a finished master of the whole song, not a stem.\n"
                                 "Playing it with the stems would play the song twice:\n\n" + masters).c_str(),
                    L"Hybrid Core", MB_ICONINFORMATION);
    if (files.empty()) return;
    std::vector<int> targets;
    if (slot >= 0) targets.push_back(slot);
    else for (int i = 0; i < static_cast<int>(kNumStems) && targets.size() < files.size(); ++i) if (!slots_[static_cast<std::size_t>(i)].loaded) targets.push_back(i);
    std::string errors;
    std::size_t skipped = files.size() > targets.size() ? files.size() - targets.size() : 0;
    double rate = loadedCount() > 0 ? sr_ : 0.0;
    struct Loaded { int slot; std::wstring path; wav::Audio a; };
    std::vector<Loaded> ok;
    SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_WAIT)));
    for (std::size_t f = 0; f < files.size() && f < targets.size(); ++f) {
        wav::Audio a;
        const std::string e = wav::read(std::filesystem::path(files[f]), a);
        if (!e.empty()) { errors += e + "\n"; continue; }
        if (a.frames() == 0) { errors += stemNameFromPath(files[f]) + ": no audio\n"; continue; }
        if (rate == 0.0) rate = a.sampleRate;
        if (a.sampleRate != rate) {
            errors += stemNameFromPath(files[f]) + fmt(": %.0f Hz", a.sampleRate) + fmt(" (the session is %.0f Hz)\n", rate);
            continue;
        }
        ok.push_back({targets[f], files[f], std::move(a)});
    }
    if (!ok.empty()) {
        stopAudio();
        eng_->stop();
        for (auto& l : ok) {
            Slot& s = slots_[static_cast<std::size_t>(l.slot)];
            s.loaded = true; s.path = l.path; s.name = stemNameFromPath(l.path);
            s.audio = std::move(l.a);
            s.peaks = computePeaks(s.audio, 2048);
        }
        const bool first = loadedCount() == static_cast<int>(ok.size());
        if (rate != sr_) rebuildEngine(rate);
        else {
            for (auto& l : ok) {
                Slot& s = slots_[static_cast<std::size_t>(l.slot)];
                eng_->setSource(static_cast<std::size_t>(l.slot), s.audio.ch[0].data(), s.audio.ch[1].data(), s.audio.frames());
            }
            startAudio();
        }
        eng_->seek(0);
        if (first || slot < 0) {
            std::vector<const wav::Audio*> v;
            for (auto& s : slots_) if (s.loaded) v.push_back(&s.audio);
            bpm_ = estimateTempo(v, sr_);
        }
        if (slot >= 0) selected_ = slot;
        toast(fmt("Loaded %.0f stem", static_cast<double>(ok.size())) + (ok.size() == 1 ? "" : "s") + fmt("  ·  tempo estimate %.1f BPM (drag the tempo box to fix)", bpm_));
    }
    SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
    if (skipped) errors += fmt("%.0f file(s) skipped: all 13 stem slots are full.\n", static_cast<double>(skipped));
    if (!errors.empty()) MessageBoxW(hwnd_, widen("Some files weren't loaded:\n\n" + errors).c_str(), L"Hybrid Core", MB_ICONWARNING);
}

// The file picker SHOWS the WAVs. Pick one stem → every WAV in its folder loads at once; pick several
// (Ctrl-click, Ctrl+A) → just those load. Either way it replaces what's loaded.
void App::loadFolder() {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    std::vector<std::wstring> picked = openFilesDialog(hwnd_, L"Load stems: click any stem to load its whole folder (or select the ones you want)",
                                                       {L"WAV audio (*.wav)", L"*.wav"}, true);
    if (picked.empty()) return;
    std::vector<std::wstring> files;
    if (picked.size() == 1) files = wavsInFolder(std::filesystem::path(picked[0]).parent_path().wstring());
    else { files = std::move(picked); std::sort(files.begin(), files.end()); }
    if (files.empty()) files = picked;   // the one picked file is an exported master: load it anyway, it was asked for
    loadStemList(std::move(files));
}

// Up to 13 WAVs replace what's loaded (dropped folders, picked folders).
void App::loadStemList(std::vector<std::wstring> files) {
    if (files.empty()) { MessageBoxW(hwnd_, L"No WAV files there.\n\nIf the stems are still in a .zip, open the zip and click Extract all first.", L"Hybrid Core", MB_ICONINFORMATION); return; }
    if (files.size() > kNumStems) files.resize(kNumStems);
    if (loadedCount()) clearAll();
    loadFiles(files);
}

void App::clearSlot(int s) {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    stopAudio();
    Slot& sl = slots_[static_cast<std::size_t>(s)];
    eng_->setSource(static_cast<std::size_t>(s), nullptr, nullptr, 0);
    sl.loaded = false; sl.path.clear(); sl.name.clear(); sl.audio = wav::Audio{}; sl.peaks.clear();
    startAudio();
}

void App::clearAll() {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    stopAudio();
    eng_->stop();
    loopOn_ = false; loopA_ = loopB_ = 0;   // a new song gets a fresh loop
    for (std::size_t i = 0; i < kNumStems; ++i) {
        eng_->setSource(i, nullptr, nullptr, 0);
        Slot& sl = slots_[i];
        sl.loaded = false; sl.path.clear(); sl.name.clear(); sl.audio = wav::Audio{}; sl.peaks.clear();
    }
    sessionPath_.clear();
    startAudio();
    eng_->seek(0);
}

void App::saveSessionAs() {
    Session s;
    s.paths.resize(kNumStems); s.chains.resize(kNumStems); s.strips.resize(kNumStems);
    for (std::size_t i = 0; i < kNumStems; ++i) {
        s.paths[i] = slots_[i].loaded ? slots_[i].path : L"";
        s.chains[i] = captureStem(static_cast<int>(i));
        s.strips[i] = stripOf(static_cast<int>(i));
    }
    s.master = captureMaster();
    s.bpm = bpm_; s.targetLufs = target_; s.normalize = normalize_; s.glueMode = static_cast<int>(glueMode_); s.preset = preset_;
    s.loopOn = loopOn_; s.loopA = loopA_; s.loopB = loopB_;
    const std::wstring def = widen(songTitle()) + L" - HEM Session";
    const std::wstring path = saveFileDialog(hwnd_, L"Save Hybrid Core Session", {L"Hybrid Core Session (*.hcs)", L"*.hcs"}, L"hcs",
                                             sessionPath_.empty() ? def : sessionPath_);
    if (path.empty()) return;
    if (auto e = saveSession(path, s); !e.empty()) { MessageBoxW(hwnd_, widen(e).c_str(), L"Hybrid Core", MB_ICONERROR); return; }
    sessionPath_ = path;
    toast("Session saved");
}

void App::openSession() {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    auto files = openFilesDialog(hwnd_, L"Open Hybrid Core Session", {L"Hybrid Core Session (*.hcs)", L"*.hcs"}, false);
    if (files.empty()) return;
    Session s;
    if (auto e = loadSession(files[0], s, kNumStems); !e.empty()) { MessageBoxW(hwnd_, widen(e).c_str(), L"Hybrid Core", MB_ICONERROR); return; }
    stopAudio();
    std::string missing;
    double rate = 0.0;
    for (std::size_t i = 0; i < kNumStems; ++i) {
        Slot& sl = slots_[i];
        sl.loaded = false; sl.audio = wav::Audio{}; sl.peaks.clear(); sl.name.clear(); sl.path.clear();
        if (s.paths[i].empty()) continue;
        wav::Audio a;
        if (auto e = wav::read(std::filesystem::path(s.paths[i]), a); !e.empty()) { missing += narrow(s.paths[i]) + "\n"; continue; }
        if (rate == 0.0) rate = a.sampleRate;
        if (a.sampleRate != rate) { missing += narrow(s.paths[i]) + " (different sample rate)\n"; continue; }
        sl.loaded = true; sl.path = s.paths[i]; sl.name = stemNameFromPath(s.paths[i]);
        sl.audio = std::move(a); sl.peaks = computePeaks(sl.audio, 2048);
    }
    bpm_ = s.bpm; target_ = s.targetLufs; normalize_ = s.normalize; preset_ = std::clamp(s.preset, 0, kNumPresets - 1);
    loopOn_ = s.loopOn; loopA_ = s.loopA; loopB_ = s.loopB;
    rebuildEngine(rate > 0 ? rate : sr_, &s);
    eng_->seek(0);
    sessionPath_ = files[0];
    if (!missing.empty()) MessageBoxW(hwnd_, widen("These stems couldn't be opened:\n\n" + missing).c_str(), L"Hybrid Core", MB_ICONWARNING);
    toast("Session opened");
}

void App::exportMaster() {
    if (exporting_) { exportCancel_ = true; toast("Cancelling export…"); return; }
    if (loadedCount() == 0) { toast("Load some stems first"); return; }
    const std::wstring def = widen(songTitle()) + L" - HEM Master";
    const std::wstring path = saveFileDialog(hwnd_, L"Export Master (24-bit WAV)", {L"WAV audio (*.wav)", L"*.wav"}, L"wav", def);
    if (path.empty()) return;
    RenderJob job;
    job.sampleRate = sr_;
    for (std::size_t i = 0; i < kNumStems; ++i) {
        const Slot& s = slots_[i];
        if (!s.loaded) continue;
        StemJob sj;
        sj.L = s.audio.ch[0].data(); sj.R = s.audio.ch[1].data(); sj.frames = s.audio.frames();
        sj.chain = captureStem(static_cast<int>(i));
        const StripState st = stripOf(static_cast<int>(i));
        sj.faderDb = st.faderDb; sj.pan = st.pan; sj.mute = st.mute; sj.solo = st.solo;
        job.stems.push_back(std::move(sj));
    }
    job.master = captureMaster();
    job.normalize = normalize_;
    job.targetLufs = target_;
    if (exportThread_.joinable()) exportThread_.join();
    exportPath_ = path;
    exportCancel_ = false;
    exportProgress_ = 0.0;
    exporting_ = true;
    HWND hwnd = hwnd_;
    exportThread_ = std::thread([this, job = std::move(job), path, hwnd]() mutable {
        exportResult_ = renderMix(job, [this](double p) { exportProgress_ = p; return !exportCancel_.load(); });
        exportError_.clear();
        if (exportResult_.ok) {
            if (auto e = wav::write(std::filesystem::path(path), exportResult_.audio, false); !e.empty()) exportError_ = e;
            exportResult_.audio = wav::Audio{};
        }
        PostMessageW(hwnd, WM_EXPORT_DONE, 0, 0);
    });
}

void App::onExportDone() {
    if (exportThread_.joinable()) exportThread_.join();
    exporting_ = false;
    if (exportResult_.cancelled) { toast("Export cancelled"); return; }
    if (!exportResult_.ok || !exportError_.empty()) {
        MessageBoxW(hwnd_, widen("Export failed: " + (exportError_.empty() ? exportResult_.error : exportError_)).c_str(), L"Hybrid Core", MB_ICONERROR);
        return;
    }
    const auto& r = exportResult_.reading;
    std::string msg = "Master exported (24-bit WAV).\n\n";
    msg += "Integrated:   " + lufsStr(r.integrated) + " LUFS" + (normalize_ ? fmt("   (target %.1f)", target_) : std::string("   (not normalized)")) + "\n";
    msg += "True peak:    " + fmt("%.2f dBTP", r.truePeakDb) + "\n";
    msg += "Short-term max: " + lufsStr(r.maxShortTerm) + " LUFS\n";
    msg += "LRA:          " + fmt("%.1f LU", r.lra) + "\n\n" + narrow(exportPath_);
    MessageBoxW(hwnd_, widen(msg).c_str(), L"Hybrid Core — Export complete", MB_ICONINFORMATION);
}

// ====================================================================================== menus
enum MenuId {
    ID_LOAD = 100, ID_LOAD_FOLDER = 99, ID_CLEAR_ALL = 101, ID_OPEN_SESSION, ID_SAVE_SESSION, ID_EXPORT, ID_EXIT,
    ID_SHARED, ID_EXCLUSIVE, ID_RESET_METERS, ID_ASIO_FIRST = 7000, ID_ASIO_LAST = 7063,
    ID_AFTER_ASIO = 7100, ID_SLOT_LOAD, ID_SLOT_CLEAR, ID_SLOT_RESET, ID_VIEW_TOGGLE
};

void App::showMainMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_LOAD_FOLDER, L"Load Stems (pick one = its whole folder)…\tCtrl+L");
    AppendMenuW(m, MF_STRING, ID_LOAD, L"Add Stem Files…\tCtrl+Shift+L");
    AppendMenuW(m, MF_STRING | (loadedCount() ? 0 : MF_GRAYED), ID_CLEAR_ALL, L"Clear All Stems");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPEN_SESSION, L"Open Session…\tCtrl+O");
    AppendMenuW(m, MF_STRING, ID_SAVE_SESSION, L"Save Session…\tCtrl+S");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (loadedCount() ? 0 : MF_GRAYED), ID_EXPORT, exporting_ ? L"Cancel Export" : L"Export Master…\tCtrl+E");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_VIEW_TOGGLE, fullscreen_ ? L"Exit Full Screen\tF11" : L"Full Screen\tF11");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case ID_LOAD_FOLDER: loadFolder(); break;
    case ID_LOAD: loadFiles(openFilesDialog(hwnd_, L"Load Stems (up to 13 WAV files)", {L"WAV audio (*.wav)", L"*.wav"}, true)); break;
    case ID_CLEAR_ALL: clearAll(); break;
    case ID_OPEN_SESSION: openSession(); break;
    case ID_SAVE_SESSION: saveSessionAs(); break;
    case ID_EXPORT: exportMaster(); break;
    case ID_VIEW_TOGGLE: toggleFullscreen(); break;
    case ID_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void App::showSettingsMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (!asioOpen_ && !exclusive_ ? MF_CHECKED : 0), ID_SHARED, L"Audio: Windows, Shared (plays with other apps)");
    AppendMenuW(m, MF_STRING | (!asioOpen_ && exclusive_ ? MF_CHECKED : 0), ID_EXCLUSIVE, L"Audio: Windows, Exclusive (lower latency)");
    asioList_ = listAsioDrivers();
    if (!asioAvailable()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"ASIO: not in this build");
    else if (asioList_.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"ASIO: no ASIO driver installed");
    else
        for (std::size_t i = 0; i < asioList_.size() && i <= ID_ASIO_LAST - ID_ASIO_FIRST; ++i)
            AppendMenuW(m, MF_STRING | (asioOpen_ && asioList_[i].name == asioDriver_ ? MF_CHECKED : 0), ID_ASIO_FIRST + static_cast<UINT>(i),
                        widen("Audio: ASIO - " + asioList_[i].name).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen(deviceDesc_).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen("Graphics: " + gfx_.adapterName()).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen(fmt("Plugin latency: %.0f samples", eng_ ? eng_->totalLatency() : 0)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_RESET_METERS, L"Reset Loudness Meters");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == ID_SHARED || cmd == ID_EXCLUSIVE) {
        const bool want = cmd == ID_EXCLUSIVE;
        if (want != exclusive_ || asioOpen_) { stopAudio(); useAsio_ = false; exclusive_ = want; startAudio(); toast(deviceDesc_); saveAudioChoice(); }
    } else if (cmd >= ID_ASIO_FIRST && cmd <= ID_ASIO_LAST && static_cast<std::size_t>(cmd - ID_ASIO_FIRST) < asioList_.size()) {
        stopAudio();
        useAsio_ = true;
        asioDriver_ = asioList_[static_cast<std::size_t>(cmd - ID_ASIO_FIRST)].name;
        startAudio();
        toast(deviceDesc_);
        saveAudioChoice();
    } else if (cmd == ID_RESET_METERS && eng_) { eng_->requestMeterReset(); toast("Meters reset"); }
}

void App::showAbout() {
    const std::string s = std::string("THE HYBRID CORE ") + kVersion + "\nHybrid Elite Mafia Studio Suite\n\n"
        "Scratch-built C++20 audio engine · WASAPI · AVX2 · Direct3D 11\n\n"
        "Keys:\n  Space  play / stop\n  Home  back to start\n  Ctrl+L  load stems    Ctrl+E  export master\n"
        "  Ctrl+O / Ctrl+S  open / save session\n\n"
        "Knobs: drag up/down (Shift = fine), mouse wheel, double-click = default.\n"
        "Track name: click = select, drag left/right = volume, double-click = 0 dB, right-click = menu.\n"
        "Plugin titles: click to bypass.";
    MessageBoxW(hwnd_, widen(s).c_str(), L"About Hybrid Core", MB_ICONINFORMATION);
}

void App::stemContextMenu(int s) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SLOT_LOAD, slots_[static_cast<std::size_t>(s)].loaded ? L"Replace Stem…" : L"Load Stem…");
    AppendMenuW(m, MF_STRING | (slots_[static_cast<std::size_t>(s)].loaded ? 0 : MF_GRAYED), ID_SLOT_CLEAR, L"Remove Stem");
    AppendMenuW(m, MF_STRING, ID_SLOT_RESET, L"Reset Volume to 0 dB");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == ID_SLOT_LOAD) loadFiles(openFilesDialog(hwnd_, L"Load Stem", {L"WAV audio (*.wav)", L"*.wav"}, false), s);
    else if (cmd == ID_SLOT_CLEAR) clearSlot(s);
    else if (cmd == ID_SLOT_RESET) eng_->stem(static_cast<std::size_t>(s)).faderDb.set(0);
}

// ====================================================================================== widgets
bool App::button(int id, const Rect& r) {
    const bool hot = r.contains(in_.mx, in_.my);
    if (hot) hotId_ = id;
    if (hot && in_.pressed) { activeId_ = id; return false; }
    if (activeId_ == id && in_.released) { activeId_ = 0; return hot; }
    return false;
}

bool App::hwButton(int id, const LY::Box& b, Icon icon, bool lit, Color litColor, float iconScale) {
    const Rect r = R(b);
    const bool clicked = button(id, r);
    const bool hot = hotId_ == id, down = activeId_ == id && r.contains(in_.mx, in_.my);
    const float rad = std::min(6.0f, 0.2f * std::min(r.w(), r.h()));
    gfx_.roundRect({r.x0, r.y0 + 1.5f, r.x1, r.y1 + 1.5f}, rad, Color(0x000000FF).alpha(0.55f));     // drop shadow
    gfx_.roundRect(r, rad, down ? Color(0x0B0F13FF) : Color(0x2A333CFF));                           // bevel edge
    const Rect face = r.inset(1.5f);
    gfx_.roundRect(face, rad - 1.0f, down ? Color(0x0E1317FF) : Color(0x161C22FF));
    if (!down) gfx_.gradientV({face.x0 + rad * 0.5f, face.y0 + 1.0f, face.x1 - rad * 0.5f, face.cy()}, Color(0xFFFFFF10), Color(0xFFFFFF00));
    if (lit) {
        gfx_.roundRect(face, rad - 1.0f, litColor.alpha(down ? 0.16f : 0.22f));
        gfx_.rectOutline(face.inset(0.5f), litColor.alpha(0.55f), 1.2f);
    } else if (hot) {
        gfx_.roundRect(face, rad - 1.0f, Color(0xFFFFFFFF).alpha(0.06f));
    }
    Rect ir = r;
    if (down) ir = {r.x0, r.y0 + 1.0f, r.x1, r.y1 + 1.0f};
    Color ic = lit ? litColor : (hot ? Color(0xFFFFFFFF) : kText);
    if (icon == Icon::Record) ic = lit || hot ? Color(0xFF5A4AFF) : kRed;
    drawIcon(icon, ir, ic, iconScale);
    return clicked;
}

void App::drawIcon(Icon icon, const Rect& r, Color c, float iconScale) {
    const float cx = r.cx(), cy = r.cy();
    const float s = 0.21f * std::min(r.w(), r.h()) * iconScale;   // icon half-size
    const float t = std::max(2.0f, s * 0.22f);                     // stroke
    switch (icon) {
    case Icon::Menu:
        for (int i = -1; i <= 1; ++i) gfx_.line(cx - s, cy + i * s * 0.62f, cx + s, cy + i * s * 0.62f, t, c);
        break;
    case Icon::Rewind:
        gfx_.rect({cx - s, cy - s * 0.85f, cx - s + t * 1.1f, cy + s * 0.85f}, c);
        gfx_.triangle(cx + s * 0.9f, cy - s * 0.85f, cx + s * 0.9f, cy + s * 0.85f, cx - s * 0.55f, cy, c);
        break;
    case Icon::Play:
        gfx_.triangle(cx - s * 0.65f, cy - s, cx - s * 0.65f, cy + s, cx + s * 0.95f, cy, c);
        break;
    case Icon::Stop:
        gfx_.roundRect({cx - s * 0.8f, cy - s * 0.8f, cx + s * 0.8f, cy + s * 0.8f}, 1.5f, c);
        break;
    case Icon::Record:
        gfx_.circle(cx, cy, s * 0.92f, c);
        break;
    case Icon::Loop: {
        // Circular arrow: a ring with a gap at the top right and an arrowhead pointing round the circle.
        const float R = s * 0.85f, a0 = -1.05f, a1 = a0 + 5.15f;
        gfx_.arc(cx, cy, R, a0 + 0.35f, a1, t, c);
        const float ax = cx + R * std::cos(a0 + 0.35f), ay = cy + R * std::sin(a0 + 0.35f);
        const float tx = -std::sin(a0 + 0.35f), ty = std::cos(a0 + 0.35f);     // tangent (direction of travel reversed)
        const float nx = std::cos(a0 + 0.35f), ny = std::sin(a0 + 0.35f);
        const float h = s * 0.62f;
        gfx_.triangle(ax - tx * h, ay - ty * h, ax + nx * h * 0.62f, ay + ny * h * 0.62f, ax - nx * h * 0.62f, ay - ny * h * 0.62f, c);
        break;
    }
    case Icon::Analyze: {
        const float w = s * 1.45f;
        const float xs[] = {-1.0f, -0.62f, -0.4f, -0.18f, 0.05f, 0.25f, 0.45f, 0.68f, 1.0f};
        const float ys[] = {0.1f, 0.1f, -0.75f, 0.6f, -0.95f, 0.45f, -0.4f, 0.1f, 0.1f};
        float pts[18];
        for (int i = 0; i < 9; ++i) { pts[2 * i] = cx + xs[i] * w; pts[2 * i + 1] = cy + ys[i] * s; }
        gfx_.polyline(pts, 9, t * 0.8f, c);
        break;
    }
    case Icon::Settings: {
        const float R = s * 0.62f;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.7853982f;
            gfx_.line(cx + std::cos(a) * R * 0.6f, cy + std::sin(a) * R * 0.6f, cx + std::cos(a) * s, cy + std::sin(a) * s, t * 1.25f, c);
        }
        gfx_.circle(cx, cy, R, c);
        gfx_.circle(cx, cy, R * 0.42f, Color(0x161C22FF));
        break;
    }
    case Icon::View:
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j) {
                const float x = cx + i * s * 0.72f, y = cy + j * s * 0.72f, h = s * 0.24f;
                gfx_.rect({x - h, y - h, x + h, y + h}, c);
            }
        break;
    case Icon::Left:
        gfx_.line(cx + s * 0.35f, cy - s * 0.7f, cx - s * 0.35f, cy, t * 0.8f, c);
        gfx_.line(cx - s * 0.35f, cy, cx + s * 0.35f, cy + s * 0.7f, t * 0.8f, c);
        break;
    case Icon::Right:
        gfx_.line(cx - s * 0.35f, cy - s * 0.7f, cx + s * 0.35f, cy, t * 0.8f, c);
        gfx_.line(cx + s * 0.35f, cy, cx - s * 0.35f, cy + s * 0.7f, t * 0.8f, c);
        break;
    }
}

float App::knob(int id, const LY::Knob& k, float value, float mn, float mx, Taper taper, float def, bool bipolar,
                Color accent, const std::string& valueText, bool dim) {
    auto toNorm = [&](float v) {
        if (taper == Taper::Log && mn > 0) return std::log(v / mn) / std::log(mx / mn);
        return (v - mn) / (mx - mn);
    };
    auto fromNorm = [&](float n) {
        n = clampf(n, 0, 1);
        if (taper == Taper::Log && mn > 0) return mn * std::pow(mx / mn, n);
        return mn + n * (mx - mn);
    };
    const float r = k.r - 8.0f;
    const float dx = in_.mx - k.cx, dy = in_.my - k.cy;
    const bool hot = dx * dx + dy * dy <= r * r;
    float norm = clampf(toNorm(value), 0, 1);
    if (hot) hotId_ = id;
    if (hot && in_.dbl) { norm = clampf(toNorm(def), 0, 1); activeId_ = 0; }
    else if (hot && in_.pressed) { activeId_ = id; dragStartY_ = in_.devY; dragStartVal_ = norm; }
    if (activeId_ == id) {
        if (in_.down) norm = clampf(dragStartVal_ + (dragStartY_ - in_.devY) / (in_.shift ? 1200.0f : 240.0f), 0, 1);
        if (in_.released) activeId_ = 0;
    }
    if (hot && in_.wheel != 0) norm = clampf(norm + in_.wheel * (in_.shift ? 0.005f : 0.025f), 0, 1);
    const float out = fromNorm(norm);

    // ---- draw: tick dots, track ring, value arc, metal body, pointer ----
    const float a0 = 0.75f * 3.14159265f, sweep = 1.5f * 3.14159265f;
    const float ringR = k.r - 12.0f, bodyR = k.r - 22.0f;
    const float alpha = dim ? 0.35f : 1.0f;
    for (int i = 0; i <= 10; ++i) {
        const float a = a0 + sweep * i / 10.0f;
        gfx_.circle(k.cx + (k.r - 3.0f) * std::cos(a), k.cy + (k.r - 3.0f) * std::sin(a), i % 5 == 0 ? 2.2f : 1.4f, Color(0x8A9AA6FF).alpha(0.7f * alpha));
    }
    gfx_.arc(k.cx, k.cy, ringR, a0, a0 + sweep, 6.0f, Color(0x16232BFF).alpha(alpha));
    const float va = a0 + sweep * norm;
    const float from = bipolar ? a0 + sweep * 0.5f : a0;
    if (std::fabs(va - from) > 0.01f) {
        gfx_.arc(k.cx, k.cy, ringR, std::min(from, va), std::max(from, va), 12.0f, accent.alpha(0.18f * alpha));
        gfx_.arc(k.cx, k.cy, ringR, std::min(from, va), std::max(from, va), 5.0f, accent.alpha(alpha));
    }
    gfx_.circle(k.cx, k.cy + 2.0f, bodyR + 3.0f, Color(0x000000FF).alpha(0.6f));
    gfx_.circleGradient(k.cx, k.cy, bodyR + 1.5f, Color(0x2A2E33FF), Color(0x0B0D10FF));
    gfx_.circleGradient(k.cx, k.cy, bodyR - 1.0f, Color(0xD5DADFFF).alpha(alpha), Color(0x3A3F45FF).alpha(alpha));
    gfx_.circleGradient(k.cx, k.cy, bodyR - 7.0f, Color(0x555B62FF).alpha(alpha), Color(0xBFC4CAFF).alpha(alpha));
    const float px = std::cos(va), py = std::sin(va);
    gfx_.line(k.cx + px * bodyR * 0.25f, k.cy + py * bodyR * 0.25f, k.cx + px * (bodyR - 3.0f), k.cy + py * (bodyR - 3.0f), 3.5f, Color(0xFFFFFFFF).alpha(alpha));
    if (hot || activeId_ == id) { tooltip_ = valueText; tipX_ = k.cx; tipY_ = k.cy - k.r - 6.0f; tipSec_ = curSec_; }
    return out;
}

// ====================================================================================== frame
Rect App::viewRect() const {
    if (focus_ >= 0) return DK::screenDst(focus_);
    return {0, 0, DK::kDeskW, DK::kDeskH};
}

void App::setFocus(int screen) {
    focus_ = screen;
    activeId_ = 0;
    if (screen >= 0 && screen < DK::kNumScreens)
        toast(std::string(DK::kScreens[screen].name) + "  -  Esc or DESK goes back  -  Tab = next screen");
}

void App::updateTransform() {
    const float W = static_cast<float>(gfx_.width()), H = static_cast<float>(gfx_.height());
    const Rect v = viewRect();
    // A screen in focus sits below the screen bar (< name DESK >), so the bar never covers its controls.
    const float bar = focus_ >= 0 ? kScreenBarUnits * W / 1438.0f : 0.0f;
    const float s = std::min(W / v.w(), (H - bar) / v.h());
    viewScale_ = s;
    viewOffX_ = 0.5f * (W - v.w() * s) - v.x0 * s;
    viewOffY_ = bar + 0.5f * (H - bar - v.h() * s) - v.y0 * s;
    // Glyphs are rasterised for the screen in focus (on the whole desk: at the panels' size), so the
    // text you are working with is always pixel-sharp.
    gfx_.setTextScale(s * DK::screenScale(focus_ >= 0 ? focus_ : DK::kDestroyer));
    setSection(Sec::Canvas);
}

void App::setSection(Sec sec) {
    curSec_ = sec;
    float scale = viewScale_, ox = viewOffX_, oy = viewOffY_;
    int screen = -1;
    switch (sec) {
    case Sec::Canvas: break;
    case Sec::Overlay: {
        // Window space, 1438 units wide (the size the old CONSOLE page used), so toasts look the same everywhere.
        scale = static_cast<float>(gfx_.width()) / 1438.0f;
        ox = oy = 0;
        break;
    }
    case Sec::Destroyer: screen = DK::kDestroyer; break;
    case Sec::NowPlaying: screen = DK::kNowPlaying; break;
    case Sec::Resonance: screen = DK::kResonance; break;
    case Sec::Transient: screen = DK::kTransient; break;
    case Sec::Console: screen = DK::kConsole; break;
    case Sec::Mastering: screen = DK::kMastering; break;
    }
    if (screen >= 0) {
        const float k = DK::screenScale(screen);
        const Rect d = DK::screenDst(screen), src = DK::kScreens[screen].src;
        scale = viewScale_ * k;
        ox = viewOffX_ + (d.x0 - src.x0 * k) * viewScale_;
        oy = viewOffY_ + (d.y0 - src.y0 * k) * viewScale_;
    }
    gfx_.setTransform(scale, ox, oy);
    secScale_ = scale; secOx_ = ox; secOy_ = oy;
    in_.mx = (in_.devX - ox) / scale;
    in_.my = (in_.devY - oy) / scale;
    // Only the screen in focus takes the mouse. On the whole desk a click opens a screen instead.
    if (screen >= 0 && screen != focus_) in_.mx = in_.my = -1.0e6f;
}

void App::toggleFullscreen() {
    if (!fullscreen_) {
        savedPlacement_.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        fullscreen_ = false;
    }
}

void App::frame() {
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
    in_.devX = static_cast<float>(p.x); in_.devY = static_cast<float>(p.y);
    updateTransform();
    in_.down = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    in_.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    in_.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!in_.down && activeId_ && !in_.released) in_.released = true;   // button released outside the window
    hotId_ = 0;
    tooltip_.clear();

    // Pending chain rebuilds (resolution changes) — retry until the audio thread takes them.
    for (std::size_t i = 0; i < kNumStems; ++i)
        if (rebuildPending_[i]) { rebuildPending_[i] = false; publishStemChain(static_cast<int>(i), &rebuildState_[i]); }
    if (eng_) eng_->collectGarbage();

    // Meters.
    for (std::size_t i = 0; i < kNumStems; ++i) {
        auto& tb = eng_->stem(i).snapshot;
        tb.update();
        slots_[i].meter = tb.read_buffer();
        const float pk = std::max(slots_[i].meter.peakDb[0], slots_[i].meter.peakDb[1]);
        const float lv = clampf((pk + 48.0f) / 48.0f, 0, 1);
        slots_[i].dotLevel = std::max(lv, slots_[i].dotLevel * 0.9f);
    }
    eng_->masterSnapshot().update();
    master_ = eng_->masterSnapshot().read_buffer();

    feedSpectrum();
    pollDeEss();
    updateFindHarsh();
    gfx_.beginFrame(0x040506FF);
    drawDesk();
    for (int i = 0; i < DK::kNumScreens; ++i) drawScreen(i);
    drawDeskChrome();
    drawOverlays();
    gfx_.endFrame();

    in_.pressed = in_.released = in_.dbl = in_.rpressed = false;
    in_.wheel = 0;
    // Stop at the end of the song.
    if (eng_->isPlaying() && loadedCount() && eng_->playhead() > songLength() + eng_->totalLatency() + static_cast<std::uint64_t>(sr_ * 0.25))
        eng_->stop();
}

// ------------------------------------------------------------------ transport
void App::drawTransport() {
    const bool playing = eng_->isPlaying();
    // Every button here is drawn live (the painted ones were cut out of the art).
    if (hwButton(1, LY::btnMenu, Icon::Menu) || hwButton(2, LY::btnTopLeft, Icon::Menu, false, kCyan, 0.85f)) showMainMenu();
    if (hwButton(3, LY::btnRewind, Icon::Rewind)) eng_->seek(loopOn_ ? loopA() : 0);
    if (hwButton(4, LY::btnPlay, Icon::Play, playing)) {
        if (!loadedCount()) loadFolder();
        else eng_->play();
    }
    if (hwButton(5, LY::btnStop, Icon::Stop)) { if (!playing) eng_->seek(loopOn_ ? loopA() : 0); eng_->stop(); }
    {
        const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(now()) * 6.0f);
        if (hwButton(6, LY::btnRecord, Icon::Record, exporting_, Color(kRed).alpha(0.4f + 0.6f * blink))) exportMaster();
    }
    if (hwButton(11, LY::btnLoop, Icon::Loop, loopOn_)) toggleLoop();
    if (hwButton(7, LY::btnAnalyze, Icon::Analyze, analyze_)) { analyze_ = !analyze_; toast(analyze_ ? "Analyze: per-stem loudness shown on the lanes" : "Analyze off"); }
    if (hwButton(8, LY::btnSettings, Icon::Settings)) showSettingsMenu();
    if (hwButton(9, LY::btnView, Icon::View, fullscreen_)) toggleFullscreen();
    if (hwButton(10, LY::btnTopRight, Icon::Menu, false, kCyan, 0.85f)) showAbout();
    if (hotId_ == 11) { tooltip_ = loopOn_ ? "LOOP on (L) - drag on the bar numbers to set the section" : "LOOP (L) - drag on the bar numbers to set the section";
                        tipX_ = LY::btnLoop.x0 + 30; tipY_ = LY::btnLoop.y1 + 34; tipSec_ = curSec_; }

    // Time display (audible position = engine playhead minus plugin latency).
    const double lat = eng_->totalLatency();
    const double pos = std::max(0.0, static_cast<double>(eng_->playhead()) - (playing ? lat : 0.0)) / sr_;
    gfx_.textIn(R(LY::timeDisplay), timeStr(pos), Font::Huge, kCyan);

    // Tempo box: drag up/down to change, double-click to re-estimate.
    const Rect tb = R(LY::tempoDisplay);
    const bool tHot = tb.contains(in_.mx, in_.my);
    if (tHot) hotId_ = 20;
    if (tHot && in_.dbl && loadedCount()) {
        std::vector<const wav::Audio*> v;
        for (auto& s : slots_) if (s.loaded) v.push_back(&s.audio);
        bpm_ = estimateTempo(v, sr_);
        toast(fmt("Tempo re-estimated: %.1f BPM", bpm_));
    } else if (tHot && in_.pressed) { activeId_ = 20; dragStartY_ = in_.devY; dragStartVal_ = static_cast<float>(bpm_); }
    if (activeId_ == 20) {
        if (in_.down) bpm_ = std::clamp(std::round((dragStartVal_ + (dragStartY_ - in_.devY) * (in_.shift ? 0.02f : 0.2f)) * 10.0) / 10.0, 40.0, 240.0);
        if (in_.released) activeId_ = 0;
    }
    if (tHot && in_.wheel != 0) bpm_ = std::clamp(bpm_ + in_.wheel * (in_.shift ? 0.1 : 1.0), 40.0, 240.0);
    gfx_.textIn({tb.x0, tb.y0 + 2, tb.x1, tb.y0 + 30}, fmt("%.2f", bpm_), Font::Large, kText);
    gfx_.textIn({tb.x0, tb.y0 + 28, tb.x1, tb.y1 - 2}, "4/4", Font::Small, kDim);
    if (tHot || activeId_ == 20) { tooltip_ = "Tempo (estimate) · drag / wheel to set · double-click to re-detect"; tipX_ = tb.cx(); tipY_ = tb.y1 + 34; tipSec_ = curSec_; }

    // Master output level meter: two rows of segments, peak with 20 dB/s fall.
    const Rect lm = R(LY::levelMeter);
    const int segs = 36;
    for (int ch = 0; ch < 2; ++ch) {
        const float pk = master_.peakDb[ch];
        lvl_[ch] = std::max(std::isfinite(pk) ? pk : -90.0f, lvl_[ch] - 0.33f);
        const float y0 = lm.y0 + 9 + ch * 16.0f;
        const float sw = (lm.w() - 16) / segs;
        for (int i = 0; i < segs; ++i) {
            const float db = -48.0f + 48.0f * (i + 1) / segs;
            const bool lit = lvl_[ch] >= db - 48.0f / segs;
            Color c = db > -3 ? kRed : (db > -9 ? kOrange : (db > -18 ? kYellow : kCyan));
            gfx_.rect({lm.x0 + 8 + i * sw, y0, lm.x0 + 8 + (i + 1) * sw - 2.0f, y0 + 12}, lit ? c : Color(c).alpha(0.10f));
        }
    }
}

// ------------------------------------------------------------------ loop
std::uint64_t App::loopB() const {
    const std::uint64_t len = songLength();
    if (loopB_ > loopA_) return std::min(loopB_, len);
    return len;
}

void App::setLoopRegion(std::uint64_t a, std::uint64_t b) { loopA_ = a; loopB_ = b; }

void App::toggleLoop() {
    loopOn_ = !loopOn_;
    if (!loopOn_) { toast("LOOP off"); return; }
    if (!loadedCount()) { toast("LOOP on - load stems, then drag on the bar numbers to pick the section"); return; }
    const std::uint64_t ph = eng_->playhead();
    eng_->setLoop(true, loopA(), loopB());
    if (ph < loopA() || ph >= loopB()) eng_->seek(loopA());
    toast(loopB_ > loopA_ ? "LOOP on" : "LOOP on: whole song - drag on the bar numbers to loop a section");
}

// ------------------------------------------------------------------ arrange
void App::drawArrange() {
    const Rect lanes{LY::lanes.x0, LY::lanes.y0, LY::lanes.x1, LY::kRowsY1 - 4}, ruler = R(LY::ruler);
    const float lx0 = lanes.x0 + 4, lx1 = lanes.x1 - 4;
    const std::uint64_t len = songLength();
    const double secs = len ? static_cast<double>(len) / sr_ : 0.0;
    gfx_.rect(lanes, kScreen);
    gfx_.rect(ruler, 0x0A1117FF);
    gfx_.line(ruler.x0, ruler.y1, ruler.x1, ruler.y1, 1.5f, 0x1E2C36FF);

    // Bar grid + ruler numbers (from the tempo box).
    if (len) {
        const double barSec = 4.0 * 60.0 / bpm_;
        const int bars = static_cast<int>(secs / barSec) + 1;
        const float pxPerBar = static_cast<float>((lx1 - lx0) * barSec / secs);
        int every = 1;
        while (every * pxPerBar < 44.0f) every *= 2;
        for (int b = 0; b <= bars; b += every) {
            const float x = lx0 + static_cast<float>(b * barSec / secs) * (lx1 - lx0);
            if (x > lx1) break;
            gfx_.line(x, lanes.y0, x, lanes.y1, 1.0f, Color(0x1A2730FF));
            gfx_.line(x, ruler.y1 - 8, x, ruler.y1, 1.5f, kDim);
            gfx_.text(x + 4, ruler.y0 + 4, std::to_string(b + 1), Font::Small, kText);
        }
    }

    // Loop section: highlighted on the ruler (cyan when LOOP is on), edges marked down the lanes.
    if (len) {
        eng_->setLoop(loopOn_, loopA(), loopB());
        if (loopOn_ || loopB_ > loopA_) {
            const float xa = lx0 + static_cast<float>(static_cast<double>(loopA()) / static_cast<double>(len)) * (lx1 - lx0);
            const float xb = lx0 + static_cast<float>(static_cast<double>(loopB()) / static_cast<double>(len)) * (lx1 - lx0);
            const Color lc = loopOn_ ? kCyan : kDim;
            gfx_.rect({xa, ruler.y0 + 2, xb, ruler.y1 - 1}, lc.alpha(loopOn_ ? 0.30f : 0.16f));
            gfx_.rect({xa, lanes.y0, xb, lanes.y1}, lc.alpha(loopOn_ ? 0.05f : 0.025f));
            for (float x : {xa, xb}) {
                gfx_.line(x, ruler.y0 + 2, x, ruler.y1, 2.5f, lc.alpha(0.95f));
                gfx_.line(x, lanes.y0, x, lanes.y1, 1.0f, lc.alpha(loopOn_ ? 0.45f : 0.25f));
            }
        }
    } else eng_->setLoop(false, 0, 0);

    // Ruler: click = jump there; drag = set the loop section (snaps to whole bars, Shift = free) and turn LOOP on.
    auto frameAt = [&](float x) {
        return static_cast<std::uint64_t>(clampf((x - lx0) / (lx1 - lx0), 0, 1) * static_cast<double>(len));
    };
    const double barFrames = 4.0 * 60.0 / bpm_ * sr_;
    auto snapBar = [&](std::uint64_t f, bool up) {
        if (in_.shift || barFrames < 1.0) return f;
        const double b = static_cast<double>(f) / barFrames;
        return std::min<std::uint64_t>(len, static_cast<std::uint64_t>((up ? std::ceil(b - 1e-9) : std::floor(b + 1e-9)) * barFrames));
    };
    const Rect rulerHit{lx0, ruler.y0, lx1, ruler.y1};
    if (len && rulerHit.contains(in_.mx, in_.my)) {
        hotId_ = 31;
        tooltip_ = "Click: jump here  -  drag: set the LOOP section (snaps to bars, Shift = free)";
        tipX_ = in_.mx; tipY_ = ruler.y1 + 34; tipSec_ = curSec_;
        if (in_.pressed) { activeId_ = 31; loopDragX0_ = in_.mx; loopDragging_ = false; }
    }
    if (activeId_ == 31) {
        if (in_.down && std::fabs(in_.mx - loopDragX0_) > 6.0f) loopDragging_ = true;
        if (loopDragging_ && len) {
            std::uint64_t a = snapBar(frameAt(std::min(loopDragX0_, in_.mx)), false);
            std::uint64_t b = snapBar(frameAt(std::max(loopDragX0_, in_.mx)), true);
            const std::uint64_t minLen = in_.shift || barFrames < 1.0 ? static_cast<std::uint64_t>(sr_ * 0.05) : static_cast<std::uint64_t>(barFrames);
            if (b < a + minLen) b = std::min<std::uint64_t>(len, a + minLen);
            if (b < a + minLen) a = b > minLen ? b - minLen : 0;   // dragged against the song's end
            setLoopRegion(a, b);
            loopOn_ = true;
        }
        if (in_.released) {
            if (!loopDragging_) eng_->seek(frameAt(in_.mx));
            else {
                const std::uint64_t ph = eng_->playhead();
                if (ph < loopA() || ph >= loopB()) eng_->seek(loopA());
                toast(fmt("LOOP: bars %.0f", std::floor(static_cast<double>(loopA()) / barFrames + 1e-9) + 1.0) +
                      fmt(" to %.0f", std::ceil(static_cast<double>(loopB()) / barFrames - 1e-9)) + "  (L or the loop button turns it off)");
            }
            loopDragging_ = false;
            activeId_ = 0;
        }
    }

    // Lanes: click / drag to move the playhead.
    const Rect seekArea{lx0, lanes.y0, lx1, lanes.y1};
    if (len && seekArea.contains(in_.mx, in_.my) && in_.pressed) activeId_ = 30;
    if (activeId_ == 30) {
        const double f = clampf((in_.mx - lx0) / (lx1 - lx0), 0, 1);
        if (in_.down) eng_->seek(static_cast<std::uint64_t>(f * static_cast<double>(len)));
        if (in_.released) activeId_ = 0;
    }

    // Rows (drawn entirely live: the painted rows were cut out of the art).
    gfx_.rect({58, LY::kRowsY0, lanes.x0, LY::kRowsY1 - 4}, 0x090E13FF);
    const float hh = 0.5f * LY::kRowH;
    bool anySolo = false;
    for (std::size_t i = 0; i < kNumStems; ++i) anySolo |= eng_->stem(i).solo.load();
    for (int i = 0; i < static_cast<int>(kNumStems); ++i) {
        Slot& s = slots_[static_cast<std::size_t>(i)];
        auto& st = eng_->stem(static_cast<std::size_t>(i));
        const float cy = LY::kRow0 + LY::kRowStep * static_cast<float>(i);
        const Rect nameR{104, cy - hh, 238, cy + hh};
        const Rect numR{62, cy - hh, 100, cy + hh};
        gfx_.roundRect(numR, 3, 0x111820FF);
        gfx_.textIn(numR, std::to_string(i + 1), Font::Normal, kDim);
        const bool isSel = i == selected_;
        gfx_.rect(nameR, isSel ? Color(0x0F2A36FF) : Color(0x101519FF));
        if (isSel) gfx_.rect({nameR.x0, nameR.y0, nameR.x0 + 3, nameR.y1}, kCyan);
        const Color base = stemColorIsOrange(i) ? kOrange : kCyan;
        const bool audible = !st.mute.load() && (!anySolo || st.solo.load());
        gfx_.circle(116, cy - 3, 9.0f, base.alpha(0.12f + 0.45f * s.dotLevel));
        gfx_.circle(116, cy - 3, 5.5f, s.loaded ? base : Color(0x33414BFF));
        gfx_.textIn({128, cy - hh + 2, 236, cy + 6}, s.loaded ? shownName(i) : std::string("empty"), Font::Normal, s.loaded ? kText : Color(0x46555FFF), Align::Left);
        // volume bar (fader −60…+6 dB)
        const float fdb = st.faderDb.target();
        const float vf = clampf((fdb + 60.0f) / 66.0f, 0, 1);
        const float vb = cy + hh - 10;
        gfx_.rect({128, vb, 234, vb + 3}, 0x1C2830FF);
        gfx_.rect({128, vb, 128 + 106 * vf, vb + 3}, s.loaded ? base.alpha(0.8f) : Color(0x2A3640FF));
        gfx_.rect({128 + 106 * (60.0f / 66.0f) - 0.75f, vb - 2, 128 + 106 * (60.0f / 66.0f) + 0.75f, vb + 5}, kDim);

        const int nid = 100 + i;
        const bool hot = nameR.contains(in_.mx, in_.my);
        if (hot) hotId_ = nid;
        if (hot && in_.dbl) {
            if (!s.loaded) loadFiles(openFilesDialog(hwnd_, L"Load Stem", {L"WAV audio (*.wav)", L"*.wav"}, false), i);
            else st.faderDb.set(0.0f);
            activeId_ = 0;
        } else if (hot && in_.pressed) { activeId_ = nid; dragStartX_ = in_.devX; dragStartVal_ = fdb; dragMoved_ = false; }
        if (hot && in_.rpressed) { selected_ = i; stemContextMenu(i); }
        if (activeId_ == nid) {
            if (in_.down && std::fabs(in_.devX - dragStartX_) > 3.0f) dragMoved_ = true;
            if (dragMoved_) st.faderDb.set(clampf(dragStartVal_ + (in_.devX - dragStartX_) * (in_.shift ? 0.02f : 0.15f), -60.0f, 6.0f));
            if (in_.released) { if (!dragMoved_) selected_ = i; activeId_ = 0; }
        }
        if (hot && in_.wheel != 0) st.faderDb.set(clampf(fdb + in_.wheel * (in_.shift ? 0.1f : 1.0f), -60.0f, 6.0f));
        if (hot || activeId_ == nid) {
            tooltip_ = s.loaded ? (fdb <= -59.9f ? std::string("-inf dB") : fmt("%+.1f dB", fdb)) + "  ·  drag = volume" : "double-click to load a stem here";
            tipX_ = 171; tipY_ = cy - hh - 3; tipSec_ = curSec_;
        }

        // M / S
        const Rect mR{LY::rowM.x0, cy - 15, LY::rowM.x1, cy + 15}, sR{LY::rowS.x0, cy - 15, LY::rowS.x1, cy + 15};
        if (button(200 + i, mR)) st.mute.store(!st.mute.load());
        if (button(220 + i, sR)) st.solo.store(!st.solo.load());
        for (const Rect* b : {&mR, &sR}) {
            gfx_.roundRect(*b, 4, 0x3A4652FF);
            gfx_.roundRect(b->inset(1.5f), 3, (hotId_ == 200 + i && b == &mR) || (hotId_ == 220 + i && b == &sR) ? Color(0x26313BFF) : Color(0x161D24FF));
        }
        gfx_.textIn(mR, "M", Font::Normal, kText);
        gfx_.textIn(sR, "S", Font::Normal, kText);
        if (st.mute.load()) { gfx_.roundRect(mR.inset(1), 3, Color(kYellow).alpha(0.9f)); gfx_.textIn(mR, "M", Font::Normal, 0x101010FF); }
        if (st.solo.load()) { gfx_.roundRect(sR.inset(1), 3, Color(kGreen).alpha(0.9f)); gfx_.textIn(sR, "S", Font::Normal, 0x101010FF); }

        // Lane + waveform
        const Rect lane{lx0, cy - hh, lx1, cy + hh};
        gfx_.rect(lane, i % 2 ? Color(0x0B1218FF) : Color(0x0D151CFF));
        if (isSel) gfx_.rectOutline(lane, Color(kCyan).alpha(0.25f), 1.5f);
        if (s.loaded && !s.peaks.empty() && len) {
            const float wfrac = static_cast<float>(static_cast<double>(s.audio.frames()) / static_cast<double>(len));
            const float x1 = lx0 + (lx1 - lx0) * wfrac;
            const int cols = static_cast<int>((x1 - lx0) / 1.5f);
            Color wc = audible ? base : Color(0x3A464FFF);
            for (int c = 0; c < cols; ++c) {
                const std::size_t b0 = s.peaks.size() * static_cast<std::size_t>(c) / static_cast<std::size_t>(cols);
                const std::size_t b1 = std::max(b0 + 1, s.peaks.size() * static_cast<std::size_t>(c + 1) / static_cast<std::size_t>(cols));
                float m = 0;
                for (std::size_t b = b0; b < b1 && b < s.peaks.size(); ++b) m = std::max(m, s.peaks[b]);
                const float h = (hh - 3.0f) * std::sqrt(clampf(m, 0, 1));
                const float x = lx0 + 1.5f * c;
                gfx_.rect({x, cy - h, x + 1.2f, cy + h}, wc.alpha(0.85f));
            }
        }
        if (analyze_ && s.loaded) {
            const auto& r = s.meter.loudness;
            const std::string t = "S " + lufsStr(r.shortTerm) + " LUFS   I " + lufsStr(r.integrated) + fmt("   TP %.1f", r.truePeakDb);
            const float tw = gfx_.textWidth(t, Font::Small);
            gfx_.roundRect({lx1 - tw - 16, cy - 11, lx1 - 4, cy + 11}, 4, 0x000000C0);
            gfx_.text(lx1 - tw - 10, cy - 9, t, Font::Small, kText);
        }
    }

    // Empty-state hint.
    if (!loadedCount()) {
        const Rect hint{lanes.x0 + 120, lanes.cy() - 50, lanes.x1 - 120, lanes.cy() + 50};
        const bool hHot = hint.contains(in_.mx, in_.my);
        if (hHot) hotId_ = 40;
        gfx_.roundRect(hint, 10, hHot ? Color(0x0E2430E0) : Color(0x000000B0));
        if (hHot) gfx_.rectOutline(hint, Color(kCyan).alpha(0.6f), 1.5f);
        if (hHot && in_.released && activeId_ == 0) loadFolder();
        gfx_.textIn({hint.x0, hint.y0 + 8, hint.x1, hint.cy()}, "DROP UP TO 13 WAV STEMS HERE", Font::Large, kCyan);
        gfx_.textIn({hint.x0, hint.cy(), hint.x1, hint.y1 - 8}, "or CLICK HERE and pick any stem - its whole folder loads at once", Font::Normal, kDim);
    }

    // Playhead.
    if (len) {
        const double lat = eng_->totalLatency();
        const double pos = std::max(0.0, static_cast<double>(eng_->playhead()) - (eng_->isPlaying() ? lat : 0.0));
        const float x = lx0 + static_cast<float>(std::min(1.0, pos / static_cast<double>(len))) * (lx1 - lx0);
        gfx_.line(x, ruler.y0 + 6, x, lanes.y1, 2.0f, kWhite);
        const float t[] = {x - 9, ruler.y0 + 2, x + 9, ruler.y0 + 2, x, ruler.y0 + 14};
        gfx_.line(t[0], t[1], t[2], t[3], 3, kWhite);
        gfx_.line(t[0], t[1], t[4], t[5], 3, kWhite);
        gfx_.line(t[2], t[3], t[4], t[5], 3, kWhite);
    }
}

// ------------------------------------------------------------------ STFT DESTROYER
void App::feedSpectrum() {
    // Spectrogram of the selected stem (post-processing, live while playing).
    if (specFor_ != selected_) {
        spec_.prepare(sr_);
        float junk[2048];
        auto& tap = eng_->stem(static_cast<std::size_t>(selected_)).tap;
        while (tap.pop_n(junk, 2048) > 0) {}
        specFor_ = selected_;
    }
    {
        float buf[2048];
        std::size_t n;
        auto& tap = eng_->stem(static_cast<std::size_t>(selected_)).tap;
        const bool live = eng_->isPlaying();   // freeze the picture when stopped
        while ((n = tap.pop_n(buf, 2048)) > 0) if (live) spec_.feed(buf, n);
    }
}

void App::drawDestroyer() {
    ArtifactProcessor* ds = sel().ds;
    if (!ds) return;
    const bool by = ds->bypassed();
    if (button(300, R(LY::dsTitle)) || button(301, R(LY::dsToggle))) ds->setBypassed(!by);
    if (by) {
        gfx_.rect(R(LY::dsTitle), 0x000000A0);
        gfx_.text(LY::dsTitle.x0 + 6, LY::dsTitle.y0 + 8, "BYPASSED", Font::Normal, kRed);
    }
    // Header selector: repair strength LIGHT / MEDIUM / HEAVY (shimmer amount).
    static const char* modes[] = {"LIGHT", "MEDIUM", "HEAVY"};
    static const float amounts[] = {0.35f, 0.6f, 1.0f};
    const float amt = ds->param(ArtifactProcessor::kShimmer).target();
    int mode = amt < 0.47f ? 0 : (amt < 0.8f ? 1 : 2);
    if (hwButton(302, LY::dsPrev, Icon::Left)) mode = (mode + 2) % 3, ds->param(ArtifactProcessor::kShimmer).set(amounts[mode]);
    if (hwButton(303, LY::dsNext, Icon::Right) || button(304, R(LY::dsHeaderBox))) mode = (mode + 1) % 3, ds->param(ArtifactProcessor::kShimmer).set(amounts[mode]);
    gfx_.textIn(R(LY::dsHeaderBox), modes[mode], Font::Normal, kText);

    const Rect sc = R(LY::dsScreen).inset(3);
    const Rect area{sc.x0, sc.y0, sc.x1 - 16, sc.y1};
    const float cw = area.w() / Spectrogram::kCols, ch = area.h() / Spectrogram::kRows;
    for (int c = 0; c < Spectrogram::kCols; ++c) {
        const int col = (spec_.head + c) % Spectrogram::kCols;
        for (int r = 0; r < Spectrogram::kRows; ++r) {
            const float v = (spec_.cell[col][r] + 110.0f) / 95.0f;
            if (v <= 0.02f) continue;
            const float x = area.x0 + c * cw, y = area.y1 - (r + 1) * ch;
            gfx_.rect({x, y, x + cw + 0.4f, y + ch + 0.4f}, heat(v));
        }
    }
    for (int i = 0; i < 40; ++i) {   // colour bar
        const float y0 = sc.y1 - (i + 1) * sc.h() / 40.0f;
        gfx_.rect({sc.x1 - 10, y0, sc.x1 - 3, y0 + sc.h() / 40.0f + 0.5f}, heat(static_cast<float>(i) / 39.0f));
    }
    for (double f : {100.0, 1000.0, 10000.0}) {
        const float y = area.y1 - static_cast<float>(std::log(f / 40.0) / std::log(20000.0 / 40.0)) * area.h();
        gfx_.line(area.x0, y, area.x0 + 10, y, 1.0f, kDim);
        gfx_.text(area.x0 + 13, y - 9, f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f), Font::Small, Color(kDim).alpha(0.9f));
    }
    const std::string selName = sel().loaded ? shownName(selected_) : std::string();
    gfx_.text(area.x1 - gfx_.textWidth(selName, Font::Small) - 6, area.y0 + 4, selName, Font::Small, kText);
    const auto& t = ds->telemetry();
    gfx_.roundRect({area.x0 + 62, area.y1 - 24, area.x0 + 470, area.y1 - 2}, 4, 0x000000B0);
    gfx_.text(area.x0 + 70, area.y1 - 22, fmt("HF jitter %.1f dB", t.hfJitterDb.load()) + fmt("   HF change %+.1f dB", t.shimmerCutDb.load()) + fmt("   coherence %.2f", t.hfCoherence.load()),
              Font::Small, Color(kText).alpha(0.85f));

    // Resolution selector (rebuilds this stem's chain off the audio thread).
    static const char* res[] = {"1024", "2048", "4096"};
    int ri = static_cast<int>(std::lround(ds->param(ArtifactProcessor::kRes).target()));
    int nri = ri;
    if (hwButton(305, LY::dsResPrev, Icon::Left)) nri = (ri + 2) % 3;
    if (hwButton(306, LY::dsResNext, Icon::Right) || button(307, R(LY::dsResBox))) nri = (ri + 1) % 3;
    if (nri != ri) {
        ChainState cs = captureStem(selected_);
        for (auto& p : cs) if (p.id == "hc.artifact") p.values[ArtifactProcessor::kRes] = static_cast<float>(nri);
        rebuildState_[static_cast<std::size_t>(selected_)] = cs;
        rebuildPending_[static_cast<std::size_t>(selected_)] = true;
        ri = nri;
        toast(fmt("STFT resolution %.0f (latency changes; all stems stay aligned)", static_cast<double>(1024 << ri)));
    }
    gfx_.textIn(R(LY::dsResBox), res[std::clamp(ri, 0, 2)], Font::Normal, kText);

    // Toggle switch (bypass) — lever up = on.
    const Rect tg = R(LY::dsToggle);
    gfx_.roundRect(tg.inset(4), 4, 0x0C0F12FF);
    gfx_.roundRect({tg.x0 + 7, by ? tg.cy() : tg.y0 + 6, tg.x1 - 7, by ? tg.y1 - 6 : tg.cy()}, 3, by ? Color(0x5A6168FF) : kCyan);

    auto& thr = ds->param(ArtifactProcessor::kShimTol);
    auto& bl = ds->param(ArtifactProcessor::kPhase);
    auto& dc = ds->param(ArtifactProcessor::kDecay);
    thr.set(knob(310, LY::dsThreshold, thr.target(), 0, 12, Taper::Linear, 1.5f, false, kCyan, fmt("Threshold %.1f dB above envelope", thr.target()), by));
    bl.set(knob(311, LY::dsBleed, bl.target(), 0, 1, Taper::Linear, 0.3f, false, kCyan, fmt("Bleed (HF stereo phase fix) %.0f %%", bl.target() * 100), by));
    dc.set(knob(312, LY::dsDecay, dc.target(), 5, 200, Taper::Log, 10, false, kCyan, fmt("Decay %.0f ms", dc.target()), by));
}

// ------------------------------------------------------------------ RESONANCE
void App::drawResonance() {
    ResonanceProcessor* rs = sel().rs;
    if (!rs) return;
    const bool by = rs->bypassed();
    if (button(400, R(LY::rsTitle))) rs->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::rsTitle), 0x000000A0); gfx_.text(LY::rsTitle.x0 + 6, LY::rsTitle.y0 + 8, "BYPASSED", Font::Normal, kRed); }
    static const char* modes[] = {"SOFT", "NORMAL", "FAST"};
    auto& mp = rs->param(ResonanceProcessor::kMode);
    int m = static_cast<int>(std::lround(mp.target()));
    if (hwButton(401, LY::rsPrev, Icon::Left)) mp.set(static_cast<float>((m + 2) % 3));
    if (hwButton(402, LY::rsNext, Icon::Right) || button(403, R(LY::rsHeaderBox))) mp.set(static_cast<float>((m + 1) % 3));
    gfx_.textIn(R(LY::rsHeaderBox), modes[std::clamp(static_cast<int>(std::lround(mp.target())), 0, 2)], Font::Normal, kText);

    if (rs->view().update()) resView_ = rs->view().read_buffer();
    const Rect sc = R(LY::rsScreen).inset(6);
    auto xOf = [&](double f) { return sc.x0 + static_cast<float>(std::log(f / 20.0) / std::log(1000.0)) * sc.w(); };
    // focus band
    const float fc = rs->param(ResonanceProcessor::kFrequency).target(), bw = rs->param(ResonanceProcessor::kBandwidth).target();
    const float fx0 = std::max(sc.x0, xOf(fc / std::pow(2.0, bw * 0.5))), fx1 = std::min(sc.x1, xOf(fc * std::pow(2.0, bw * 0.5)));
    gfx_.rect({fx0, sc.y0, fx1, sc.y1}, Color(kCyan).alpha(by ? 0.03f : 0.07f));
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = xOf(f);
        gfx_.line(x, sc.y0, x, sc.y1, 1.0f, 0x15222BFF);
        gfx_.text(x + 3, sc.y1 - 18, f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f), Font::Small, Color(kDim).alpha(0.8f));
    }
    // spectrum (cyan fill)
    const std::size_t B = ResonanceView::kBands;
    std::vector<float> pts(2 * B), cut(2 * B);
    const float zeroY = sc.y0 + sc.h() * 0.22f;
    for (std::size_t b = 0; b < B; ++b) {
        const float x = sc.x0 + sc.w() * (static_cast<float>(b) + 0.5f) / B;
        pts[2 * b] = x;
        pts[2 * b + 1] = sc.y1 - clampf((resView_.levelDb[b] + 100.0f) / 100.0f, 0, 1) * sc.h();
        cut[2 * b] = x;
        cut[2 * b + 1] = zeroY - clampf(resView_.cutDb[b], -18.0f, 6.0f) / 18.0f * (sc.y1 - zeroY - 4);
    }
    gfx_.fillUnder(pts.data(), B, sc.y1, Color(kCyan).alpha(0.30f), Color(kCyan).alpha(0.02f));
    gfx_.polyline(pts.data(), B, 1.6f, Color(kCyan).alpha(0.85f));
    gfx_.line(sc.x0, zeroY, sc.x1, zeroY, 1.0f, Color(kOrange).alpha(0.25f));
    gfx_.text(sc.x0 + 4, zeroY - 18, "0 dB", Font::Small, Color(kOrange).alpha(0.7f));
    gfx_.polyline(cut.data(), B, 2.6f, by ? Color(0x6A6F74FF) : kOrange);
    float maxCut = 0;
    for (std::size_t b = 0; b < B; ++b) maxCut = std::min(maxCut, resView_.cutDb[b]);
    gfx_.text(sc.x1 - 150, sc.y0 + 4, fmt("max cut %.1f dB", -maxCut), Font::Small, kOrange);

    auto& se = rs->param(ResonanceProcessor::kSensitivity);
    auto& fr = rs->param(ResonanceProcessor::kFrequency);
    auto& bwp = rs->param(ResonanceProcessor::kBandwidth);
    auto& rd = rs->param(ResonanceProcessor::kReduction);
    auto& cp = rs->param(ResonanceProcessor::kCompensation);
    se.set(knob(410, LY::rsSensitivity, se.target(), 0, 100, Taper::Linear, 50, false, kCyan, fmt("Sensitivity %.0f %%", se.target()), by));
    fr.set(knob(411, LY::rsFrequency, fr.target(), 200, 16000, Taper::Log, 3500, false, kCyan,
                fr.target() >= 1000 ? fmt("Focus %.2f kHz", fr.target() / 1000) : fmt("Focus %.0f Hz", fr.target()), by));
    bwp.set(knob(412, LY::rsBandwidth, bwp.target(), 0.5f, 8, Taper::Linear, 5, false, kCyan, fmt("Bandwidth %.1f octaves", bwp.target()), by));
    rd.set(knob(413, LY::rsReduction, rd.target(), 0, 18, Taper::Linear, 6, false, kOrange, fmt("Max reduction %.1f dB", rd.target()), by));
    cp.set(knob(414, LY::rsCompensation, cp.target(), 0, 100, Taper::Linear, 50, false, kCyan, fmt("Compensation %.0f %%", cp.target()), by));
}

// ------------------------------------------------------------------ TRANSIENT
void App::drawTransient() {
    TransientProcessor* tr = sel().tr;
    if (!tr) return;
    const bool by = tr->bypassed();
    if (button(500, R(LY::trTitle)) || button(501, R(LY::trLed))) tr->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::trTitle), 0x000000A0); gfx_.text(LY::trTitle.x0 + 6, LY::trTitle.y0 + 8, "BYPASSED", Font::Normal, kRed); }
    const Rect led = R(LY::trLed);
    gfx_.circle(led.x1 - 10, led.cy(), 8, 0x000000FF);
    gfx_.circle(led.x1 - 10, led.cy(), 6, by ? Color(0x3A2A1FFF) : (tr->active() ? kOrange : Color(0x8A4E1FFF)));
    if (!by && tr->active()) gfx_.circle(led.x1 - 10, led.cy(), 12, Color(kOrange).alpha(0.25f));

    // History from the audio thread.
    TransientViz v;
    while (tr->viz().pop(v)) { trHist_[trHead_] = v; trHead_ = (trHead_ + 1) % trHist_.size(); }
    auto drawScreen = [&](const Rect& sc, bool attack, Color col, const char* label) {
        const Rect a = sc.inset(6);
        const float mid = a.cy();
        const std::size_t N = trHist_.size();
        const float bw = a.w() / static_cast<float>(N);
        for (std::size_t i = 0; i < N; ++i) {
            const TransientViz& h = trHist_[(trHead_ + i) % N];
            const float pk = clampf((20.0f * std::log10(h.peak + 1e-6f) + 60.0f) / 60.0f, 0, 1);
            const float x = a.x0 + bw * static_cast<float>(i);
            const float amp = pk * (a.h() * 0.45f);
            gfx_.rect({x, mid - amp, x + bw * 0.8f, mid + amp}, col.alpha(0.22f));
            const float g = attack ? h.attackDb : h.sustainDb;
            if (std::fabs(g) > 0.05f) {
                const float gh = clampf(std::fabs(g) / 15.0f, 0, 1) * amp;
                gfx_.rect({x, mid - gh, x + bw * 0.8f, mid + gh}, (g > 0 ? col : Color(0xB0BEC8FF)).alpha(0.9f));
            }
        }
        gfx_.line(a.x0, mid, a.x1, mid, 1.0f, col.alpha(0.35f));
        gfx_.textIn({a.x0, a.y1 - 26, a.x1, a.y1 - 2}, label, Font::Normal, kText);
        const float cur = attack ? tr->param(TransientProcessor::kAttack).target() : tr->param(TransientProcessor::kSustain).target();
        gfx_.text(a.x1 - 70, a.y0 + 2, fmt("%+.1f dB", cur), Font::Small, col);
    };
    drawScreen(R(LY::trAttackScreen), true, kCyan, "ATTACK");
    drawScreen(R(LY::trSustainScreen), false, kOrange, "SUSTAIN");

    // PUNCH / SPEED switch.
    auto& sp = tr->param(TransientProcessor::kSpeed);
    const Rect sw = R(LY::trSwitch);
    if (button(502, sw)) sp.set(sp.target() > 0.5f ? 0.0f : 1.0f);
    const bool speed = sp.target() > 0.5f;
    gfx_.roundRect(sw.inset(2), 5, 0x0B0E11FF);
    gfx_.roundRect({sw.x0 + 6, speed ? sw.cy() + 1 : sw.y0 + 5, sw.x1 - 6, speed ? sw.y1 - 5 : sw.cy() - 1}, 3, kCyan);
    if (sw.contains(in_.mx, in_.my)) { tooltip_ = speed ? "SPEED: fast detector (tight, clicky hits)" : "PUNCH: slower detector (bigger, fuller hits)"; tipX_ = sw.cx(); tipY_ = sw.y0 - 30; tipSec_ = curSec_; }

    auto& at = tr->param(TransientProcessor::kAttack);
    auto& su = tr->param(TransientProcessor::kSustain);
    auto& ga = tr->param(TransientProcessor::kGain);
    auto& mx = tr->param(TransientProcessor::kMix);
    at.set(knob(510, LY::trAttack, at.target(), -15, 15, Taper::Linear, 0, true, kCyan, fmt("Attack %+.1f dB", at.target()), by));
    su.set(knob(511, LY::trSustain, su.target(), -15, 15, Taper::Linear, 0, true, kCyan, fmt("Sustain %+.1f dB", su.target()), by));
    ga.set(knob(512, LY::trGain, ga.target(), -12, 12, Taper::Linear, 0, true, kOrange, fmt("Output gain %+.1f dB", ga.target()), by));
    mx.set(knob(513, LY::trMix, mx.target(), 0, 100, Taper::Linear, 100, false, kCyan, fmt("Mix %.0f %%", mx.target()), by));
}

// ------------------------------------------------------------------ MASTERING
void App::drawMastering() {
    if (!lim_ || !width_) return;
    const bool by = lim_->bypassed();
    if (button(600, R(LY::msTitle))) lim_->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::msTitle), 0x000000A0); gfx_.text(LY::msTitle.x0 + 6, LY::msTitle.y0 + 8, "LIMITER OFF", Font::Normal, kRed); }

    // Target preset selector.
    auto setPreset = [&](int p) {
        preset_ = (p + kNumPresets) % kNumPresets;
        normalize_ = kPresets[preset_].normalize;
        if (normalize_) target_ = kPresets[preset_].lufs;
    };
    if (hwButton(601, LY::msPrev, Icon::Left)) setPreset(preset_ - 1);
    if (hwButton(602, LY::msNext, Icon::Right) || button(603, R(LY::msHeaderBox))) setPreset(preset_ + 1);
    gfx_.textIn(R(LY::msHeaderBox), kPresets[preset_].name, Font::Normal, kText);

    const auto& L = master_.loudness;
    // ---- meters ----
    const Rect mt = R(LY::msMeters);
    const float top = mt.y0 + 12, bot = mt.y1 - 10;
    auto yOf = [&](float db) { return bot - clampf((db + 48.0f) / 48.0f, 0, 1) * (bot - top); };
    const float xs[4] = {828, 851, 933, 956};   // LUFS M, LUFS S, TP L, TP R
    for (int t = 0; t <= 8; ++t) {
        const float db = -6.0f * t, y = yOf(db);
        const std::string s = t == 0 ? "0" : fmt("%.0f", db);
        // One scale between the LUFS and TRUE PEAK pairs, plus the outer edges.
        gfx_.text(812 - gfx_.textWidth(s, Font::Small), y - 9, s, Font::Small, kDim);
        gfx_.text(892 - 0.5f * gfx_.textWidth(s, Font::Small), y - 9, s, Font::Small, kDim);
        gfx_.text(972, y - 9, s, Font::Small, kDim);
        gfx_.line(862, y, 868, y, 1.0f, Color(kDim).alpha(0.6f));
        gfx_.line(916, y, 922, y, 1.0f, Color(kDim).alpha(0.6f));
    }
    const double nowT = now();
    float vals[4] = {L.momentary, L.shortTerm, master_.tpDb[0], master_.tpDb[1]};
    for (int c = 2; c < 4; ++c) {
        const float v = std::isfinite(vals[c]) ? vals[c] : -90.0f;
        if (v >= tpHold_[c - 2] || nowT - tpHoldT_[c - 2] > 1.5) { tpHold_[c - 2] = v; tpHoldT_[c - 2] = nowT; }
    }
    const int segs = 40;
    for (int c = 0; c < 4; ++c) {
        const float v = std::isfinite(vals[c]) ? vals[c] : -90.0f;
        for (int i = 0; i < segs; ++i) {
            const float db = -48.0f + 48.0f * (i + 0.5f) / segs;
            const float y1 = yOf(-48.0f + 48.0f * i / segs), y0 = yOf(-48.0f + 48.0f * (i + 1) / segs);
            Color col;
            if (c < 2) col = db > static_cast<float>(target_) + 1.0f ? kOrange : (db > static_cast<float>(target_) - 3.0f ? kYellow : kGreen);
            else col = db > -1.0f ? kRed : (db > -6.0f ? kYellow : kGreen);
            const bool lit = v >= db;
            gfx_.rect({xs[c] - 8, y0 + 1, xs[c] + 8, y1 - 1}, lit ? col : col.alpha(0.08f));
        }
        if (c >= 2 && tpHold_[c - 2] > -48.0f) gfx_.rect({xs[c] - 8, yOf(tpHold_[c - 2]) - 1.5f, xs[c] + 8, yOf(tpHold_[c - 2]) + 1.5f}, kWhite);
    }
    gfx_.line(xs[0] - 12, yOf(static_cast<float>(target_)), xs[1] + 12, yOf(static_cast<float>(target_)), 1.5f, Color(kCyan).alpha(0.8f));
    gfx_.textIn({xs[0] - 12, bot + 1, xs[0] + 12, bot + 14}, "M", Font::Small, kDim);
    gfx_.textIn({xs[1] - 12, bot + 1, xs[1] + 12, bot + 14}, "S", Font::Small, kDim);
    gfx_.textIn({xs[2] - 12, bot + 1, xs[2] + 12, bot + 14}, "L", Font::Small, kDim);
    gfx_.textIn({xs[3] - 12, bot + 1, xs[3] + 12, bot + 14}, "R", Font::Small, kDim);

    // ---- readouts ----
    const bool onTarget = std::isfinite(L.integrated) && std::fabs(L.integrated - target_) <= 1.0;
    gfx_.textIn(R(LY::msIntegrated), lufsStr(L.integrated), Font::Large, onTarget ? kGreen : kOrange);
    gfx_.textIn(R(LY::msShortTerm), lufsStr(L.shortTerm), Font::Large, kOrange);
    gfx_.textIn(R(LY::msPsr), std::isfinite(L.shortTerm) && L.shortTerm > -70 ? fmt("%.1f", L.psr) : "--", Font::Large, kCyan);
    gfx_.textIn(R(LY::msPlr), std::isfinite(L.integrated) && L.integrated > -70 ? fmt("%.1f", L.plr) : "--", Font::Large, kCyan);
    gfx_.textIn(R(LY::msPlrLabel), "PLR", Font::Normal, kText);
    gfx_.textIn(R(LY::msFooterLabel), fmt("LRA %.1f", L.lra), Font::Small, kDim);

    // ---- MODE (glue) and TARGET boxes ----
    static const char* glue[] = {"CLEAN", "PUNCH", "LOUD"};
    if (button(610, R(LY::msModeBox))) { glueMode_ = static_cast<Glue>((static_cast<int>(glueMode_) + 1) % 3); applyGlue(); }
    gfx_.textIn(R(LY::msModeBox), glue[static_cast<int>(glueMode_)], Font::Small, kCyan);
    if (R(LY::msModeBox).contains(in_.mx, in_.my)) { tooltip_ = "Bus glue before the limiter: CLEAN off · PUNCH 2:1 · LOUD 3:1 (click)"; tipX_ = LY::msModeBox.x0 + 40; tipY_ = LY::msModeBox.y0 - 30; tipSec_ = curSec_; }
    const Rect tbx = R(LY::msTargetBox);
    const bool tHot = tbx.contains(in_.mx, in_.my);
    if (tHot) {
        hotId_ = 611;
        tooltip_ = normalize_ ? "Export loudness target · drag / wheel to set" : "Export without normalizing (pick a preset above to enable)";
        tipX_ = tbx.cx(); tipY_ = tbx.y0 - 30; tipSec_ = curSec_;
        if (in_.pressed && normalize_) { activeId_ = 611; dragStartY_ = in_.devY; dragStartVal_ = static_cast<float>(target_); }
        if (in_.wheel != 0 && normalize_) target_ = std::clamp(target_ + in_.wheel * 0.5, -30.0, -6.0);
    }
    if (activeId_ == 611) {
        if (in_.down) target_ = std::clamp(std::round((dragStartVal_ + (dragStartY_ - in_.devY) * 0.05f) * 2.0) / 2.0, -30.0, -6.0);
        if (in_.released) activeId_ = 0;
    }
    gfx_.textIn(tbx, normalize_ ? fmt("%.1f", target_) : "OFF", Font::Small, kCyan);
    gfx_.textIn(R(LY::msTargetLabel), "TARGET", Font::Normal, kText);

    // ---- knobs ----
    auto& ce = lim_->param(LimiterProcessor::kCeiling);
    auto& dr = lim_->param(LimiterProcessor::kDrive);
    auto& re = lim_->param(LimiterProcessor::kRelease);
    auto& wd = width_->param(0);
    ce.set(knob(620, LY::msCeiling, ce.target(), -6, 0, Taper::Linear, -1, false, kCyan, fmt("Ceiling %.1f dBTP", ce.target()), by));
    const float thr = knob(621, LY::msThreshold, -dr.target(), -24, 0, Taper::Linear, 0, false, kCyan,
                           fmt("Threshold %.1f dB (limiter drive)", -dr.target()), by);
    dr.set(-thr);
    re.set(knob(622, LY::msRelease, re.target(), 5, 1000, Taper::Log, 80, false, kCyan, fmt("Release %.0f ms", re.target()), by));
    wd.set(knob(623, LY::msWidth, wd.target(), 0, 200, Taper::Linear, 100, true, kCyan, fmt("Width %.0f %%", wd.target()), false));
    const float gr = lim_->limiter().gainReductionDb();
    lim_->limiter().clearGrMeter();
    if (gr < -0.1f) {   // limiter gain reduction, shown on the threshold knob's top edge
        const std::string g = fmt("GR %.1f dB", gr);
        const float w = gfx_.textWidth(g, Font::Small) + 10;
        const Rect gb{LY::msThreshold.cx - w / 2, LY::msThreshold.cy - LY::msThreshold.r - 20, LY::msThreshold.cx + w / 2, LY::msThreshold.cy - LY::msThreshold.r - 2};
        gfx_.roundRect(gb, 4, 0x000000C8);
        gfx_.textIn(gb, g, Font::Small, kOrange);
    }
}

// ------------------------------------------------------------------ overlays
void App::drawOverlays() {
    // Tooltip anchor → window space (tooltips and toasts are drawn at one size on every screen).
    float tipDevX = 0, tipDevY = 0;
    if (!tooltip_.empty()) {
        setSection(tipSec_);
        tipDevX = secOx_ + tipX_ * secScale_;
        tipDevY = secOy_ + tipY_ * secScale_;
    }
    setSection(Sec::Overlay);
    const float W = 1438.0f, H = static_cast<float>(gfx_.height()) / secScale_;
    const Rect v{0, 0, W, H};
    if (exporting_) {
        const Rect b{v.cx() - 380, v.y1 - 190, v.cx() + 380, v.y1 - 70};
        gfx_.roundRect(b, 12, 0x000000E0);
        gfx_.rectOutline(b, Color(kCyan).alpha(0.5f), 2);
        const double p = exportProgress_.load();
        gfx_.text(b.x0 + 24, b.y0 + 14, fmt("EXPORTING MASTER  %.0f %%", p * 100) + (normalize_ ? fmt("   (normalizing to %.1f LUFS)", target_) : std::string()), Font::Normal, kText);
        const Rect bar{b.x0 + 24, b.y0 + 52, b.x1 - 24, b.y0 + 72};
        gfx_.rect(bar, 0x15202AFF);
        gfx_.rect({bar.x0, bar.y0, bar.x0 + bar.w() * static_cast<float>(p), bar.y1}, kCyan);
        gfx_.text(b.x0 + 24, b.y0 + 82, "click the red record button to cancel", Font::Small, kDim);
    }
    if (!toast_.empty() && now() < toastUntil_) {
        const float w = std::min(W - 20, gfx_.textWidth(toast_, Font::Normal) + 40);
        const Rect t{v.cx() - w / 2, v.y1 - 70, v.cx() + w / 2, v.y1 - 28};
        gfx_.roundRect(t, 8, 0x000000D8);
        gfx_.rectOutline(t, Color(kCyan).alpha(0.35f), 1.5f);
        gfx_.textIn(t, toast_, Font::Normal, kText);
    }
    if (!tooltip_.empty()) {
        const float tx = tipDevX / secScale_, ty = tipDevY / secScale_;
        const float w = gfx_.textWidth(tooltip_, Font::Small) + 20;
        const float x0 = clampf(tx - w / 2, 10, W - 10 - w);
        const float y1 = std::max(ty, 40.0f);
        const Rect t{x0, y1 - 26, x0 + w, y1};
        gfx_.roundRect(t, 5, 0x05080BF0);
        gfx_.rectOutline(t, Color(kCyan).alpha(0.5f), 1.2f);
        gfx_.textIn(t, tooltip_, Font::Small, kText);
    }
}

// ------------------------------------------------------------------ NOW PLAYING (top-centre monitor)
// Section coordinates 0..1480 x 0..480. Song, clock, bars, tempo, loop, the master's loudness and peaks,
// and an overview of the whole song (all stems) with the playhead — click / drag it to move the playhead.
void App::drawNowPlaying() {
    const Rect sc{0, 0, 1480, 480};
    gfx_.gradientV(sc, 0x0B1219FF, 0x05080BFF);
    gfx_.rectOutline(sc.inset(2), Color(kCyan).alpha(0.18f), 2.0f);
    const bool playing = eng_->isPlaying();
    const std::uint64_t len = songLength();
    const double lat = eng_->totalLatency();
    const double pos = std::max(0.0, static_cast<double>(eng_->playhead()) - (playing ? lat : 0.0)) / sr_;

    // Left: what's playing.
    gfx_.text(36, 26, playing ? "NOW PLAYING" : (loadedCount() ? "STOPPED" : "NO SONG LOADED"), Font::Normal, playing ? kGreen : kDim);
    const std::string title = loadedCount() ? songTitle() : std::string("Open the CONSOLE screen and load your stems");
    gfx_.textIn({36, 58, 640, 104}, title, Font::Large, kText, Align::Left);
    gfx_.text(36, 112, loadedCount() ? fmt("%.0f stems", static_cast<double>(loadedCount())) + fmt("   -   %.0f Hz", sr_) : std::string(" "), Font::Small, kDim);

    // Centre: clock, bar.beat, tempo, loop.
    gfx_.textIn({640, 20, 1040, 96}, timeStr(pos), Font::Huge, kCyan);
    const double beatSec = 60.0 / bpm_;
    const double beats = pos / beatSec;
    const int bar = static_cast<int>(beats / 4.0) + 1, beat = static_cast<int>(std::fmod(beats, 4.0)) + 1;
    gfx_.textIn({640, 96, 1040, 128}, fmt("BAR %.0f", static_cast<double>(bar)) + fmt(" . %.0f", static_cast<double>(beat)) + fmt("     %.1f BPM", bpm_), Font::Normal, kText);
    if (loopOn_) {
        const double barF = 4.0 * beatSec * sr_;
        const std::string l = barF > 1.0 ? fmt("LOOP  bars %.0f", std::floor(static_cast<double>(loopA()) / barF + 1e-9) + 1.0) + fmt(" - %.0f", std::ceil(static_cast<double>(loopB()) / barF - 1e-9)) : std::string("LOOP");
        const float w = gfx_.textWidth(l, Font::Small) + 24;
        const Rect lb{840 - w / 2, 134, 840 + w / 2, 160};
        gfx_.roundRect(lb, 6, Color(kCyan).alpha(0.22f));
        gfx_.textIn(lb, l, Font::Small, kCyan);
    }

    // Right: master loudness + true-peak bars.
    const auto& L = master_.loudness;
    const bool onTarget = normalize_ && std::isfinite(L.integrated) && std::fabs(L.integrated - target_) <= 1.0;
    gfx_.text(1090, 26, "INTEGRATED", Font::Small, kDim);
    gfx_.textIn({1090, 44, 1290, 92}, lufsStr(L.integrated) + " LUFS", Font::Large, onTarget ? kGreen : kOrange, Align::Left);
    gfx_.text(1090, 100, "SHORT TERM", Font::Small, kDim);
    gfx_.textIn({1090, 116, 1290, 156}, lufsStr(L.shortTerm), Font::Large, kOrange, Align::Left);
    for (int c = 0; c < 2; ++c) {
        const float v = std::isfinite(master_.tpDb[c]) ? master_.tpDb[c] : -90.0f;
        const float x0 = 1330.0f + static_cast<float>(c) * 52.0f;
        const Rect bar{x0, 30, x0 + 36, 156};
        gfx_.rect(bar, 0x0C141BFF);
        const float f = clampf((v + 48.0f) / 48.0f, 0, 1);
        const Color col = v > -1.0f ? kRed : (v > -6.0f ? kYellow : kGreen);
        gfx_.rect({bar.x0 + 3, bar.y1 - 3 - f * (bar.h() - 6), bar.x1 - 3, bar.y1 - 3}, col);
        gfx_.textIn({x0, 158, x0 + 36, 178}, c ? "R" : "L", Font::Small, kDim);
    }

    // Bottom: the whole song (every loaded stem), loop band and playhead.
    const Rect ov{36, 200, 1444, 440};
    gfx_.rect(ov, 0x081017FF);
    gfx_.rectOutline(ov, Color(kCyan).alpha(0.15f), 1.5f);
    if (len) {
        const int cols = 704;
        const float cw = ov.w() / static_cast<float>(cols), mid = ov.cy();
        for (int x = 0; x < cols; ++x) {
            float pk = 0;
            for (const Slot& sl : slots_) {
                if (!sl.loaded || sl.peaks.empty() || sl.audio.frames() == 0) continue;
                const double f = (static_cast<double>(x) + 0.5) / cols * static_cast<double>(len) / static_cast<double>(sl.audio.frames());
                if (f >= 1.0) continue;
                pk = std::max(pk, sl.peaks[static_cast<std::size_t>(f * static_cast<double>(sl.peaks.size()))]);
            }
            const float h = std::min(1.0f, pk) * (ov.h() * 0.5f - 6);
            const float fx = static_cast<float>(x) / cols;
            const bool past = fx * static_cast<float>(len) < static_cast<float>(pos * sr_);
            gfx_.rect({ov.x0 + x * cw, mid - h, ov.x0 + (x + 1) * cw - 0.4f, mid + h}, past ? Color(kCyan).alpha(0.85f) : Color(0x2E6A84FF));
        }
        if (loopOn_ || loopB_ > loopA_) {
            const float xa = ov.x0 + static_cast<float>(static_cast<double>(loopA()) / static_cast<double>(len)) * ov.w();
            const float xb = ov.x0 + static_cast<float>(static_cast<double>(loopB()) / static_cast<double>(len)) * ov.w();
            gfx_.rect({xa, ov.y0, xb, ov.y1}, (loopOn_ ? kCyan : kDim).alpha(loopOn_ ? 0.12f : 0.06f));
            gfx_.line(xa, ov.y0, xa, ov.y1, 2.0f, (loopOn_ ? kCyan : kDim).alpha(0.8f));
            gfx_.line(xb, ov.y0, xb, ov.y1, 2.0f, (loopOn_ ? kCyan : kDim).alpha(0.8f));
        }
        const float px = ov.x0 + clampf(static_cast<float>(pos * sr_ / static_cast<double>(len)), 0, 1) * ov.w();
        gfx_.line(px, ov.y0 - 6, px, ov.y1 + 6, 2.5f, 0xFFFFFFFF);
        // Click / drag to move the playhead (only when this screen is open).
        if (ov.contains(in_.mx, in_.my)) {
            hotId_ = 9200;
            tooltip_ = "Click or drag to move the playhead";
            tipX_ = in_.mx; tipY_ = ov.y0 - 8; tipSec_ = curSec_;
            if (in_.pressed) activeId_ = 9200;
        }
        if (activeId_ == 9200) {
            if (in_.down) eng_->seek(static_cast<std::uint64_t>(clampf((in_.mx - ov.x0) / ov.w(), 0, 1) * static_cast<float>(len)));
            if (in_.released) activeId_ = 0;
        }
    } else {
        gfx_.textIn(ov, "Drag your stems (or their folder) onto the window", Font::Large, kDim);
    }
    gfx_.text(36, 450, "HYBRID CORE 2 ELITE  -  Hybrid Elite Mafia", Font::Small, Color(kDim).alpha(0.7f));
}

// ------------------------------------------------------------------ plug-in tabs + the four live-drawn plug-ins
int App::drawTabs(int screen, float x1, float y0, const char* const* names, int n) {
    const float h = 27, gap = 4;
    float x = x1;
    for (int t = n - 1; t >= 0; --t) {
        const float w = gfx_.textWidth(names[t], Font::Small) + 26;
        const Rect r{x - w, y0, x, y0 + h};
        const int id = 9400 + screen * 4 + t;
        if (button(id, r)) screenTab_[screen] = t;
        const bool on = screenTab_[screen] == t;
        gfx_.roundRect(r, 6, on ? Color(kCyan).alpha(0.85f) : Color(0x05080BE0));
        gfx_.rectOutline(r, Color(kCyan).alpha(on ? 0.9f : (hotId_ == id ? 0.8f : 0.4f)), 1.5f);
        gfx_.textIn(r, names[t], Font::Small, on ? Color(0x041016FF) : kText);
        x = r.x0 - gap;
    }
    tabsX0_ = x + gap;
    return screenTab_[screen];
}

bool App::panelFrame(int id, float ox, float oy, const char* name, IProcessor* p) {
    const Rect r{ox + 6, oy + 34, ox + 713, oy + 449};
    gfx_.roundRect({r.x0, r.y0 + 3, r.x1, r.y1 + 4}, 14, 0x000000B0);   // shadow
    gfx_.roundRect(r, 14, 0x2E363EFF);                                   // bevelled edge
    gfx_.roundRect(r.inset(2), 12, 0x12171CFF);                         // plate
    gfx_.gradientV({r.x0 + 14, r.y0 + 3, r.x1 - 14, r.y0 + 70}, 0xFFFFFF10, 0xFFFFFF00);   // top light
    for (float sx : {r.x0 + 16, r.x1 - 16})
        for (float sy : {r.y0 + 16, r.y1 - 16}) {
            gfx_.circleGradient(sx, sy, 6.5f, 0xA8B0B8FF, 0x353B41FF);
            gfx_.line(sx - 4, sy + 1.5f, sx + 4, sy - 1.5f, 1.6f, 0x20262BFF);
        }
    const bool on = p && !p->bypassed();
    const Rect title{ox + 34, oy + 44, ox + 560, oy + 80};
    gfx_.text(title.x0, title.y0, "HEM", Font::Large, kText);
    gfx_.text(title.x0 + gfx_.textWidth("HEM ", Font::Large), title.y0, name, Font::Large, kCyan);
    const Rect pw{ox + 606, oy + 46, ox + 692, oy + 76};
    if (p && (button(id, pw) || button(id + 1, title))) p->setBypassed(on);
    gfx_.roundRect(pw, 6, on ? Color(kCyan).alpha(0.85f) : Color(0x05080BE0));
    gfx_.rectOutline(pw, Color(kCyan).alpha(hotId_ == id ? 0.9f : 0.5f), 1.5f);
    gfx_.textIn(pw, on ? "ON" : "OFF", Font::Small, on ? Color(0x041016FF) : kDim);
    if (hotId_ == id || hotId_ == id + 1) { tooltip_ = on ? "Turn this plug-in off (bypass)" : "Turn this plug-in on"; tipX_ = pw.cx(); tipY_ = pw.y0 - 4; tipSec_ = curSec_; }
    return on;
}

float App::panelKnob(int id, float cx, float cy, float r, float v, float mn, float mx, Taper t, float def, bool bipolar,
                     const char* label, const std::string& value, bool dim, Color accent) {
    gfx_.textIn({cx - 75, cy - r - 28, cx + 75, cy - r - 6}, label, Font::Small, dim ? kDim : kText);
    const float nv = knob(id, LY::Knob{cx, cy, r}, v, mn, mx, t, def, bipolar, accent, std::string(label) + "  " + value, dim);
    gfx_.textIn({cx - 75, cy + r + 2, cx + 75, cy + r + 26}, value, Font::Normal, dim ? kDim : accent);
    return nv;
}

// EQ: six bands on the selected stem. Drag a dot (left/right = frequency, up/down = boost/cut),
// mouse wheel on a dot = width (Q), double-click a dot = reset that band. FIND PING hunts the ringing tone.
void App::drawEqPanel(float ox, float oy) {
    Eq6* eq = sel().eq;
    if (!eq) return;
    const bool on = panelFrame(700, ox, oy, "EQ", eq);
    const bool dim = !on;
    const Rect d{ox + 24, oy + 86, ox + 695, oy + 262};
    gfx_.rect(d, 0x05080BFF);
    gfx_.rectOutline(d, Color(kCyan).alpha(0.2f), 1.2f);
    auto xOf = [&](double f) { return d.x0 + static_cast<float>(std::log(f / 20.0) / std::log(1000.0)) * d.w(); };
    auto fOf = [&](float x) { return 20.0 * std::pow(1000.0, static_cast<double>((x - d.x0) / d.w())); };
    const float half = d.h() * 0.5f - 10;
    auto yOf = [&](float db) { return d.cy() - clampf(db, -24, 24) / 24.0f * half; };
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        gfx_.line(xOf(f), d.y0, xOf(f), d.y1, 1.0f, 0x15222BFF);
        gfx_.text(xOf(f) + 3, d.y1 - 18, f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f), Font::Small, Color(kDim).alpha(0.8f));
    }
    for (float db : {-12.0f, 0.0f, 12.0f}) {
        gfx_.line(d.x0, yOf(db), d.x1, yOf(db), 1.0f, db == 0 ? Color(0x2A3C48FF) : Color(0x15222BFF));
        gfx_.text(d.x0 + 4, yOf(db) - 17, fmt("%+.0f", db), Font::Small, Color(kDim).alpha(0.7f));
    }
    // The stem's live spectrum underneath (what's coming out of this stem's plug-ins).
    if (spec_.disp.size() == Spectrogram::kN / 2 + 1 && spec_.frames > 0) {
        constexpr int kPts = 180;
        float pts[2 * kPts];
        for (int i = 0; i < kPts; ++i) {
            const float x = d.x0 + d.w() * static_cast<float>(i) / (kPts - 1);
            const double f0 = fOf(x - d.w() / (2 * kPts)), f1 = fOf(x + d.w() / (2 * kPts));
            const int k0 = std::clamp(static_cast<int>(f0 * Spectrogram::kN / sr_), 1, Spectrogram::kN / 2);
            const int k1 = std::clamp(static_cast<int>(f1 * Spectrogram::kN / sr_), k0, Spectrogram::kN / 2);
            float pw = 0;
            for (int k = k0; k <= k1; ++k) pw = std::max(pw, spec_.disp[static_cast<std::size_t>(k)]);
            const float db = 10.0f * std::log10(pw + 1e-20f) + spec_.cal;
            pts[2 * i] = x;
            pts[2 * i + 1] = d.y1 - clampf((db + 100.0f) / 100.0f, 0, 1) * (d.h() - 4);
        }
        gfx_.fillUnder(pts, kPts, d.y1, Color(kCyan).alpha(dim ? 0.08f : 0.22f), Color(kCyan).alpha(0.02f));
    }
    // The EQ curve (sum of the active bands).
    {
        constexpr int kPts = 240;
        float pts[2 * kPts];
        BiquadCoeffs c[Eq6::kBands];
        bool act[Eq6::kBands];
        for (std::size_t b = 0; b < Eq6::kBands; ++b) {
            const FilterType ft = Eq6::bandType(b);
            const float fr = eq->param(3 * b).target(), gn = eq->param(3 * b + 1).target(), q = eq->param(3 * b + 2).target();
            c[b] = designBiquad(ft, sr_, fr, q, gn);
            const bool pass = ft == FilterType::HighPass || ft == FilterType::LowPass;
            act[b] = pass ? (ft == FilterType::HighPass ? fr > 20.5f : fr < 19999.0f) : std::fabs(gn) > 0.01f;
        }
        const float outDb = eq->param(18).target();
        for (int i = 0; i < kPts; ++i) {
            const float x = d.x0 + d.w() * static_cast<float>(i) / (kPts - 1);
            const double w = 2.0 * 3.14159265358979 * fOf(x) / sr_;
            double db = outDb;
            for (std::size_t b = 0; b < Eq6::kBands; ++b) {
                if (!act[b]) continue;
                const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
                const std::complex<double> num = c[b].b0 + c[b].b1 * z1 + c[b].b2 * z2;
                const std::complex<double> den = 1.0 + c[b].a1 * z1 + c[b].a2 * z2;
                db += 20.0 * std::log10(std::abs(num / den) + 1e-12);
            }
            pts[2 * i] = x;
            pts[2 * i + 1] = yOf(static_cast<float>(db));
        }
        gfx_.polyline(pts, kPts, 2.5f, dim ? Color(kDim) : kCyan);
    }
    // Band dots.
    static const Color bandCol[Eq6::kBands] = {0x9AA6B0FF, 0x5BE37AFF, 0xE8C547FF, 0xFF9A3EFF, 0xC27BFFFF, 0x9AA6B0FF};
    for (std::size_t b = 0; b < Eq6::kBands; ++b) {
        const FilterType ft = Eq6::bandType(b);
        const bool pass = ft == FilterType::HighPass || ft == FilterType::LowPass;
        AtomicParam& pf = eq->param(3 * b);
        AtomicParam& pg = eq->param(3 * b + 1);
        AtomicParam& pq = eq->param(3 * b + 2);
        const float x = xOf(pf.target()), y = pass ? d.cy() : yOf(pg.target());
        const int id = 720 + static_cast<int>(b);
        const bool hot = std::hypot(in_.mx - x, in_.my - y) < 15.0f;
        if (hot) {
            hotId_ = id;
            tooltip_ = std::string(b == 0 ? "HPF" : b == 1 ? "LOW" : b == 2 ? "BELL 1" : b == 3 ? "BELL 2" : b == 4 ? "HIGH" : "LPF") +
                       fmt("  %.0f Hz", pf.target()) + (pass ? std::string() : fmt("  %+.1f dB", pg.target())) + fmt("  Q %.2f  (wheel = width)", pq.target());
            tipX_ = x; tipY_ = y - 16; tipSec_ = curSec_;
            if (in_.dbl) { pf.set(Eq6::defaultFreq(b)); pg.set(0); pq.set(0.707f); activeId_ = 0; }
            else if (in_.pressed) { activeId_ = id; eqBand_ = static_cast<int>(b); }
            if (in_.wheel != 0) pq.set(clampf(pq.target() * std::pow(1.15f, in_.wheel), 0.1f, 18.0f));
        }
        if (activeId_ == id) {
            if (in_.down) {
                pf.set(static_cast<float>(std::clamp(fOf(in_.mx), 20.0, 22000.0)));
                if (!pass) pg.set(clampf((d.cy() - in_.my) / half * 24.0f, -24, 24));
            }
            if (in_.released) activeId_ = 0;
        }
        const bool selB = eqBand_ == static_cast<int>(b);
        gfx_.circle(x, y, selB ? 11.0f : 9.0f, 0x000000FF);
        gfx_.circle(x, y, selB ? 9.0f : 7.0f, dim ? Color(kDim) : bandCol[b]);
        if (selB || hot) gfx_.ring(x, y, 14.0f, 1.5f, Color(0xFFFFFFFF).alpha(0.7f));
    }
    // Band buttons + FIND PING.
    static const char* bnames[Eq6::kBands] = {"HPF", "LOW", "BELL 1", "BELL 2", "HIGH", "LPF"};
    const float bw = 82, by0 = oy + 270, by1 = oy + 296;
    for (int b = 0; b < static_cast<int>(Eq6::kBands); ++b) {
        const Rect r{ox + 24 + b * (bw + 4), by0, ox + 24 + b * (bw + 4) + bw, by1};
        if (button(740 + b, r)) eqBand_ = b;
        const bool sb = eqBand_ == b;
        gfx_.roundRect(r, 5, sb ? bandCol[b].alpha(0.85f) : Color(0x05080BE0));
        gfx_.rectOutline(r, bandCol[b].alpha(hotId_ == 740 + b ? 0.9f : 0.45f), 1.2f);
        gfx_.textIn(r, bnames[b], Font::Small, sb ? Color(0x041016FF) : kText);
    }
    const Rect ping{ox + 24 + 6 * (bw + 4) + 6, by0, ox + 695, by1};
    if (button(750, ping)) findPing();
    gfx_.roundRect(ping, 5, Color(kOrange).alpha(hotId_ == 750 ? 0.9f : 0.7f));
    gfx_.textIn(ping, "FIND PING", Font::Small, 0x140800FF);
    if (hotId_ == 750) { tooltip_ = "Play the part where it rings, then click: finds the ringing tone and cuts it with BELL 2"; tipX_ = ping.cx(); tipY_ = ping.y0 - 4; tipSec_ = curSec_; }
    // Knobs for the selected band + output.
    const std::size_t b = static_cast<std::size_t>(std::clamp(eqBand_, 0, 5));
    const bool pass = b == 0 || b == 5;
    AtomicParam& pf = eq->param(3 * b);
    AtomicParam& pg = eq->param(3 * b + 1);
    AtomicParam& pq = eq->param(3 * b + 2);
    AtomicParam& po = eq->param(18);
    const float ky = oy + 368, kr = 40;
    pf.set(panelKnob(760, ox + 110, ky, kr, pf.target(), 20, 22000, Taper::Log, Eq6::defaultFreq(b), false, "FREQUENCY",
                     pf.target() >= 1000 ? fmt("%.2f kHz", pf.target() / 1000) : fmt("%.0f Hz", pf.target()), dim, bandCol[b]));
    if (!pass) pg.set(panelKnob(761, ox + 265, ky, kr, pg.target(), -24, 24, Taper::Linear, 0, true, "BOOST / CUT", fmt("%+.1f dB", pg.target()), dim, bandCol[b]));
    else gfx_.textIn({ox + 190, ky - 20, ox + 340, ky + 20}, b == 0 ? "cuts the lows" : "cuts the highs", Font::Small, kDim);
    pq.set(panelKnob(762, ox + 420, ky, kr, pq.target(), 0.1f, 18, Taper::Log, 0.707f, false, pass ? "SLOPE (Q)" : "WIDTH (Q)",
                     fmt("%.2f", pq.target()), dim, bandCol[b]));
    po.set(panelKnob(763, ox + 600, ky, kr, po.target(), -24, 24, Taper::Linear, 0, true, "OUTPUT", fmt("%+.1f dB", po.target()), dim));
}

void App::findPing() {
    Eq6* eq = sel().eq;
    if (!eq) return;
    if (!eng_->isPlaying() || spec_.frames < 40 || spec_.avg.size() != Spectrogram::kN / 2 + 1) {
        toast("Play the part with the ping first, then click FIND PING while it rings");
        return;
    }
    float f = 0.0f;
    const float bestScore = pingSearch(spec_.avg, Spectrogram::kN, sr_, f);
    if (bestScore < 6.0f) { toast("No clear ping found - click FIND PING while the ringing is sounding"); return; }
    eq->param(3 * 3).set(f);
    eq->param(3 * 3 + 1).set(-9.0f);
    eq->param(3 * 3 + 2).set(8.0f);
    eq->setBypassed(false);
    eqBand_ = 3;
    toast(f >= 1000 ? fmt("Found a ping at %.2f kHz", f / 1000) + " - BELL 2 now cuts it 9 dB (drag the orange dot to fine-tune)"
                    : fmt("Found a ping at %.0f Hz", f) + " - BELL 2 now cuts it 9 dB (drag the orange dot to fine-tune)");
}

// COMPRESSOR: evens out loud and soft. Left: what it does to the level (in → out); the bar = how much it's pulling down now.
void App::drawCompPanel(float ox, float oy) {
    CompProcessor* c = sel().comp;
    if (!c) return;
    const bool on = panelFrame(800, ox, oy, "COMPRESSOR", c);
    const bool dim = !on;
    AtomicParam& th = c->param(CompProcessor::kThresh);
    AtomicParam& ra = c->param(CompProcessor::kRatio);
    AtomicParam& kn = c->param(CompProcessor::kKnee);
    AtomicParam& at = c->param(CompProcessor::kAttack);
    AtomicParam& re = c->param(CompProcessor::kRelease);
    AtomicParam& mk = c->param(CompProcessor::kMakeup);
    const Rect g{ox + 24, oy + 92, ox + 254, oy + 322};
    gfx_.rect(g, 0x05080BFF);
    gfx_.rectOutline(g, Color(kCyan).alpha(0.2f), 1.2f);
    auto gx = [&](float db) { return g.x0 + (db + 60.0f) / 60.0f * g.w(); };
    auto gy = [&](float db) { return g.y1 - clampf((db + 60.0f) / 60.0f, 0, 1) * g.h(); };
    gfx_.line(gx(-60), gy(-60), gx(0), gy(0), 1.0f, 0x2A3C48FF);   // 1:1
    float pts[2 * 61];
    const float T = th.target(), R = ra.target(), W = kn.target(), slope = 1.0f / R - 1.0f;
    for (int i = 0; i <= 60; ++i) {
        const float x = -60.0f + static_cast<float>(i);
        const float dlt = x - T;
        float gr = 0.0f;
        if (2.0f * dlt >= -W) gr = (W > 0.0f && 2.0f * std::fabs(dlt) <= W) ? slope * (dlt + 0.5f * W) * (dlt + 0.5f * W) / (2.0f * W) : slope * dlt;
        pts[2 * i] = gx(x);
        pts[2 * i + 1] = gy(x + gr + mk.target());
    }
    gfx_.polyline(pts, 61, 2.5f, dim ? Color(kDim) : kCyan);
    gfx_.line(gx(T), g.y0, gx(T), g.y1, 1.0f, Color(kOrange).alpha(0.6f));
    gfx_.text(g.x0 + 6, g.y0 + 4, "IN -> OUT", Font::Small, kDim);
    // Gain-reduction bar.
    const float gr = on ? -c->meterValue() : 0.0f;
    const Rect mb{ox + 268, oy + 92, ox + 298, oy + 322};
    gfx_.rect(mb, 0x05080BFF);
    gfx_.rect({mb.x0 + 3, mb.y0 + 3, mb.x1 - 3, mb.y0 + 3 + clampf(gr / 24.0f, 0, 1) * (mb.h() - 6)}, kOrange);
    gfx_.textIn({ox + 240, oy + 326, ox + 330, oy + 350}, fmt("-%.1f dB", gr), Font::Small, kOrange);
    // Knobs.
    const float kr = 38, c1 = ox + 410, c2 = ox + 530, c3 = ox + 650, r1 = oy + 170, r2 = oy + 330;
    th.set(panelKnob(810, c1, r1, kr, th.target(), -60, 0, Taper::Linear, -18, false, "THRESHOLD", fmt("%.1f dB", th.target()), dim));
    ra.set(panelKnob(811, c2, r1, kr, ra.target(), 1, 20, Taper::Log, 3, false, "RATIO", fmt("%.1f : 1", ra.target()), dim));
    kn.set(panelKnob(812, c3, r1, kr, kn.target(), 0, 24, Taper::Linear, 6, false, "KNEE", fmt("%.1f dB", kn.target()), dim));
    at.set(panelKnob(813, c1, r2, kr, at.target(), 0.05f, 200, Taper::Log, 10, false, "ATTACK", fmt("%.1f ms", at.target()), dim));
    re.set(panelKnob(814, c2, r2, kr, re.target(), 5, 2000, Taper::Log, 120, false, "RELEASE", fmt("%.0f ms", re.target()), dim));
    mk.set(panelKnob(815, c3, r2, kr, mk.target(), -12, 24, Taper::Linear, 0, true, "MAKEUP", fmt("%+.1f dB", mk.target()), dim));
}

// GATE: shuts the stem between hits. The bar is the stem's level; above the cyan line the gate opens.
void App::drawGatePanel(float ox, float oy) {
    GateProcessor* g = sel().gate;
    if (!g) return;
    const bool on = panelFrame(850, ox, oy, "GATE", g);
    const bool dim = !on;
    AtomicParam& th = g->param(GateProcessor::kThresh);
    AtomicParam& rg = g->param(GateProcessor::kRange);
    AtomicParam& at = g->param(GateProcessor::kAttack);
    AtomicParam& ho = g->param(GateProcessor::kHold);
    AtomicParam& re = g->param(GateProcessor::kRelease);
    AtomicParam& kh = g->param(GateProcessor::kKeyHpf);
    const Rect m{ox + 24, oy + 96, ox + 560, oy + 134};
    gfx_.rect(m, 0x05080BFF);
    gfx_.rectOutline(m, Color(kCyan).alpha(0.2f), 1.2f);
    auto mx = [&](float db) { return m.x0 + clampf((db + 80.0f) / 80.0f, 0, 1) * m.w(); };
    const float lv = g->keyLevelDb();   // shown even while OFF, so THRESHOLD can be set first
    gfx_.rect({m.x0 + 2, m.y0 + 4, mx(lv), m.y1 - 4}, lv > th.target() ? kGreen : Color(0x2E6A84FF));
    gfx_.line(mx(th.target()), m.y0 - 6, mx(th.target()), m.y1 + 6, 3.0f, kCyan);
    gfx_.line(mx(th.target() - GateProcessor::kHysteresisDb), m.y0, mx(th.target() - GateProcessor::kHysteresisDb), m.y1, 1.0f, Color(kCyan).alpha(0.4f));
    for (float db : {-60.0f, -40.0f, -20.0f, 0.0f}) gfx_.text(mx(db) - 10, m.y1 + 4, fmt("%.0f", db), Font::Small, kDim);
    const bool open = on && g->isOpen();
    const Rect led{ox + 580, oy + 94, ox + 695, oy + 136};
    gfx_.roundRect(led, 8, open ? Color(kGreen).alpha(0.85f) : Color(0x3A1414FF));
    gfx_.textIn(led, !on ? "OFF" : (open ? "OPEN" : "SHUT"), Font::Normal, open ? Color(0x062010FF) : Color(0xFF6A6AFF));
    const float kr = 36, ky = oy + 262;
    const float xs[6] = {ox + 75, ox + 188, ox + 301, ox + 414, ox + 527, ox + 640};
    th.set(panelKnob(860, xs[0], ky, kr, th.target(), -80, 0, Taper::Linear, -40, false, "THRESHOLD", fmt("%.1f dB", th.target()), dim));
    rg.set(panelKnob(861, xs[1], ky, kr, rg.target(), -80, 0, Taper::Linear, -60, false, "RANGE", fmt("%.0f dB", rg.target()), dim));
    at.set(panelKnob(862, xs[2], ky, kr, at.target(), 0.05f, 50, Taper::Log, 0.5f, false, "ATTACK", fmt("%.2f ms", at.target()), dim));
    ho.set(panelKnob(863, xs[3], ky, kr, ho.target(), 0, 500, Taper::Linear, 40, false, "HOLD", fmt("%.0f ms", ho.target()), dim));
    re.set(panelKnob(864, xs[4], ky, kr, re.target(), 5, 2000, Taper::Log, 120, false, "RELEASE", fmt("%.0f ms", re.target()), dim));
    kh.set(panelKnob(865, xs[5], ky, kr, kh.target(), 20, 2000, Taper::Log, 20, false, "KEY HPF", fmt("%.0f Hz", kh.target()), dim));
    gfx_.textIn({ox + 24, oy + 345, ox + 695, oy + 370}, "Set THRESHOLD just under the hits. RANGE = how far it shuts.", Font::Small, kDim);
    gfx_.textIn({ox + 24, oy + 372, ox + 695, oy + 397}, "KEY HPF up = the gate ignores kick / bass bleed when it decides.", Font::Small, kDim);
}

// DE-ESSER / HAT TAMER: turns down harsh highs only when they spike.
void App::drawDeEssPanel(float ox, float oy) {
    DeEsserProcessor* d = sel().dess;
    if (!d) return;
    const auto si = static_cast<std::size_t>(selected_);
    const bool on = panelFrame(900, ox, oy, "DE-ESSER", d);
    const bool dim = !on;
    AtomicParam& fr = d->param(DeEsserProcessor::kFreq);
    AtomicParam& th = d->param(DeEsserProcessor::kThresh);
    AtomicParam& mc = d->param(DeEsserProcessor::kMaxCut);
    AtomicParam& li = d->param(DeEsserProcessor::kListen);

    // ---- level meter: how loud the band above FREQUENCY is (works while OFF), with the THRESHOLD line.
    // Drag on it (or the THRESHOLD knob) to set the threshold; double-click = -24 dB.
    const Rect m{ox + 24, oy + 90, ox + 695, oy + 120};
    const auto xOf = [&](float db) { return m.x0 + 2 + clampf((db + 60.0f) / 60.0f, 0, 1) * (m.w() - 4); };
    const auto dbAt = [&](float x) { return clampf((x - m.x0 - 2) / (m.w() - 4) * 60.0f - 60.0f, -60.0f, 0.0f); };
    {
        const bool hot = m.contains(in_.mx, in_.my);
        if (hot) hotId_ = 916;
        if (hot && in_.dbl) { th.set(-24.0f); activeId_ = 0; }
        else if (hot && in_.pressed) activeId_ = 916;
        if (activeId_ == 916) {
            if (in_.down) th.set(std::round(dbAt(in_.mx) * 2.0f) * 0.5f);
            if (in_.released) activeId_ = 0;
        }
        if (hot || activeId_ == 916) { tooltip_ = "Drag to set THRESHOLD (double-click = -24 dB)"; tipX_ = clampf(in_.mx, m.x0 + 160, m.x1 - 160); tipY_ = m.y0 - 6; tipSec_ = curSec_; }
    }
    const float thr = th.target(), lvl = dessLvl_[si], hold = dessHold_[si];
    gfx_.rect(m, 0x05080BFF);
    gfx_.rectOutline(m, Color(kOrange).alpha(0.3f), 1.2f);
    for (float db : {-48.0f, -36.0f, -24.0f, -12.0f}) gfx_.line(xOf(db), m.y0 + 2, xOf(db), m.y1 - 2, 1.0f, Color(0xFFFFFF0C));
    if (lvl > -60.0f) {
        const float xl = xOf(lvl), xt = xOf(thr);
        gfx_.gradientV({m.x0 + 2, m.y0 + 4, std::min(xl, xt), m.y1 - 4}, Color(0xD9933AFF).alpha(dim ? 0.55f : 0.9f), Color(0x8A5420FF).alpha(dim ? 0.55f : 0.9f));
        if (xl > xt) gfx_.gradientV({xt, m.y0 + 4, xl, m.y1 - 4}, 0xFF5A2AFF, 0xC0281AFF);   // over the threshold: this part gets cut
    }
    if (hold > -60.0f) gfx_.rect({xOf(hold) - 1.5f, m.y0 + 3, xOf(hold) + 1.5f, m.y1 - 3}, Color(0xFFE2B0FF).alpha(0.85f));
    {
        const float xt = xOf(thr);
        gfx_.rect({xt - 1.2f, m.y0 - 5, xt + 1.2f, m.y1 + 5}, 0xF4F7FAFF);
        gfx_.triangle(xt - 6, m.y0 - 11, xt + 6, m.y0 - 11, xt, m.y0 - 4, 0xF4F7FAFF);
    }
    for (float db : {-60.0f, -48.0f, -36.0f, -24.0f, -12.0f, 0.0f}) {
        const std::string t = db == 0.0f ? std::string("0 dB") : fmt("%.0f", db);
        const float w = gfx_.textWidth(t, Font::Small);
        const float x = clampf(xOf(db) - w * 0.5f, m.x0, m.x1 - w);
        gfx_.text(x, m.y1 + 2, t, Font::Small, Color(kDim).alpha(0.9f));
    }
    // ---- cut: held for a moment so short cuts are seen.
    const float cut = on ? dessCut_[si] : 0.0f;
    gfx_.text(ox + 24, oy + 145, "CUT", Font::Small, dim ? kDim : kText);
    const Rect cb{ox + 70, oy + 149, ox + 440, oy + 161};
    gfx_.rect(cb, 0x05080BFF);
    gfx_.rectOutline(cb, Color(kOrange).alpha(0.25f), 1.0f);
    if (cut > 0.05f) gfx_.rect({cb.x0 + 1, cb.y0 + 2, cb.x0 + 1 + clampf(cut / 24.0f, 0, 1) * (cb.w() - 2), cb.y1 - 2}, kOrange);
    std::string status;
    if (harshStem_ == selected_) status = fmt("FIND HARSH: listening %.1f s", std::max(0.0, harshUntil_ - now()));
    else status = (lvl > -60.0f ? fmt("sss %.0f dB", lvl) : std::string("sss --")) + (on ? fmt("   cutting %.1f dB", cut) : std::string("   (off)"));
    gfx_.text(ox + 452, oy + 143, status, Font::Small, harshStem_ == selected_ ? kYellow : kOrange);

    const float kr = 44, ky = oy + 262;
    fr.set(panelKnob(910, ox + 120, ky, kr, fr.target(), 2000, 16000, Taper::Log, 6500, false, "FREQUENCY", fmt("%.1f kHz", fr.target() / 1000), dim, kOrange));
    th.set(panelKnob(911, ox + 290, ky, kr, th.target(), -60, 0, Taper::Linear, -24, false, "THRESHOLD", fmt("%.1f dB", th.target()), dim, kOrange));
    mc.set(panelKnob(912, ox + 460, ky, kr, mc.target(), 0, 24, Taper::Linear, 12, false, "MAX CUT", fmt("%.1f dB", mc.target()), dim, kOrange));
    // FIND HARSH: finds the tambourine / hats / "sss" while it plays and sets FREQUENCY + THRESHOLD.
    const Rect fb{ox + 560, oy + 178, ox + 695, oy + 222};
    const bool learning = harshStem_ == selected_;
    if (button(917, fb)) findHarsh();
    gfx_.roundRect(fb, 8, learning ? Color(kYellow).alpha(0.85f) : Color(0x3A1E08F0));
    gfx_.rectOutline(fb, Color(kOrange).alpha(hotId_ == 917 ? 0.95f : 0.6f), 1.5f);
    gfx_.textIn(fb, "FIND HARSH", Font::Small, learning ? Color(0x140800FF) : kText);
    if (hotId_ == 917) { tooltip_ = "Play the tambourine / hats / 'sss' part, then click: sets FREQUENCY + THRESHOLD and turns it on"; tipX_ = fb.cx() - 120; tipY_ = fb.y0 - 4; tipSec_ = curSec_; }
    const bool listen = li.target() > 0.5f;
    const Rect lb{ox + 560, ky - 14, ox + 695, ky + 30};
    if (button(915, lb)) li.set(listen ? 0.0f : 1.0f);
    gfx_.roundRect(lb, 8, listen ? Color(kOrange).alpha(0.9f) : Color(0x05080BE0));
    gfx_.rectOutline(lb, Color(kOrange).alpha(hotId_ == 915 ? 0.9f : 0.5f), 1.5f);
    gfx_.textIn(lb, "LISTEN", Font::Small, listen ? Color(0x140800FF) : kText);
    if (hotId_ == 915) { tooltip_ = "Hear only what's being cut (turn it back off when you're done)"; tipX_ = lb.cx() - 80; tipY_ = lb.y0 - 4; tipSec_ = curSec_; }
    gfx_.textIn({ox + 24, oy + 345, ox + 695, oy + 370}, "FIND HARSH: play the part, click it - it sets FREQUENCY + THRESHOLD and turns on.", Font::Small, kDim);
    gfx_.textIn({ox + 24, oy + 372, ox + 695, oy + 397}, "Red on the meter = what gets cut. Hats / tambourine 7-10 kHz, vocal \"sss\" 5-8 kHz.", Font::Small, kDim);
}

void App::pollDeEss() {
    const double t = now();
    const float dt = lastPoll_ > 0 ? static_cast<float>(std::min(0.25, t - lastPoll_)) : 0.0f;
    if (lastPoll_ == 0) { dessLvl_.fill(-100.0f); dessHold_.fill(-100.0f); dessCut_.fill(0.0f); }
    lastPoll_ = t;
    for (std::size_t i = 0; i < kNumStems; ++i) {
        DeEsserProcessor* d = slots_[i].dess;
        if (!d || !slots_[i].loaded) { dessLvl_[i] = dessHold_[i] = -100.0f; dessCut_[i] = 0.0f; continue; }
        const float lv = d->takeLevelPeakDb(), cut = d->takeCutPeakDb();
        dessLvl_[i] = std::max(lv, dessLvl_[i] - 40.0f * dt);                    // falls 40 dB/s
        if (lv >= dessHold_[i]) { dessHold_[i] = lv; dessHoldT_[i] = t; }
        else if (t - dessHoldT_[i] > 1.0) dessHold_[i] -= 20.0f * dt;           // peak mark: held 1 s, then falls
        if (cut >= dessCut_[i]) { dessCut_[i] = cut; dessCutT_[i] = t; }
        else if (t - dessCutT_[i] > 0.8) dessCut_[i] = std::max(0.0f, dessCut_[i] - 24.0f * dt);
        if (static_cast<int>(i) == harshStem_ && t >= harshFrom_ && t < harshUntil_) harshMax_ = std::max(harshMax_, lv);
    }
}

void App::findHarsh() {
    DeEsserProcessor* d = sel().dess;
    if (!d || !sel().loaded) return;
    if (!eng_->isPlaying()) { toast("Play the part with the tambourine / hats / 'sss' first, then click FIND HARSH"); return; }
    const int n = spec_.hitsWithin(8.0);
    if (n < 3) { toast("No harsh hits in the last 8 s - play a part with tambourine, hats or 'sss', then click FIND HARSH"); return; }
    harshHz_ = spec_.hitHzWithin(8.0);
    harshHits_ = n;
    d->param(DeEsserProcessor::kFreq).set(clampf(harshHz_ / 1.41f, 4000.0f, 12000.0f));   // half an octave under the hits
    d->param(DeEsserProcessor::kListen).set(0.0f);
    harshStem_ = selected_;
    harshFrom_ = now() + 0.15;   // let the detector settle on the new frequency
    harshUntil_ = harshFrom_ + 3.0;
    harshMax_ = -200.0f;
    toast(fmt("%.0f harsh hits", n) + fmt(" around %.1f kHz - listening 3 s to set THRESHOLD (keep it playing)", harshHz_ / 1000.0));
}

void App::updateFindHarsh() {
    if (harshStem_ < 0 || now() < harshUntil_) return;
    const int s = harshStem_;
    harshStem_ = -1;
    DeEsserProcessor* d = slots_[static_cast<std::size_t>(s)].dess;
    if (!d || !slots_[static_cast<std::size_t>(s)].loaded) return;
    if (harshMax_ < -70.0f) { toast("Too quiet to set a threshold - play the harsh part and click FIND HARSH again"); return; }
    const float thr = clampf(std::round((harshMax_ - 10.0f) * 2.0f) * 0.5f, -60.0f, -6.0f);
    d->param(DeEsserProcessor::kThresh).set(thr);
    d->setBypassed(false);
    toast(fmt("De-esser ON: %.1f kHz", d->param(DeEsserProcessor::kFreq).target() / 1000.0) + fmt(", threshold %.1f dB", thr) +
          fmt(" (loudest hit %.1f dB). Too much? Turn MAX CUT down.", harshMax_));
}

// ------------------------------------------------------------------ 3D SCOPE
// The selected stem as a lit 3D landscape: newest sound in front, ~2.3 s going back, bass left, highs right,
// height + colour = level. The range follows the stem (top of the box = its loudest point), so a quiet
// tambourine fills the box as well as a loud vocal. Harsh hits get red pins; a ring gets a magenta curtain.
// Drag to turn, double-click to reset. Built from the 3D landscape in THE HYBRID CORE's ANALYZER.
namespace scope3d {
constexpr int   kRows = Spectrogram::kLRows, kPts = Spectrogram::kLPts;
constexpr float kXW = 1.6f, kZD = 2.4f, kH = 0.95f, kCamD = 5.6f, kRange = 72.0f;

struct Rgb { float r, g, b; };
inline Rgb rgbOf(std::uint32_t v) { return {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f}; }
inline Color colorOf(Rgb c, float a) {
    auto b = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return Color((b(c.r) << 24) | (b(c.g) << 16) | (b(c.b) << 8) | b(a));
}
inline Rgb mixRgb(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
// Deep violet -> magenta -> orange -> gold -> white.
inline Rgb heatRgb(float t) {
    static const std::uint32_t stops[] = {0x0D0520, 0x3B0F70, 0x8C2981, 0xDE4968, 0xFE9F6D, 0xFDE68A, 0xFFFFFF};
    t = std::clamp(t, 0.0f, 1.0f) * 6.0f;
    const int i = std::min(static_cast<int>(t), 5);
    return mixRgb(rgbOf(stops[i]), rgbOf(stops[i + 1]), t - static_cast<float>(i));
}
struct Projector {
    float cy, sy, cp, sp, scale = 1, ox = 0, oy = 0;
    Projector(float yaw, float pitch, const Rect& view) {
        cy = std::cos(yaw); sy = std::sin(yaw); cp = std::cos(pitch); sp = std::sin(pitch);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (float x : {-kXW - 0.2f, kXW + 0.2f})
            for (float y : {0.0f, kH * 1.15f})
                for (float z : {-0.55f, kZD}) {
                    float u, v, d;
                    raw(x, y, z, u, v, d);
                    x0 = std::min(x0, u); x1 = std::max(x1, u); y0 = std::min(y0, v); y1 = std::max(y1, v);
                }
        scale = std::min(view.w() / (x1 - x0), view.h() / (y1 - y0)) * 0.97f;
        ox = view.cx() - scale * 0.5f * (x0 + x1);
        oy = view.cy() - scale * 0.5f * (y0 + y1);
    }
    void raw(float x, float y, float z, float& u, float& v, float& depth) const {
        const float ry = y - kH * 0.3f, rz0 = z - kZD * 0.5f;
        const float rx = x * cy + rz0 * sy, rz = -x * sy + rz0 * cy;
        const float up = ry * cp + rz * sp;
        depth = rz * cp - ry * sp + kCamD;
        u = rx / depth * kCamD;
        v = -up / depth * kCamD;
    }
    void operator()(float x, float y, float z, float& sx, float& sy_) const {
        float u, v, d;
        raw(x, y, z, u, v, d);
        sx = ox + u * scale; sy_ = oy + v * scale;
    }
    float depth(float x, float y, float z) const { float u, v, d; raw(x, y, z, u, v, d); return d; }
};
inline float pointX(int i) { return -kXW + 2.0f * kXW * static_cast<float>(i) / (kPts - 1); }
inline float freqX(float hz) { return -kXW + 2.0f * kXW * std::log(hz / 30.0f) / std::log(20000.0f / 30.0f); }
inline float rowZ(int age) { return kZD * static_cast<float>(age) / (kRows - 1); }
} // namespace scope3d

void App::drawScope3D(float ox, float oy) {
    using namespace scope3d;
    const Spectrogram& sp = spec_;
    const Rect panel{ox + 6, oy + 34, ox + 713, oy + 449};
    gfx_.roundRect({panel.x0, panel.y0 + 3, panel.x1, panel.y1 + 4}, 14, 0x000000B0);
    gfx_.roundRect(panel, 14, 0x2E363EFF);
    gfx_.roundRect(panel.inset(2), 12, 0x070B12FF);
    gfx_.gradientV({panel.x0 + 3, panel.y0 + 3, panel.x1 - 3, panel.y0 + 200}, 0x101A2CFF, 0x070B1200);
    gfx_.text(ox + 30, oy + 44, "HEM", Font::Large, kText);
    gfx_.text(ox + 30 + gfx_.textWidth("HEM ", Font::Large), oy + 44, "3D SCOPE", Font::Large, kCyan);
    const std::string nm = sel().loaded ? shownName(selected_) : std::string("no stem");
    gfx_.text(ox + 690 - gfx_.textWidth(nm, Font::Small), oy + 50, nm, Font::Small, kText);

    const Rect view{ox + 14, oy + 84, ox + 705, oy + 398};
    // Turn: drag (only when this screen fills the window), double-click resets.
    if (focus_ == DK::kDestroyer) {
        const bool hot = view.contains(in_.mx, in_.my);
        if (hot) hotId_ = 4700;
        if (hot && in_.dbl) { scopeYaw_ = 0.32f; scopePitch_ = 0.52f; activeId_ = 0; }
        else if (hot && in_.pressed) { activeId_ = 4700; dragStartX_ = in_.devX; dragStartY_ = in_.devY; scopeYaw0_ = scopeYaw_; scopePitch0_ = scopePitch_; }
        if (activeId_ == 4700) {
            if (in_.down) {
                scopeYaw_ = clampf(scopeYaw0_ + (in_.devX - dragStartX_) * 0.004f, -0.9f, 0.9f);
                scopePitch_ = clampf(scopePitch0_ + (in_.devY - dragStartY_) * 0.003f, 0.12f, 1.15f);
            }
            if (in_.released) activeId_ = 0;
        }
    }
    const Projector P(scopeYaw_, scopePitch_, view);

    // Auto range: the top of the box follows the loudest point of the last 2.3 s (fast up, slow down).
    float loud = -140.0f;
    for (int a = 0; a < kRows; ++a) { const float* r = sp.landRow(a); for (int i = 0; i < kPts; ++i) loud = std::max(loud, r[i]); }
    const float want = clampf(loud + 3.0f, -60.0f, 3.0f);
    scopeTop_ = want > scopeTop_ ? scopeTop_ + (want - scopeTop_) * 0.35f : scopeTop_ + (want - scopeTop_) * 0.02f;
    const float lo = scopeTop_ - kRange;
    const auto levelT = [&](float db) { return clampf((db - lo) / kRange, 0.0f, 1.0f); };
    const auto heightOf = [&](float t) { return kH * std::pow(t, 1.35f); };

    // Heights and levels of the grid (row 0 = newest).
    static std::vector<float> T, Hh;
    static std::vector<Color> C;
    static std::vector<float> SX, SY;
    T.assign(static_cast<std::size_t>(kRows) * kPts, 0.0f);
    Hh.assign(T.size(), 0.0f);
    for (int a = 0; a < kRows; ++a) {
        const float* r = sp.landRow(a);
        for (int i = 0; i < kPts; ++i) {
            const float t = levelT(r[i]);
            T[static_cast<std::size_t>(a * kPts + i)] = t;
            Hh[static_cast<std::size_t>(a * kPts + i)] = heightOf(t);
        }
    }
    const auto h = [&](int a, int i) { a = std::clamp(a, 0, kRows - 1); i = std::clamp(i, 0, kPts - 1); return Hh[static_cast<std::size_t>(a * kPts + i)]; };
    // Lighting: key light from the upper left (the desk lamp side), Blinn-Phong sheen, valleys darkened,
    // haze toward the back. Harsh rows turn red in the highs.
    const float dx = 2.0f * kXW / (kPts - 1), dz = kZD / (kRows - 1);
    float Lx = -0.45f, Ly = 0.80f, Lz = -0.40f;
    { const float l = std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz); Lx /= l; Ly /= l; Lz /= l; }
    float Hx = Lx, Hy = Ly + 0.45f, Hz = Lz - 0.89f;
    { const float l = std::sqrt(Hx * Hx + Hy * Hy + Hz * Hz); Hx /= l; Hy /= l; Hz /= l; }
    const Rgb fog = rgbOf(0x070B14), red = rgbOf(0xFF3B1E);
    C.assign(T.size(), Color(0));
    SX.assign(T.size(), 0.0f); SY.assign(T.size(), 0.0f);
    const int i4k = std::clamp(static_cast<int>(std::lround(std::log(4000.0f / 30.0f) / std::log(20000.0f / 30.0f) * (kPts - 1))), 0, kPts - 1);
    for (int a = 0; a < kRows; ++a) {
        const bool harsh = sp.landHarsh[sp.landIdx(a)] != 0;
        for (int i = 0; i < kPts; ++i) {
            const std::size_t k = static_cast<std::size_t>(a * kPts + i);
            const float gx = (h(a, i + 1) - h(a, i - 1)) / (2.0f * dx), gz = (h(a + 1, i) - h(a - 1, i)) / (2.0f * dz);
            const float nl = std::sqrt(gx * gx + 1.0f + gz * gz);
            const float nx = -gx / nl, ny = 1.0f / nl, nz = -gz / nl;
            const float diff = std::max(0.0f, nx * Lx + ny * Ly + nz * Lz);
            const float spec = std::pow(std::max(0.0f, nx * Hx + ny * Hy + nz * Hz), 30.0f) * (0.25f + 0.75f * T[k]);
            const float around = 0.25f * (h(a, i - 1) + h(a, i + 1) + h(a - 1, i) + h(a + 1, i));
            const float ao = clampf(1.0f - (around - Hh[k]) * 2.2f, 0.55f, 1.0f);
            Rgb c = heatRgb(0.06f + 0.94f * T[k]);
            if (harsh && i >= i4k) c = mixRgb(c, red, 0.35f + 0.5f * T[k]);
            const float lit = (0.38f + 0.82f * diff) * ao;
            c = {c.r * lit + spec * 0.95f, c.g * lit + spec * 0.85f, c.b * lit + spec * 0.65f};
            c = mixRgb(c, fog, 0.72f * static_cast<float>(a) / (kRows - 1));
            C[k] = colorOf(c, 0.98f);
            P(pointX(i), Hh[k], rowZ(a), SX[k], SY[k]);
        }
    }

    // ---- floor, apron (front lip with the reflection), back wall, grid
    const auto line3 = [&](float x0, float y0, float z0, float x1, float y1, float z1, float w, Color c) {
        float a0, b0, a1, b1;
        P(x0, y0, z0, a0, b0); P(x1, y1, z1, a1, b1);
        gfx_.line(a0, b0, a1, b1, w, c);
    };
    {
        float q[8];
        const Color wall[4] = {0x0A1222F0, 0x0A1222F0, 0x05081280, 0x05081280};
        P(-kXW, 0, kZD, q[0], q[1]); P(kXW, 0, kZD, q[2], q[3]); P(kXW, kH, kZD, q[4], q[5]); P(-kXW, kH, kZD, q[6], q[7]);
        gfx_.quadC(q, wall);
        const Color fl[4] = {0x0F1B30F8, 0x0F1B30F8, 0x060A12F8, 0x060A12F8};
        P(-kXW, 0, 0, q[0], q[1]); P(kXW, 0, 0, q[2], q[3]); P(kXW, 0, kZD, q[4], q[5]); P(-kXW, 0, kZD, q[6], q[7]);
        gfx_.quadC(q, fl);
        const Color ap[4] = {0x0B1424FF, 0x0B1424FF, 0x14223AFF, 0x14223AFF};
        P(-kXW, 0, -0.5f, q[0], q[1]); P(kXW, 0, -0.5f, q[2], q[3]); P(kXW, 0, 0, q[4], q[5]); P(-kXW, 0, 0, q[6], q[7]);
        gfx_.quadC(q, ap);
        // Reflection of the newest slice on the glossy apron.
        for (int i = 0; i < kPts - 1; ++i) {
            const float h0 = Hh[static_cast<std::size_t>(i)] * 0.22f, h1 = Hh[static_cast<std::size_t>(i + 1)] * 0.22f;
            if (h0 + h1 <= 0.002f) continue;
            P(pointX(i), 0, -0.002f, q[0], q[1]); P(pointX(i + 1), 0, -0.002f, q[2], q[3]);
            P(pointX(i + 1), 0, -h1, q[4], q[5]); P(pointX(i), 0, -h0, q[6], q[7]);
            const Rgb c0 = heatRgb(0.1f + 0.9f * T[static_cast<std::size_t>(i)]), c1 = heatRgb(0.1f + 0.9f * T[static_cast<std::size_t>(i + 1)]);
            const Color rc[4] = {colorOf(c0, 0.22f), colorOf(c1, 0.22f), colorOf(c1, 0.0f), colorOf(c0, 0.0f)};
            gfx_.quadC(q, rc);
        }
        for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
            const float x = freqX(hz);
            line3(x, 0, 0, x, 0, kZD, 1.0f, Color(0x3FD0FF2A));
            line3(x, 0, kZD, x, kH, kZD, 1.0f, Color(0x3FD0FF1A));
        }
        for (int k = 0; k <= 4; ++k) line3(-kXW, 0, kZD * k / 4.0f, kXW, 0, kZD * k / 4.0f, 1.0f, Color(k == 0 ? 0x3FD0FF60 : 0x3FD0FF22));
        for (int k = 0; k <= 3; ++k) {   // level lines on the back wall, every 24 dB under the top
            const float db = scopeTop_ - 24.0f * k;
            const float y = heightOf(levelT(db));
            line3(-kXW, y, kZD, kXW, y, kZD, 1.0f, Color(k == 0 ? 0x3FD0FF55 : 0x3FD0FF22));
            float lx, ly;
            P(-kXW - 0.03f, y, kZD, lx, ly);
            const std::string t = fmt("%.0f dB", db);
            gfx_.text(lx - gfx_.textWidth(t, Font::Small) - 4, ly - 8, t, Font::Small, Color(kDim).alpha(0.85f));
        }
    }

    // ---- the landscape, back to front (painter's order), far side first.
    const bool leftFar = P.depth(-kXW, 0, kZD * 0.5f) > P.depth(kXW, 0, kZD * 0.5f);
    for (int a = kRows - 2; a >= 0; --a) {
        for (int k = 0; k < kPts - 1; ++k) {
            const int i = leftFar ? k : kPts - 2 - k;
            const std::size_t b0 = static_cast<std::size_t>((a + 1) * kPts + i), b1 = b0 + 1, f0 = static_cast<std::size_t>(a * kPts + i), f1 = f0 + 1;
            if (T[b0] + T[b1] + T[f0] + T[f1] <= 0.0f) continue;   // flat: the floor shows through
            const float q[8] = {SX[b0], SY[b0], SX[b1], SY[b1], SX[f1], SY[f1], SX[f0], SY[f0]};
            const Color cs[4] = {C[b0], C[b1], C[f1], C[f0]};
            gfx_.quadC(q, cs);
        }
        if ((sp.landRows + static_cast<std::uint64_t>(kRows) * 1000 - static_cast<std::uint64_t>(a)) % 6 == 0) {   // contour every 6th row
            float xy[2 * kPts];
            for (int i = 0; i < kPts; ++i) P(pointX(i), h(a, i) + 0.004f, rowZ(a), xy[2 * i], xy[2 * i + 1]);
            gfx_.polyline(xy, kPts, 1.0f, Color(0xFDE68AFF).alpha(0.16f * (1.0f - static_cast<float>(a) / kRows)));
        }
    }
    // Front face (newest slice down to the floor) and the lit ridge.
    {
        for (int i = 0; i < kPts - 1; ++i) {
            if (T[static_cast<std::size_t>(i)] + T[static_cast<std::size_t>(i + 1)] <= 0.0f) continue;
            float q[8];
            P(pointX(i), Hh[static_cast<std::size_t>(i)], 0, q[0], q[1]); P(pointX(i + 1), Hh[static_cast<std::size_t>(i + 1)], 0, q[2], q[3]);
            P(pointX(i + 1), 0, 0, q[4], q[5]); P(pointX(i), 0, 0, q[6], q[7]);
            const Rgb t0 = heatRgb(0.1f + 0.9f * T[static_cast<std::size_t>(i)]), t1 = heatRgb(0.1f + 0.9f * T[static_cast<std::size_t>(i + 1)]);
            const Color cs[4] = {colorOf({t0.r * 0.6f, t0.g * 0.6f, t0.b * 0.6f}, 0.97f), colorOf({t1.r * 0.6f, t1.g * 0.6f, t1.b * 0.6f}, 0.97f), 0x05070DF8, 0x05070DF8};
            gfx_.quadC(q, cs);
        }
        float xy[2 * kPts];
        for (int i = 0; i < kPts; ++i) P(pointX(i), Hh[static_cast<std::size_t>(i)], 0, xy[2 * i], xy[2 * i + 1]);
        gfx_.polyline(xy, kPts, 5.0f, Color(0xFFB86B40));
        gfx_.polyline(xy, kPts, 2.0f, Color(0xFFF1C8F0));
    }

    // ---- ring: a magenta curtain along time at the ringing pitch.
    const bool ringOn = sp.ringScore >= 8.0f && sp.ringHz > 0.0f;
    if (ringOn) {
        const float x = freqX(sp.ringHz);
        const int pi = std::clamp(static_cast<int>(std::lround((x + kXW) / (2.0f * kXW) * (kPts - 1))), 0, kPts - 1);
        const float a = clampf((sp.ringScore - 6.0f) / 14.0f, 0.3f, 1.0f);
        float q[8];
        P(x, 0, 0, q[0], q[1]); P(x, 0, kZD, q[2], q[3]); P(x, kH * 1.1f, kZD, q[4], q[5]); P(x, kH * 1.1f, 0, q[6], q[7]);
        const Color cc[4] = {Color(0xFF3BD4FF).alpha(0.22f * a), Color(0xFF3BD4FF).alpha(0.05f * a), Color(0xFF3BD4FF).alpha(0.0f), Color(0xFF3BD4FF).alpha(0.0f)};
        gfx_.quadC(q, cc);
        float xy[2 * kRows];
        for (int r = 0; r < kRows; ++r) P(x, h(r, pi) + 0.01f, rowZ(r), xy[2 * r], xy[2 * r + 1]);
        gfx_.polyline(xy, kRows, 4.0f, Color(0xFF3BD4FF).alpha(0.3f * a));
        gfx_.polyline(xy, kRows, 1.8f, Color(0xFF9BEAFF).alpha(a));
        float lx, ly;
        P(x, kH * 1.1f, 0, lx, ly);
        const std::string t = sp.ringHz >= 1000 ? fmt("RING %.2f kHz", sp.ringHz / 1000.0) : fmt("RING %.0f Hz", sp.ringHz);
        const float w = gfx_.textWidth(t, Font::Small);
        gfx_.roundRect({lx - w * 0.5f - 6, ly - 22, lx + w * 0.5f + 6, ly - 2}, 5, 0x1A0616E0);
        gfx_.text(lx - w * 0.5f, ly - 21, t, Font::Small, Color(0xFF9BEAFF));
    }
    // ---- harsh hits: a red pin over each hit, at its pitch.
    for (int a = 0; a < kRows; ++a) {
        const int idx = sp.landIdx(a);
        if (!sp.landHarsh[idx]) continue;
        const float x = freqX(std::max(30.0f, sp.landHarshHz[idx]));
        const int pi = std::clamp(static_cast<int>(std::lround((x + kXW) / (2.0f * kXW) * (kPts - 1))), 0, kPts - 1);
        const float y0 = h(a, pi), y1 = y0 + 0.22f;
        const float fade = 1.0f - 0.6f * static_cast<float>(a) / kRows;
        float x0s, y0s, x1s, y1s;
        P(x, y0, rowZ(a), x0s, y0s);
        P(x, y1, rowZ(a), x1s, y1s);
        gfx_.line(x0s, y0s, x1s, y1s, 1.6f, Color(0xFF6A3CFF).alpha(0.8f * fade));
        const float r = 4.5f * fade + 1.5f;
        gfx_.triangle(x1s - r, y1s, x1s + r, y1s, x1s, y1s - r * 1.3f, Color(0xFF3B1EFF).alpha(fade));
        gfx_.triangle(x1s - r, y1s, x1s + r, y1s, x1s, y1s + r * 1.3f, Color(0xFFB08AFF).alpha(fade));
    }

    // ---- axis labels
    for (const auto& [hz, name] : {std::pair{50.0f, "50"}, {100.0f, "100"}, {200.0f, "200"}, {500.0f, "500"}, {1000.0f, "1k"},
                                   {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"}}) {
        float sx, sy;
        P(freqX(hz), 0, -0.5f, sx, sy);
        gfx_.text(sx - gfx_.textWidth(name, Font::Small) * 0.5f, sy + 2, name, Font::Small, Color(kDim).alpha(0.9f));
    }
    {
        float sx, sy;
        P(kXW + 0.06f, 0, 0, sx, sy);
        gfx_.text(sx, sy - 8, "NOW", Font::Small, Color(0xFFE2A8FF));
        P(kXW + 0.06f, 0, kZD, sx, sy);
        gfx_.text(sx, sy - 8, fmt("%.1f s", kRows * Spectrogram::kLSub * Spectrogram::kHop / sr_), Font::Small, kDim);
    }

    // ---- bottom bar: what it found, and the fixes
    const int hits10 = sp.hitsWithin(10.0);
    const float hz10 = sp.hitHzWithin(10.0);
    {
        const Rect chip{ox + 20, oy + 404, ox + 262, oy + 436};
        gfx_.roundRect(chip, 7, hits10 ? Color(0x3A1206F0) : Color(0x0B1018E0));
        gfx_.rectOutline(chip, Color(0xFF5A2AFF).alpha(hits10 ? 0.8f : 0.25f), 1.2f);
        gfx_.triangle(chip.x0 + 12, chip.cy(), chip.x0 + 20, chip.cy(), chip.x0 + 16, chip.cy() - 7, 0xFF3B1EFF);
        gfx_.triangle(chip.x0 + 12, chip.cy(), chip.x0 + 20, chip.cy(), chip.x0 + 16, chip.cy() + 7, 0xFFB08AFF);
        const std::string t = hits10 ? fmt("HARSH HITS %.0f", hits10) + fmt("  ~%.1f kHz", hz10 / 1000.0) : std::string("HARSH HITS  none");
        gfx_.text(chip.x0 + 28, chip.y0 + 7, t, Font::Small, hits10 ? Color(0xFFC0A0FF) : kDim);
        const Rect tb{ox + 268, oy + 404, ox + 384, oy + 436};
        if (button(4701, tb)) { screenTab_[DK::kResonance] = 1; findHarsh(); }
        gfx_.roundRect(tb, 7, harshStem_ == selected_ ? Color(kYellow).alpha(0.85f) : Color(0x05080BE0));
        gfx_.rectOutline(tb, Color(kOrange).alpha(hotId_ == 4701 ? 0.95f : 0.55f), 1.4f);
        gfx_.textIn(tb, "TAME IT", Font::Small, harshStem_ == selected_ ? Color(0x140800FF) : kText);
        if (hotId_ == 4701) { tooltip_ = "While it plays: the De-esser (RESONANCE screen) finds the harsh hits and turns them down"; tipX_ = tb.cx(); tipY_ = tb.y0 - 4; tipSec_ = curSec_; }
    }
    {
        const Rect chip{ox + 398, oy + 404, ox + 584, oy + 436};
        gfx_.roundRect(chip, 7, ringOn ? Color(0x2A0626F0) : Color(0x0B1018E0));
        gfx_.rectOutline(chip, Color(0xFF3BD4FF).alpha(ringOn ? 0.8f : 0.25f), 1.2f);
        const std::string t = ringOn ? (sp.ringHz >= 1000 ? fmt("RING %.2f kHz", sp.ringHz / 1000.0) : fmt("RING %.0f Hz", sp.ringHz)) : std::string("RING  none");
        gfx_.text(chip.x0 + 12, chip.y0 + 7, t, Font::Small, ringOn ? Color(0xFF9BEAFF) : kDim);
        const Rect cb{ox + 590, oy + 404, ox + 700, oy + 436};
        if (button(4702, cb)) { findPing(); screenTab_[DK::kDestroyer] = 2; }
        gfx_.roundRect(cb, 7, Color(0x05080BE0));
        gfx_.rectOutline(cb, Color(0xFF3BD4FF).alpha(hotId_ == 4702 ? 0.95f : 0.55f), 1.4f);
        gfx_.textIn(cb, "CUT IT", Font::Small, kText);
        if (hotId_ == 4702) { tooltip_ = "While it rings: the EQ (EQ tab) cuts the ringing tone 9 dB"; tipX_ = cb.cx() - 100; tipY_ = cb.y0 - 4; tipSec_ = curSec_; }
    }
    if (focus_ == DK::kDestroyer && !sel().loaded)
        gfx_.textIn(view, "Load stems and press play", Font::Normal, kDim);
    else if (focus_ == DK::kDestroyer)
        gfx_.text(view.x0 + 6, view.y0 + 2, "DRAG TO TURN  /  DOUBLE-CLICK TO RESET", Font::Small, Color(kDim).alpha(0.6f));
}

// ====================================================================================== the desk
void App::drawDesk() {
    setSection(Sec::Canvas);
    gfx_.image(0, {0, 0, DK::kDeskTexW, DK::kDeskTexH}, {0, 0, DK::kDeskW, DK::kDeskH});
    // (The monitors' glass is already black in the art; shapes are always drawn over images, so no
    // fill here — it would cover the faceplate art.)
}

void App::drawScreen(int i) {
    static const Sec secs[DK::kNumScreens] = {Sec::Destroyer, Sec::NowPlaying, Sec::Resonance, Sec::Transient, Sec::Console, Sec::Mastering};
    const DK::Screen& sc = DK::kScreens[i];
    setSection(secs[i]);
    const int tab = screenTab_[i];
    if (sc.skin.w() > 0 && tab == 0) gfx_.image(1, sc.skin, sc.src);
    const float ox = sc.src.x0, oy = sc.src.y0;
    switch (i) {
    case DK::kDestroyer: if (tab == 0) drawDestroyer(); else if (tab == 1) drawEqPanel(ox, oy); else drawScope3D(ox, oy); break;
    case DK::kResonance: if (tab == 0) drawResonance(); else drawDeEssPanel(ox, oy); break;
    case DK::kTransient: if (tab == 0) drawTransient(); else if (tab == 1) drawCompPanel(ox, oy); else drawGatePanel(ox, oy); break;
    case DK::kMastering: drawMastering(); break;
    case DK::kConsole: drawTransport(); drawArrange(); break;
    case DK::kNowPlaying: drawNowPlaying(); break;
    default: break;
    }
    // The three per-stem screens show (and, in focus, pick) the stem they work on.
    if (i == DK::kDestroyer || i == DK::kResonance || i == DK::kTransient) {
        setSection(secs[i]);
        static const char* t0[] = {"DESTROYER", "EQ", "3D"};
        static const char* t2[] = {"RESONANCE", "DE-ESSER"};
        static const char* t3[] = {"TRANSIENT", "COMP", "GATE"};
        if (i == DK::kDestroyer) drawTabs(i, sc.src.x1 - 12, sc.src.y0 + 3, t0, 3);
        else if (i == DK::kResonance) drawTabs(i, sc.src.x1 - 12, sc.src.y0 + 3, t2, 2);
        else drawTabs(i, sc.src.x1 - 12, sc.src.y0 + 3, t3, 3);
        // (The stem picker sits in the top bar while this screen is open — see drawDeskChrome.)
    }
}

void App::drawDeskChrome() {
    setSection(Sec::Canvas);
    constexpr float kFar = 100000.0f;
    if (focus_ < 0) {
        // Whole desk: hover lights a monitor's edge; a click opens that screen.
        int hover = -1;
        for (int i = 0; i < DK::kNumScreens; ++i)
            if (DK::kScreens[i].glass.contains(in_.mx, in_.my)) hover = i;
        for (int i = 0; i < DK::kNumScreens; ++i) {
            hoverGlow_[i] += ((i == hover ? 1.0f : 0.0f) - hoverGlow_[i]) * 0.3f;
            if (hoverGlow_[i] < 0.02f) continue;
            const Rect g = DK::kScreens[i].glass;
            gfx_.rectOutline({g.x0 - 4, g.y0 - 4, g.x1 + 4, g.y1 + 4}, Color(kCyan).alpha(0.85f * hoverGlow_[i]), 4.0f);
            const std::string tag = std::string(DK::kScreens[i].name) + "  -  click to open";
            const float w = gfx_.textWidth(tag, Font::Large) + 30;
            const Rect t{g.cx() - w / 2, g.y1 - 52, g.cx() + w / 2, g.y1 - 10};
            gfx_.roundRect(t, 8, Color(0x000000FF).alpha(0.8f * hoverGlow_[i]));
            gfx_.textIn(t, tag, Font::Large, Color(kCyan).alpha(hoverGlow_[i]));
        }
        if (hover >= 0) {
            hotId_ = 9300 + hover;
            if (in_.pressed) setFocus(hover);
        }
        // The audio interface on the desk plays / stops; the crown is the about box.
        const bool onBox = DK::kInterfaceBox.contains(in_.mx, in_.my), onCrown = DK::kCrown.contains(in_.mx, in_.my);
        const bool playing = eng_->isPlaying();
        if (playing || onBox) {
            const Rect b = DK::kInterfaceBox;
            gfx_.rectOutline({b.x0 - 3, b.y0 - 3, b.x1 + 3, b.y1 + 3}, Color(kCyan).alpha(onBox ? 0.9f : 0.45f), 3.0f);
        }
        if (onBox) {
            hotId_ = 9310;
            tooltip_ = playing ? "Stop (Space)" : (loadedCount() ? "Play (Space)" : "Load stems first - open the CONSOLE screen");
            tipX_ = DK::kInterfaceBox.cx(); tipY_ = DK::kInterfaceBox.y0 - 10; tipSec_ = Sec::Canvas;
            if (in_.pressed) {
                if (playing) eng_->stop();
                else if (loadedCount()) eng_->play();
                else setFocus(DK::kConsole);
            }
        }
        if (onCrown) {
            hotId_ = 9311;
            gfx_.rectOutline({DK::kCrown.x0 - 3, DK::kCrown.y0 - 3, DK::kCrown.x1 + 3, DK::kCrown.y1 + 3}, Color(kYellow).alpha(0.7f), 3.0f);
            if (in_.pressed) showAbout();
        }
    } else {
        // One screen fills the window: black out the rest of the desk around it.
        const Rect v = viewRect();
        const Color black = 0x040506FF;
        gfx_.rect({-kFar, -kFar, v.x0, kFar}, black);
        gfx_.rect({v.x1, -kFar, kFar, kFar}, black);
        gfx_.rect({v.x0, -kFar, v.x1, v.y0}, black);
        gfx_.rect({v.x0, v.y1, v.x1, kFar}, black);
    }

    // Screen bar, top-right of the window: < DESK > (on the desk: just the hint).
    setSection(Sec::Overlay);
    const float th = gfx_.lineHeight(Font::Small) + 12, tw = gfx_.textWidth("DESK", Font::Small) + 40, aw = th + 6, gap = 6;
    const float right = 1438.0f - 10;
    if (focus_ >= 0) {
        const Rect next{right - aw, 8, right, 8 + th};
        const Rect desk{next.x0 - gap - tw, 8, next.x0 - gap, 8 + th};
        const Rect prev{desk.x0 - gap - aw, 8, desk.x0 - gap, 8 + th};
        if (button(9001, desk)) setFocus(-1);
        if (button(9002, prev)) setFocus((focus_ + DK::kNumScreens - 1) % DK::kNumScreens);
        if (button(9003, next)) setFocus((focus_ + 1) % DK::kNumScreens);
        const Rect rs[3] = {prev, desk, next};
        const char* labels[3] = {"<", "DESK", ">"};
        for (int i = 0; i < 3; ++i) {
            const bool hot = hotId_ == 9002 + (i == 0 ? 0 : (i == 1 ? -1 : 1));
            gfx_.roundRect(rs[i], 6, i == 1 ? Color(kCyan).alpha(0.85f) : Color(0x05080BE0));
            gfx_.rectOutline(rs[i], Color(kCyan).alpha(hot ? 0.9f : 0.45f), 1.5f);
            gfx_.textIn(rs[i], labels[i], Font::Small, i == 1 ? Color(0x041016FF) : kText);
        }
        if (focus_ == DK::kDestroyer || focus_ == DK::kResonance || focus_ == DK::kTransient)
            drawStemPicker(9100, 10, 8, th, 520);   // which stem this screen works on
        const std::string nm = DK::kScreens[focus_].name;
        const float nw = gfx_.textWidth(nm, Font::Small) + 24;
        const Rect name{prev.x0 - gap - nw, 8, prev.x0 - gap, 8 + th};
        gfx_.roundRect(name, 6, Color(0x05080BC8));
        gfx_.textIn(name, nm, Font::Small, kCyan);
    }
}

// Per-stem screens (DESTROYER / RESONANCE / TRANSIENT), top-left: which stem the screen works on, with < >
// to step through the loaded stems (empty slots are skipped). Same stem as the one selected on CONSOLE.
void App::drawStemPicker(int idBase, float x0, float y0, float h, float maxW) {
    std::vector<int> loaded;
    for (int i = 0; i < static_cast<int>(kNumStems); ++i) if (slots_[static_cast<std::size_t>(i)].loaded) loaded.push_back(i);
    const float aw = h + 8;   // arrow button width
    const float nw = std::max(60.0f, std::min(gfx_.textWidth("STEM 13 - PERCUSSION", Font::Small) + 30, maxW - 2 * aw - 8));
    const Rect prev{x0, y0, x0 + aw, y0 + h};
    const Rect box{prev.x1 + 4, y0, prev.x1 + 4 + nw, y0 + h};
    const Rect next{box.x1 + 4, y0, box.x1 + 4 + aw, y0 + h};
    if (!loaded.empty()) {
        // Position of the current stem among the loaded ones (or the first loaded one, if an empty slot is selected).
        auto it = std::find(loaded.begin(), loaded.end(), selected_);
        int k = it == loaded.end() ? -1 : static_cast<int>(it - loaded.begin());
        const int n = static_cast<int>(loaded.size());
        if (button(idBase, prev)) selected_ = loaded[static_cast<std::size_t>(k < 0 ? 0 : (k + n - 1) % n)];
        if (button(idBase + 1, next) || button(idBase + 2, box)) selected_ = loaded[static_cast<std::size_t>(k < 0 ? 0 : (k + 1) % n)];
    }
    for (int i = 0; i < 3; ++i) {
        const Rect r = i == 0 ? prev : (i == 1 ? box : next);
        gfx_.roundRect(r, 6, Color(0x05080BE0));
        gfx_.rectOutline(r, Color(kCyan).alpha(hotId_ == idBase + (i == 0 ? 0 : (i == 1 ? 2 : 1)) ? 0.8f : 0.4f), 1.5f);
    }
    gfx_.textIn(prev, "<", Font::Small, kText);
    gfx_.textIn(next, ">", Font::Small, kText);
    std::string label = "NO STEMS LOADED";
    if (!loaded.empty() && sel().loaded) {
        label = fmt("STEM %.0f - ", static_cast<double>(selected_ + 1));
        for (char c : shownName(selected_)) label += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        while (label.size() > 4 && gfx_.textWidth(label, Font::Small) > nw - 16) label.pop_back();
    } else if (!loaded.empty()) label = "PICK A STEM >";
    gfx_.textIn(box, label, Font::Small, loaded.empty() ? kDim : kCyan);
}

// ====================================================================================== window procedure
LRESULT App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        gfx_.resize(LOWORD(l), HIWORD(l));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        mm->ptMinTrackSize.x = 420; mm->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(h); in_.pressed = true; return 0;
    case WM_LBUTTONUP: ReleaseCapture(); in_.released = true; return 0;
    case WM_LBUTTONDBLCLK: in_.dbl = true; in_.pressed = true; return 0;
    case WM_RBUTTONDOWN: in_.rpressed = true; return 0;
    case WM_MOUSEWHEEL: in_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA; return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (w == VK_SPACE) { if (eng_->isPlaying()) eng_->stop(); else if (loadedCount()) eng_->play(); }
        else if (w == VK_HOME) eng_->seek(0);
        else if (w == VK_TAB) {
            const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            setFocus(focus_ < 0 ? (back ? DK::kNumScreens - 1 : 0) : (focus_ + (back ? DK::kNumScreens - 1 : 1)) % DK::kNumScreens);
        }
        else if (w == VK_F9) setFocus(-1);
        else if (w == VK_ESCAPE && focus_ >= 0) setFocus(-1);
        else if (w == VK_F11 || (w == VK_ESCAPE && fullscreen_)) toggleFullscreen();
        else if (!ctrl && w == 'L') toggleLoop();
        else if (ctrl && w == 'L' && !(GetKeyState(VK_SHIFT) & 0x8000)) loadFolder();
        else if (ctrl && w == 'L') loadFiles(openFilesDialog(hwnd_, L"Load Stems (up to 13 WAV files)", {L"WAV audio (*.wav)", L"*.wav"}, true));
        else if (ctrl && w == 'O') openSession();
        else if (ctrl && w == 'S') saveSessionAs();
        else if (ctrl && w == 'E') exportMaster();
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = reinterpret_cast<HDROP>(w);
        const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        bool droppedFolder = false;
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW(d, i, buf, static_cast<UINT>(std::size(buf)))) {
                std::wstring f = buf;
                std::error_code ec;
                if (std::filesystem::is_directory(std::filesystem::path(f), ec)) {   // a dropped folder: its WAVs
                    for (auto& wv : wavsInFolder(f)) files.push_back(std::move(wv));
                    droppedFolder = true;
                    continue;
                }
                std::wstring ext = f.size() > 4 ? f.substr(f.size() - 4) : L"";
                for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (ext == L".wav") files.push_back(f);
                else if (ext == L".hcs") { toast("Use the menu → Open Session for .hcs files"); }
            }
        }
        DragFinish(d);
        std::sort(files.begin(), files.end());
        if (droppedFolder) loadStemList(std::move(files));   // a folder = a song: it replaces what's loaded
        else loadFiles(files);                              // loose WAVs fill the empty slots
        return 0;
    }
    case WM_EXPORT_DONE: onExportDone(); return 0;
    case WM_CLOSE:
        if (exporting_ && MessageBoxW(h, L"An export is still running. Quit anyway?", L"Hybrid Core", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    simd::ScopedFlushDenormals ftz;
    static App app;
    g_app = &app;
    if (!app.init(inst, show)) return 1;
    app.openFromCommandLine();
    const int rc = app.run();
    CoUninitialize();
    return rc;
}
