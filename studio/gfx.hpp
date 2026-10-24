// =============================================================================
//  studio/gfx.hpp — Graphics Engine (Direct3D 11). Windows only.
//
//  The faceplate art is drawn first as one textured quad (mip-mapped, so it
//  stays clean at any window size). Every live control is then drawn on top
//  through ONE dynamic vertex buffer in ONE draw call:
//    vertex = { x, y (device px), u, v (glyph atlas), rgba }
//    pixel  = rgba · atlas.r   (the atlas has a white block for solid shapes)
//  The UI is written in FACEPLATE IMAGE PIXELS; setTransform() maps that
//  space onto the window, so every control lands exactly on its painted spot
//  at any size. Text comes from a glyph atlas rasterised with GDI (part of
//  Windows) at the current scale, so it is always pixel-sharp.
//  Geometry renders into a 4x MSAA target that is resolved into a flip-model
//  swap chain: knob arcs and round shapes are anti-aliased.
// =============================================================================
#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hc::gfx {

// 0xRRGGBBAA
struct Color {
    std::uint32_t rgba = 0xFFFFFFFF;
    constexpr Color() = default;
    constexpr Color(std::uint32_t v) : rgba(v) {}
    Color alpha(float a) const {
        float na = static_cast<float>(rgba & 0xFF) * a;
        if (na < 0) na = 0;
        if (na > 255) na = 255;
        return Color((rgba & 0xFFFFFF00u) | static_cast<std::uint32_t>(na));
    }
    static Color lerp(Color a, Color b, float t);
};

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    Rect inset(float d) const { return {x0 + d, y0 + d, x1 - d, y1 - d}; }
};

enum class Font : int { Small = 0, Normal = 1, Large = 2, Huge = 3 };
enum class Align { Left, Center, Right };

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    std::string init(HWND hwnd);                       // "" on success
    void resize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    std::string adapterName() const { return adapter_; }

    // Images (RGBA8, any size), uploaded once with a full mip chain:
    //   layer 0 = backdrop (the desk), layer 1 = skin (faceplate art).
    std::string setSkin(const std::uint8_t* rgba, int w, int h) { return setImage(1, rgba, w, h); }
    std::string setBackdrop(const std::uint8_t* rgba, int w, int h) { return setImage(0, rgba, w, h); }
    // Draws image px rect `src` of a layer into `dst` (current coordinates). All backdrop quads are drawn
    // first, then all skin quads, then every shape and glyph — so images always sit underneath.
    void image(int layer, const Rect& src, const Rect& dst, Color tint = 0xFFFFFFFF);
    // Scale the glyph atlas is rasterised at (device px per coordinate unit of the screen in focus).
    // Text drawn at any other scale is resized from it. 0 = use the current transform's scale.
    void setTextScale(float s) { textScaleHint_ = s; }
    // Image-space → device: dev = offset + img * scale.
    void setTransform(float scale, float offX, float offY);
    float scale() const { return scale_; }

    void beginFrame(Color clear);
    void endFrame();

    // ---- primitives (IMAGE coordinates) ----
    void rect(const Rect& r, Color c);
    void rectOutline(const Rect& r, Color c, float t = 1.5f);
    void roundRect(const Rect& r, float radius, Color c);
    void gradientV(const Rect& r, Color top, Color bottom);
    void line(float x0, float y0, float x1, float y1, float t, Color c);
    void polyline(const float* xy, std::size_t n, float t, Color c);
    void arc(float cx, float cy, float r, float a0, float a1, float t, Color c);   // radians, clockwise on screen
    void circle(float cx, float cy, float r, Color c);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);   // filled
    void circleGradient(float cx, float cy, float r, Color top, Color bottom);
    void ring(float cx, float cy, float r, float t, Color c) { arc(cx, cy, r, 0.0f, 6.2831853f, t, c); }
    // Filled polygon below a curve down to baseline y (for spectra).
    void fillUnder(const float* xy, std::size_t n, float baseY, Color top, Color bottom);

    // ---- text (IMAGE coordinates; sizes are image px) ----
    float textWidth(std::string_view s, Font f) const;
    float lineHeight(Font f) const;
    void text(float x, float y, std::string_view s, Font f, Color c);           // y = top
    void textIn(const Rect& r, std::string_view s, Font f, Color c, Align a = Align::Center);
    std::string ellipsize(std::string_view s, Font f, float maxW) const;

private:
    struct Vertex { float x, y, u, v; std::uint32_t c; };
    struct Glyph { float u0, v0, u1, v1, w, h, adv; };
    struct FontInfo { Glyph g[96]{}; float lineH = 0; };

    float X(float x) const { return offX_ + x * scale_; }
    float Y(float y) const { return offY_ + y * scale_; }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t c);
    void triC(float x0, float y0, std::uint32_t c0, float x1, float y1, std::uint32_t c1, float x2, float y2, std::uint32_t c2);
    void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, std::uint32_t c);
    std::string buildAtlas();
    std::string createTargets();
    void releaseTargets();

    HWND hwnd_ = nullptr;
    std::string adapter_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID3D11Texture2D* msaaTex_ = nullptr;
    ID3D11RenderTargetView* msaaRtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* psAtlas_ = nullptr;
    ID3D11PixelShader* psImage_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vb_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11Texture2D* atlasTex_ = nullptr;
    ID3D11ShaderResourceView* atlasSrv_ = nullptr;
    std::string setImage(int layer, const std::uint8_t* rgba, int w, int h);
    ID3D11Texture2D* imgTex_[2] = {};
    ID3D11ShaderResourceView* imgSrv_[2] = {};
    int imgW_[2] = {}, imgH_[2] = {};
    std::vector<Vertex> imgVerts_[2];
    float textScaleHint_ = 0.0f;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    std::size_t vbCapacity_ = 0;
    UINT msaa_ = 1;
    int width_ = 0, height_ = 0;
    float scale_ = 1.0f, offX_ = 0.0f, offY_ = 0.0f, atlasScale_ = 0.0f;
    float clear_[4] = {0, 0, 0, 1};
    float whiteU_ = 0, whiteV_ = 0;
    FontInfo fonts_[4];
    std::vector<Vertex> verts_;
};

} // namespace hc::gfx
