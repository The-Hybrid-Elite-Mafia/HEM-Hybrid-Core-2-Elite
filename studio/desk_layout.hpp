// =============================================================================
//  studio/desk_layout.hpp — HYBRID CORE 2 ELITE: the producer desk.
//
//  The window shows assets/desk.jpg (2880 x 1620, night-city studio desk with six
//  monitors). Each monitor shows one live screen. A screen's controls are written
//  in the ORIGINAL faceplate-art coordinates (skin_layout.hpp); `src` is the part
//  of those coordinates the screen shows, `skin` the same area in assets/skin.jpg
//  (the faceplate art's middle band, 2876 x 910). The app scales `src` to fit the
//  monitor's glass, keeping its shape.
//  Glass rectangles were measured on the 2880 x 1620 desk picture.
// =============================================================================
#pragma once
#include "gfx.hpp"
#include <algorithm>

namespace hc::studio::desk {

inline constexpr float kDeskW = 2880.0f, kDeskH = 1620.0f;          // desk coordinates (glass rects below)
inline constexpr float kDeskTexW = 4096.0f, kDeskTexH = 2304.0f;    // assets/desk.jpg: the full 4K art
inline constexpr float kSkinY0 = 354.0f;   // skin.jpg row 0 = faceplate canvas row 354

struct Screen {
    const char* name;
    gfx::Rect glass;   // monitor glass on the desk picture
    gfx::Rect src;     // screen content, in its section's coordinates
    gfx::Rect skin;    // faceplate art behind it, in skin.jpg pixels (w == 0: no art, drawn live)
};

// Order = screen index (Tab walks it): top row left → right, then bottom row.
inline constexpr int kNumScreens = 6;
inline constexpr int kDestroyer = 0, kNowPlaying = 1, kResonance = 2, kTransient = 3, kConsole = 4, kMastering = 5;
inline constexpr Screen kScreens[kNumScreens] = {
    {"STFT DESTROYER", {425, 295, 926, 615},   {0, 1286, 719, 1741},   {1438, 0, 2157, 455}},
    {"NOW PLAYING",    {947, 295, 1934, 615},  {0, 0, 1480, 480},      {0, 0, 0, 0}},
    {"RESONANCE",      {1956, 295, 2454, 615}, {719, 1286, 1438, 1741}, {2157, 0, 2876, 455}},
    {"TRANSIENT",      {425, 665, 1077, 997},  {0, 1741, 719, 2196},   {1438, 455, 2157, 910}},
    {"CONSOLE",        {1099, 665, 1780, 997}, {0, 452, 1438, 1362},   {0, 0, 1438, 910}},
    {"MASTERING",      {1803, 665, 2454, 997}, {719, 1741, 1438, 2196}, {2157, 455, 2876, 910}},
};

// Things on the desk you can click.
inline constexpr gfx::Rect kInterfaceBox{1215, 1062, 1656, 1179};   // the audio interface → play / stop
inline constexpr gfx::Rect kCrown{2646, 1107, 2844, 1242};          // the crown → about

// Scale and placement of a screen's content inside its glass (shape kept, centred).
inline float screenScale(int i) {
    const Screen& s = kScreens[i];
    return std::min(s.glass.w() / s.src.w(), s.glass.h() / s.src.h());
}
inline gfx::Rect screenDst(int i) {
    const Screen& s = kScreens[i];
    const float k = screenScale(i), w = s.src.w() * k, h = s.src.h() * k;
    const float x0 = s.glass.cx() - 0.5f * w, y0 = 0.5f * (s.glass.y0 + s.glass.y1) - 0.5f * h;
    return {x0, y0, x0 + w, y0 + h};
}

} // namespace hc::studio::desk
