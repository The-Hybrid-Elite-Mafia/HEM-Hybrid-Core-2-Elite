// =============================================================================
//  studio/gfx.cpp — Direct3D 11 implementation of the Graphics Engine.
// =============================================================================
#include "gfx.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hc::gfx {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr float kPi = 3.14159265358979f;

// Image-px font sizes and weights: Small, Normal, Large, Huge (digits).
constexpr float kFontPx[4] = {15.0f, 19.0f, 26.0f, 44.0f};
constexpr float kFontMinPx[4] = {8.0f, 9.0f, 11.0f, 14.0f};
constexpr int kFontWeight[4] = {FW_SEMIBOLD, FW_SEMIBOLD, FW_SEMIBOLD, FW_LIGHT};

const char* kShader = R"(
cbuffer CB : register(b0) { float2 invSize; float2 pad; };
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
PSIn vsMain(VSIn i) {
    PSIn o;
    o.pos = float4(i.pos.x * invSize.x * 2.0 - 1.0, 1.0 - i.pos.y * invSize.y * 2.0, 0.0, 1.0);
    o.uv = i.uv; o.col = i.col; return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 psAtlas(PSIn i) : SV_TARGET { return float4(i.col.rgb, i.col.a * tex.Sample(smp, i.uv).r); }
float4 psImage(PSIn i) : SV_TARGET { return tex.Sample(smp, i.uv) * i.col; }
)";

std::string hrMsg(const char* what, HRESULT h) {
    char b[160];
    std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, static_cast<unsigned long>(h));
    return b;
}

std::uint32_t pack(Color c) {   // 0xRRGGBBAA → R8G8B8A8_UNORM little-endian
    const std::uint32_t r = (c.rgba >> 24) & 0xFF, g = (c.rgba >> 16) & 0xFF, b = (c.rgba >> 8) & 0xFF, a = c.rgba & 0xFF;
    return r | (g << 8) | (b << 16) | (a << 24);
}

ID3DBlob* compile(const char* entry, const char* target, std::string& err) {
    ID3DBlob* code = nullptr;
    ID3DBlob* msg = nullptr;
    const HRESULT h = D3DCompile(kShader, std::strlen(kShader), "hc_ui", nullptr, nullptr, entry, target,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg);
    if (FAILED(h)) {
        err = std::string("shader compile failed: ") + (msg ? static_cast<const char*>(msg->GetBufferPointer()) : entry);
        if (msg) msg->Release();
        return nullptr;
    }
    if (msg) msg->Release();
    return code;
}

} // namespace

Color Color::lerp(Color a, Color b, float t) {
    auto ch = [&](int s) {
        const float x = static_cast<float>((a.rgba >> s) & 0xFF), y = static_cast<float>((b.rgba >> s) & 0xFF);
        return static_cast<std::uint32_t>(std::clamp(x + (y - x) * t, 0.0f, 255.0f)) << s;
    };
    return Color(ch(24) | ch(16) | ch(8) | ch(0));
}

Renderer::~Renderer() {
    releaseTargets();
    rel(vs_); rel(psAtlas_); rel(psImage_); rel(layout_); rel(vb_); rel(cb_);
    rel(atlasSrv_); rel(atlasTex_);
    for (int i = 0; i < 2; ++i) { rel(imgSrv_[i]); rel(imgTex_[i]); }
    rel(sampler_); rel(blend_); rel(raster_); rel(swap_); rel(ctx_); rel(dev_);
}

std::string Renderer::init(HWND hwnd) {
    hwnd_ = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    width_ = std::max<int>(1, rc.right - rc.left);
    height_ = std::max<int>(1, rc.bottom - rc.top);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(h))   // no usable GPU driver: WARP software rasteriser still runs the UI
        h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                              D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(h)) return hrMsg("D3D11CreateDevice", h);

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    dev_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
    if (dxgiDev) dxgiDev->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(adapter->GetDesc(&d))) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
            adapter_ = name;
        }
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    }
    if (!factory) { rel(adapter); rel(dxgiDev); return "DXGI 1.2 factory unavailable (Windows 8 or newer is required)"; }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(width_); sd.Height = static_cast<UINT>(height_);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    h = factory->CreateSwapChainForHwnd(dev_, hwnd, &sd, nullptr, nullptr, &swap_);
    if (FAILED(h)) {   // older Windows 10 builds / remote sessions: fall back to sequential flip
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        h = factory->CreateSwapChainForHwnd(dev_, hwnd, &sd, nullptr, nullptr, &swap_);
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    rel(factory); rel(adapter); rel(dxgiDev);
    if (FAILED(h)) return hrMsg("CreateSwapChainForHwnd", h);

    UINT q = 0;
    if (SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q)) && q > 0) msaa_ = 4;

    std::string err;
    ID3DBlob* vsb = compile("vsMain", "vs_4_0", err);
    if (!vsb) return err;
    ID3DBlob* pab = compile("psAtlas", "ps_4_0", err);
    if (!pab) { vsb->Release(); return err; }
    ID3DBlob* pib = compile("psImage", "ps_4_0", err);
    if (!pib) { vsb->Release(); pab->Release(); return err; }
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(pab->GetBufferPointer(), pab->GetBufferSize(), nullptr, &psAtlas_);
    dev_->CreatePixelShader(pib->GetBufferPointer(), pib->GetBufferSize(), nullptr, &psImage_);
    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    h = dev_->CreateInputLayout(ied, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    vsb->Release(); pab->Release(); pib->Release();
    if (FAILED(h) || !vs_ || !psAtlas_ || !psImage_) return hrMsg("shader/input layout creation", h);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&cbd, nullptr, &cb_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev_->CreateSamplerState(&smp, &sampler_);

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bd, &blend_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = msaa_ > 1 ? TRUE : FALSE;
    dev_->CreateRasterizerState(&rd, &raster_);

    return createTargets();
}

std::string Renderer::createTargets() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width_); td.Height = static_cast<UINT>(height_);
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = msaa_; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT h = dev_->CreateTexture2D(&td, nullptr, &msaaTex_);
    if (FAILED(h)) return hrMsg("CreateTexture2D(render target)", h);
    h = dev_->CreateRenderTargetView(msaaTex_, nullptr, &msaaRtv_);
    if (FAILED(h)) return hrMsg("CreateRenderTargetView", h);
    return {};
}

void Renderer::releaseTargets() { rel(msaaRtv_); rel(msaaTex_); }

void Renderer::resize(int w, int h) {
    if (!swap_ || w <= 0 || h <= 0 || (w == width_ && h == height_)) return;
    width_ = w; height_ = h;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    releaseTargets();
    swap_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0);
    createTargets();
}

std::string Renderer::setImage(int layer, const std::uint8_t* rgba, int w, int h) {
    rel(imgSrv_[layer]); rel(imgTex_[layer]);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h);
    td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &imgTex_[layer]);
    if (FAILED(hr)) return hrMsg(layer ? "CreateTexture2D(skin)" : "CreateTexture2D(backdrop)", hr);
    ctx_->UpdateSubresource(imgTex_[layer], 0, nullptr, rgba, static_cast<UINT>(w * 4), 0);
    hr = dev_->CreateShaderResourceView(imgTex_[layer], nullptr, &imgSrv_[layer]);
    if (FAILED(hr)) return hrMsg("CreateShaderResourceView(image)", hr);
    ctx_->GenerateMips(imgSrv_[layer]);
    imgW_[layer] = w; imgH_[layer] = h;
    return {};
}

void Renderer::image(int layer, const Rect& src, const Rect& dst, Color tint) {
    if (layer < 0 || layer > 1 || !imgW_[layer]) return;
    const float x0 = X(dst.x0), y0 = Y(dst.y0), x1 = X(dst.x1), y1 = Y(dst.y1);
    const float u0 = src.x0 / imgW_[layer], v0 = src.y0 / imgH_[layer], u1 = src.x1 / imgW_[layer], v1 = src.y1 / imgH_[layer];
    const auto c = pack(tint);
    auto& v = imgVerts_[layer];
    v.push_back({x0, y0, u0, v0, c}); v.push_back({x1, y0, u1, v0, c}); v.push_back({x1, y1, u1, v1, c});
    v.push_back({x0, y0, u0, v0, c}); v.push_back({x1, y1, u1, v1, c}); v.push_back({x0, y1, u0, v1, c});
}

void Renderer::setTransform(float s, float ox, float oy) { scale_ = s; offX_ = ox; offY_ = oy; }

// ---------------------------------------------------------------------------- glyph atlas
std::string Renderer::buildAtlas() {
    rel(atlasSrv_); rel(atlasTex_);
    const float scale = textScaleHint_ > 0 ? textScaleHint_ : scale_;
    atlasScale_ = scale;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT hf[4];
    int px[4];
    for (int f = 0; f < 4; ++f) {
        px[f] = static_cast<int>(std::lround(std::max(kFontMinPx[f], kFontPx[f] * scale)));
        hf[f] = CreateFontW(-px[f], 0, 0, 0, kFontWeight[f], FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
    // Pass 1: measure and pack.
    const int W = 1024;
    struct Cell { int x, y, w, h; };
    Cell cells[4][96];
    int x = 4, y = 0, rowH = 0;   // 4×4 white block lives at (0,0)
    for (int f = 0; f < 4; ++f) {
        SelectObject(dc, hf[f]);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        fonts_[f].lineH = static_cast<float>(tm.tmHeight);
        for (int c = 0; c < 96; ++c) {
            wchar_t ch = static_cast<wchar_t>(32 + c);
            if (c == 95) ch = L'·';   // slot 127 → middle dot
            SIZE sz; GetTextExtentPoint32W(dc, &ch, 1, &sz);
            const int cw = sz.cx + 4, chh = tm.tmHeight + 2;
            if (x + cw > W) { x = 0; y += rowH + 1; rowH = 0; }
            cells[f][c] = {x, y, cw, chh};
            Glyph& g = fonts_[f].g[c];
            g.w = static_cast<float>(cw); g.h = static_cast<float>(chh); g.adv = static_cast<float>(sz.cx);
            x += cw + 1; rowH = std::max(rowH, chh);
        }
    }
    int H = 64;
    while (H < y + rowH + 2) H *= 2;
    // Pass 2: rasterise white-on-black into a DIB, keep the red channel.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { for (auto f : hf) DeleteObject(f); DeleteDC(dc); return "CreateDIBSection failed"; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<std::size_t>(W) * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int f = 0; f < 4; ++f) {
        SelectObject(dc, hf[f]);
        for (int c = 0; c < 96; ++c) {
            wchar_t ch = static_cast<wchar_t>(32 + c);
            if (c == 95) ch = L'·';
            TextOutW(dc, cells[f][c].x + 2, cells[f][c].y + 1, &ch, 1);
        }
    }
    GdiFlush();
    std::vector<std::uint8_t> r8(static_cast<std::size_t>(W) * H);
    const auto* src = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < r8.size(); ++i) {
        const std::uint8_t b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
        r8[i] = std::max({r, g, b});
    }
    for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) r8[static_cast<std::size_t>(yy) * W + xx] = 255;
    whiteU_ = 2.0f / W; whiteV_ = 2.0f / H;
    for (int f = 0; f < 4; ++f)
        for (int c = 0; c < 96; ++c) {
            Glyph& g = fonts_[f].g[c];
            g.u0 = static_cast<float>(cells[f][c].x) / W; g.v0 = static_cast<float>(cells[f][c].y) / H;
            g.u1 = static_cast<float>(cells[f][c].x + cells[f][c].w) / W; g.v1 = static_cast<float>(cells[f][c].y + cells[f][c].h) / H;
        }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    for (auto f : hf) DeleteObject(f);
    DeleteDC(dc);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = static_cast<UINT>(H); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{r8.data(), static_cast<UINT>(W), 0};
    HRESULT h = dev_->CreateTexture2D(&td, &sd, &atlasTex_);
    if (FAILED(h)) return hrMsg("CreateTexture2D(atlas)", h);
    dev_->CreateShaderResourceView(atlasTex_, nullptr, &atlasSrv_);
    return {};
}

// ---------------------------------------------------------------------------- frame
void Renderer::beginFrame(Color c) {
    const float want = textScaleHint_ > 0 ? textScaleHint_ : scale_;
    if (!atlasSrv_ || std::fabs(want - atlasScale_) > 0.02f * atlasScale_) buildAtlas();
    clear_[0] = static_cast<float>((c.rgba >> 24) & 0xFF) / 255.0f;
    clear_[1] = static_cast<float>((c.rgba >> 16) & 0xFF) / 255.0f;
    clear_[2] = static_cast<float>((c.rgba >> 8) & 0xFF) / 255.0f;
    clear_[3] = 1.0f;
    verts_.clear();
    imgVerts_[0].clear();
    imgVerts_[1].clear();
}

void Renderer::endFrame() {
    if (!msaaRtv_) return;
    const std::size_t n0 = imgVerts_[0].size(), n1 = imgVerts_[1].size();
    const std::size_t bytes = (n0 + n1 + verts_.size()) * sizeof(Vertex);
    if (bytes == 0) return;
    if (bytes > vbCapacity_) {
        rel(vb_);
        vbCapacity_ = std::max<std::size_t>(bytes * 3 / 2, 1u << 20);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(vbCapacity_); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &vb_))) { vbCapacity_ = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* dst = static_cast<Vertex*>(m.pData);
        if (n0) std::memcpy(dst, imgVerts_[0].data(), n0 * sizeof(Vertex));
        if (n1) std::memcpy(dst + n0, imgVerts_[1].data(), n1 * sizeof(Vertex));
        if (!verts_.empty()) std::memcpy(dst + n0 + n1, verts_.data(), verts_.size() * sizeof(Vertex));
        ctx_->Unmap(vb_, 0);
    }
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = static_cast<float*>(m.pData);
        f[0] = 1.0f / width_; f[1] = 1.0f / height_; f[2] = f[3] = 0.0f;
        ctx_->Unmap(cb_, 0);
    }
    ctx_->ClearRenderTargetView(msaaRtv_, clear_);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_);
    ctx_->OMSetRenderTargets(1, &msaaRtv_, nullptr);
    const float bf[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blend_, bf, 0xFFFFFFFF);
    const UINT stride = sizeof(Vertex), off = 0;
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &off);
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    for (int l = 0; l < 2; ++l) {
        const std::size_t n = imgVerts_[l].size();
        if (!n || !imgSrv_[l]) continue;
        ctx_->PSSetShader(psImage_, nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, &imgSrv_[l]);
        ctx_->Draw(static_cast<UINT>(n), static_cast<UINT>(l == 0 ? 0 : n0));
    }
    if (!verts_.empty()) {
        ctx_->PSSetShader(psAtlas_, nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, &atlasSrv_);
        ctx_->Draw(static_cast<UINT>(verts_.size()), static_cast<UINT>(n0 + n1));
    }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        if (msaa_ > 1) ctx_->ResolveSubresource(back, 0, msaaTex_, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        else ctx_->CopyResource(back, msaaTex_);
        back->Release();
    }
    swap_->Present(1, 0);
}

// ---------------------------------------------------------------------------- primitives
void Renderer::tri(float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t c) {
    verts_.push_back({x0, y0, whiteU_, whiteV_, c});
    verts_.push_back({x1, y1, whiteU_, whiteV_, c});
    verts_.push_back({x2, y2, whiteU_, whiteV_, c});
}
void Renderer::triC(float x0, float y0, std::uint32_t c0, float x1, float y1, std::uint32_t c1, float x2, float y2, std::uint32_t c2) {
    verts_.push_back({x0, y0, whiteU_, whiteV_, c0});
    verts_.push_back({x1, y1, whiteU_, whiteV_, c1});
    verts_.push_back({x2, y2, whiteU_, whiteV_, c2});
}
void Renderer::quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, std::uint32_t c) {
    tri(x0, y0, x1, y1, x2, y2, c);
    tri(x0, y0, x2, y2, x3, y3, c);
}

void Renderer::rect(const Rect& r, Color c) {
    const float x0 = X(r.x0), y0 = Y(r.y0), x1 = X(r.x1), y1 = Y(r.y1);
    quad(x0, y0, x1, y0, x1, y1, x0, y1, pack(c));
}

void Renderer::rectOutline(const Rect& r, Color c, float t) {
    rect({r.x0, r.y0, r.x1, r.y0 + t}, c);
    rect({r.x0, r.y1 - t, r.x1, r.y1}, c);
    rect({r.x0, r.y0 + t, r.x0 + t, r.y1 - t}, c);
    rect({r.x1 - t, r.y0 + t, r.x1, r.y1 - t}, c);
}

void Renderer::gradientV(const Rect& r, Color top, Color bottom) {
    const float x0 = X(r.x0), y0 = Y(r.y0), x1 = X(r.x1), y1 = Y(r.y1);
    const auto a = pack(top), b = pack(bottom);
    triC(x0, y0, a, x1, y0, a, x1, y1, b);
    triC(x0, y0, a, x1, y1, b, x0, y1, b);
}

void Renderer::roundRect(const Rect& r, float rad, Color c) {
    rad = std::min({rad, r.w() * 0.5f, r.h() * 0.5f});
    if (rad <= 0.5f) { rect(r, c); return; }
    rect({r.x0 + rad, r.y0, r.x1 - rad, r.y1}, c);
    rect({r.x0, r.y0 + rad, r.x0 + rad, r.y1 - rad}, c);
    rect({r.x1 - rad, r.y0 + rad, r.x1, r.y1 - rad}, c);
    const auto pc = pack(c);
    const float cxs[4] = {r.x0 + rad, r.x1 - rad, r.x1 - rad, r.x0 + rad};
    const float cys[4] = {r.y0 + rad, r.y0 + rad, r.y1 - rad, r.y1 - rad};
    const float start[4] = {kPi, 1.5f * kPi, 0.0f, 0.5f * kPi};
    const int seg = 6;
    for (int k = 0; k < 4; ++k) {
        const float cx = X(cxs[k]), cy = Y(cys[k]), rr = rad * scale_;
        for (int i = 0; i < seg; ++i) {
            const float a0 = start[k] + 0.5f * kPi * i / seg, a1 = start[k] + 0.5f * kPi * (i + 1) / seg;
            tri(cx, cy, cx + rr * std::cos(a0), cy + rr * std::sin(a0), cx + rr * std::cos(a1), cy + rr * std::sin(a1), pc);
        }
    }
}

void Renderer::line(float x0, float y0, float x1, float y1, float t, Color c) {
    const float ax = X(x0), ay = Y(y0), bx = X(x1), by = Y(y1);
    float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float hw = 0.5f * std::max(1.0f, t * scale_);
    dx /= len; dy /= len;
    const float nx = -dy * hw, ny = dx * hw;
    quad(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny, pack(c));
}

void Renderer::polyline(const float* xy, std::size_t n, float t, Color c) {
    for (std::size_t i = 1; i < n; ++i) line(xy[2 * i - 2], xy[2 * i - 1], xy[2 * i], xy[2 * i + 1], t, c);
}

void Renderer::arc(float cx, float cy, float r, float a0, float a1, float t, Color c) {
    if (a1 < a0) std::swap(a0, a1);
    const float R = r * scale_, hw = 0.5f * std::max(1.0f, t * scale_);
    const int seg = std::clamp(static_cast<int>((a1 - a0) * R / 3.0f), 6, 160);
    const float px = X(cx), py = Y(cy);
    const auto pc = pack(c);
    for (int i = 0; i < seg; ++i) {
        const float b0 = a0 + (a1 - a0) * i / seg, b1 = a0 + (a1 - a0) * (i + 1) / seg;
        const float c0 = std::cos(b0), s0 = std::sin(b0), c1 = std::cos(b1), s1 = std::sin(b1);
        quad(px + (R - hw) * c0, py + (R - hw) * s0, px + (R + hw) * c0, py + (R + hw) * s0,
             px + (R + hw) * c1, py + (R + hw) * s1, px + (R - hw) * c1, py + (R - hw) * s1, pc);
    }
}

void Renderer::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    tri(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), pack(c));
}

void Renderer::circle(float cx, float cy, float r, Color c) { circleGradient(cx, cy, r, c, c); }

void Renderer::circleGradient(float cx, float cy, float r, Color top, Color bottom) {
    const float R = r * scale_, px = X(cx), py = Y(cy);
    const int seg = std::clamp(static_cast<int>(R * 0.9f), 12, 96);
    const auto mid = pack(Color::lerp(top, bottom, 0.5f));
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2.0f * kPi * i / seg, a1 = 2.0f * kPi * (i + 1) / seg;
        const float s0 = std::sin(a0), s1 = std::sin(a1);
        triC(px, py, mid, px + R * std::cos(a0), py + R * s0, pack(Color::lerp(top, bottom, 0.5f + 0.5f * s0)),
             px + R * std::cos(a1), py + R * s1, pack(Color::lerp(top, bottom, 0.5f + 0.5f * s1)));
    }
}

void Renderer::fillUnder(const float* xy, std::size_t n, float baseY, Color top, Color bottom) {
    const auto b = pack(bottom), t = pack(top);
    const float by = Y(baseY);
    for (std::size_t i = 1; i < n; ++i) {
        const float x0 = X(xy[2 * i - 2]), y0 = Y(xy[2 * i - 1]), x1 = X(xy[2 * i]), y1 = Y(xy[2 * i + 1]);
        triC(x0, y0, t, x1, y1, t, x1, by, b);
        triC(x0, y0, t, x1, by, b, x0, by, b);
    }
}

// ---------------------------------------------------------------------------- text
// UTF-8 → glyph slots. ASCII renders as-is; a few typographic characters map
// to close ASCII forms; anything else becomes '?'.
static void forEachGlyph(std::string_view s, const auto& fn) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); n = 2; }
        i += n;
        if (cp >= 32 && cp < 127) { fn(static_cast<int>(cp - 32)); continue; }
        switch (cp) {
        case 0xB7: fn(95); break;                                   // middle dot
        case 0x2192: fn('>' - 32); break;                           // →
        case 0x2014: case 0x2013: fn('-' - 32); break;              // — –
        case 0x2026: fn('.' - 32); fn('.' - 32); fn('.' - 32); break; // …
        case 0x2261: fn('=' - 32); break;                           // ≡
        case 0xB0: fn('o' - 32); break;
        default: fn('?' - 32); break;
        }
    }
}

float Renderer::textWidth(std::string_view s, Font f) const {
    float w = 0;
    const FontInfo& fi = fonts_[static_cast<int>(f)];
    forEachGlyph(s, [&](int g) { w += fi.g[g].adv; });
    return atlasScale_ > 0 ? w / atlasScale_ : w;   // glyphs are atlas px; coordinate units = atlas px / atlasScale
}

float Renderer::lineHeight(Font f) const { return atlasScale_ > 0 ? fonts_[static_cast<int>(f)].lineH / atlasScale_ : 0; }

void Renderer::text(float x, float y, std::string_view s, Font f, Color c) {
    const FontInfo& fi = fonts_[static_cast<int>(f)];
    const float k = atlasScale_ > 0 ? scale_ / atlasScale_ : 1.0f;   // 1 when this screen is the one in focus
    const bool snap = std::fabs(k - 1.0f) < 0.02f;
    float px = (snap ? std::round(X(x)) : X(x)) - 2.0f * k;
    const float py = (snap ? std::round(Y(y)) : Y(y)) - 1.0f * k;
    const auto pc = pack(c);
    forEachGlyph(s, [&](int gi) {
        const Glyph& g = fi.g[gi];
        if (gi != 0) {
            const float x0 = px, y0 = py, x1 = px + g.w * k, y1 = py + g.h * k;
            verts_.push_back({x0, y0, g.u0, g.v0, pc}); verts_.push_back({x1, y0, g.u1, g.v0, pc});
            verts_.push_back({x1, y1, g.u1, g.v1, pc}); verts_.push_back({x0, y0, g.u0, g.v0, pc});
            verts_.push_back({x1, y1, g.u1, g.v1, pc}); verts_.push_back({x0, y1, g.u0, g.v1, pc});
        }
        px += g.adv * k;
    });
}

std::string Renderer::ellipsize(std::string_view s, Font f, float maxW) const {
    if (textWidth(s, f) <= maxW) return std::string(s);
    std::string out(s);
    while (!out.empty() && textWidth(out + "..", f) > maxW) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
    }
    return out + "..";
}

void Renderer::textIn(const Rect& r, std::string_view s, Font f, Color c, Align a) {
    const std::string t = ellipsize(s, f, r.w());
    const float w = textWidth(t, f), h = lineHeight(f);
    float x = r.x0;
    if (a == Align::Center) x = r.cx() - 0.5f * w;
    else if (a == Align::Right) x = r.x1 - w;
    text(x, r.cy() - 0.5f * h, t, f, c);
}

} // namespace hc::gfx
