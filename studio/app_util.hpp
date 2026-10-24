// =============================================================================
//  studio/app_util.hpp — Windows helpers for Hybrid Core Studio:
//  UTF-8/UTF-16 conversion, file dialogs, embedded-image decoding (WIC),
//  session file format, tempo estimate, waveform overview.
// =============================================================================
#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include "hc/app/offline_render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hc::studio {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::string stemNameFromPath(const std::wstring& path) {
    std::wstring f = path;
    if (const auto p = f.find_last_of(L"\\/"); p != std::wstring::npos) f = f.substr(p + 1);
    if (const auto d = f.find_last_of(L'.'); d != std::wstring::npos) f = f.substr(0, d);
    return narrow(f);
}

// ---------------------------------------------------------------- file dialogs
struct DialogFilter { const wchar_t* name; const wchar_t* spec; };

inline std::vector<std::wstring> openFilesDialog(HWND owner, const wchar_t* title, const DialogFilter& filt, bool multi) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC spec{filt.name, filt.spec};
    dlg->SetFileTypes(1, &spec);
    dlg->SetTitle(title);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (multi ? static_cast<DWORD>(FOS_ALLOWMULTISELECT) : 0u));
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out.emplace_back(p); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

inline std::wstring saveFileDialog(HWND owner, const wchar_t* title, const DialogFilter& filt, const wchar_t* defExt,
                                   const std::wstring& defName) {
    std::wstring out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC spec{filt.name, filt.spec};
    dlg->SetFileTypes(1, &spec);
    dlg->SetTitle(title);
    dlg->SetDefaultExtension(defExt);
    if (!defName.empty()) dlg->SetFileName(defName.c_str());
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

// Folder picker (one click → a whole Suno stem folder).
inline std::wstring pickFolderDialog(HWND owner, const wchar_t* title) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

// Every .wav directly inside a folder, sorted by name. Exported masters
// ("... HEM Master ...") are skipped so a re-load never pulls the mix back in.
inline std::vector<std::wstring> wavsInFolder(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::wstring ext = e.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext != L".wav") continue;
        std::wstring name = e.path().filename().wstring();
        std::wstring low = name;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low.find(L"hem master") != std::wstring::npos) continue;
        out.push_back(e.path().wstring());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- image decode
// Decodes an image held in memory (JPEG/PNG…) to RGBA8 with WIC.
inline bool decodeImage(const void* data, std::size_t size, std::vector<std::uint8_t>& rgba, int& w, int& h) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(f->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        cv->GetSize(&uw, &uh);
        w = static_cast<int>(uw); h = static_cast<int>(uh);
        rgba.resize(static_cast<std::size_t>(uw) * uh * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return ok;
}

inline bool loadResource(int id, const void*& data, std::size_t& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // RT_RCDATA
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = SizeofResource(nullptr, r);
    return data && size;
}

// ---------------------------------------------------------------- waveform overview
// Max |mid| per bin over the whole stem, for the arrange lanes.
inline std::vector<float> computePeaks(const wav::Audio& a, std::size_t bins) {
    std::vector<float> p(bins, 0.0f);
    const std::uint64_t n = a.frames();
    if (n == 0) return p;
    for (std::size_t b = 0; b < bins; ++b) {
        const std::uint64_t i0 = n * b / bins, i1 = std::max<std::uint64_t>(i0 + 1, n * (b + 1) / bins);
        float m = 0.0f;
        for (std::uint64_t i = i0; i < i1 && i < n; ++i) m = std::max(m, std::fabs(0.5f * (a.ch[0][i] + a.ch[1][i])));
        p[b] = m;
    }
    return p;
}

// ---------------------------------------------------------------- tempo estimate
// Onset-strength autocorrelation over the first 60 s of the summed stems.
// It is an ESTIMATE (used only for the bar ruler); the tempo box can be dragged.
inline double estimateTempo(const std::vector<const wav::Audio*>& stems, double sr) {
    if (stems.empty()) return 120.0;
    const std::size_t hop = static_cast<std::size_t>(sr / 100.0);   // 10 ms frames
    std::uint64_t len = 0;
    for (auto* a : stems) len = std::max(len, a->frames());
    len = std::min<std::uint64_t>(len, static_cast<std::uint64_t>(sr * 60.0));
    const std::size_t frames = static_cast<std::size_t>(len / hop);
    if (frames < 400) return 120.0;
    std::vector<double> e(frames, 0.0);
    for (auto* a : stems)
        for (std::size_t f = 0; f < frames; ++f) {
            double s = 0.0;
            for (std::size_t i = f * hop; i < (f + 1) * hop && i < a->frames(); ++i) {
                const double m = 0.5 * (a->ch[0][i] + a->ch[1][i]);
                s += m * m;
            }
            e[f] += s;
        }
    std::vector<double> o(frames, 0.0);
    for (std::size_t f = 1; f < frames; ++f) o[f] = std::max(0.0, std::log10(e[f] + 1e-12) - std::log10(e[f - 1] + 1e-12));
    double mean = 0;
    for (double v : o) mean += v;
    mean /= static_cast<double>(frames);
    for (double& v : o) v -= mean;
    double best = 0, bestBpm = 120;
    for (double bpm = 70.0; bpm <= 180.0; bpm += 0.25) {
        const double lag = 6000.0 / bpm;   // frames per beat at 100 fps
        double r = 0;
        for (int mult = 1; mult <= 4; ++mult) {   // beats and bars reinforce
            const double L = lag * mult;
            const std::size_t li = static_cast<std::size_t>(L);
            const double fr = L - static_cast<double>(li);
            for (std::size_t f = li + 1; f < frames; ++f) r += o[f] * ((1 - fr) * o[f - li] + fr * o[f - li - 1]);
        }
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 0.6, 2.0));
        r *= prior;
        if (r > best) { best = r; bestBpm = bpm; }
    }
    return std::round(bestBpm * 10.0) / 10.0;
}

// ---------------------------------------------------------------- session files
struct StripState { float faderDb = 0, pan = 0; bool mute = false, solo = false; };

struct Session {
    std::vector<std::wstring> paths;   // kNumStems entries ("" = empty)
    std::vector<ChainState> chains;    // per stem
    std::vector<StripState> strips;
    ChainState master;
    double bpm = 120, targetLufs = -14;
    bool normalize = true;
    int glueMode = 0, preset = 0;
    bool loopOn = false;
    std::uint64_t loopA = 0, loopB = 0;   // source frames; loopB == 0 = whole song
};

inline void writeChain(std::ostream& o, const char* tag, int slot, const ChainState& c) {
    for (std::size_t p = 0; p < c.size(); ++p) {
        o << tag << ' ' << slot << ' ' << p << ' ' << c[p].id << ' ' << (c[p].bypass ? 1 : 0) << ' ' << c[p].values.size();
        for (float v : c[p].values) o << ' ' << v;
        o << '\n';
    }
}

inline std::string saveSession(const std::wstring& file, const Session& s) {
    std::ofstream o(std::filesystem::path(file), std::ios::binary);
    if (!o) return "Can't write the session file.";
    o << "HYBRIDCORE_SESSION 1\n";
    o << "bpm " << s.bpm << "\ntarget " << s.targetLufs << "\nnormalize " << (s.normalize ? 1 : 0)
      << "\nglue " << s.glueMode << "\npreset " << s.preset << '\n';
    o << "loop " << (s.loopOn ? 1 : 0) << ' ' << s.loopA << ' ' << s.loopB << '\n';
    for (std::size_t i = 0; i < s.paths.size(); ++i) {
        if (!s.paths[i].empty()) o << "stem " << i << ' ' << narrow(s.paths[i]) << '\n';
        const auto& st = s.strips[i];
        o << "strip " << i << ' ' << st.faderDb << ' ' << st.pan << ' ' << (st.mute ? 1 : 0) << ' ' << (st.solo ? 1 : 0) << '\n';
        writeChain(o, "proc", static_cast<int>(i), s.chains[i]);
    }
    writeChain(o, "mproc", 0, s.master);
    return o ? std::string{} : "Write failed.";
}

inline std::string loadSession(const std::wstring& file, Session& s, std::size_t nStems) {
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    if (!in) return "Can't open the session file.";
    std::string line;
    if (!std::getline(in, line) || line.rfind("HYBRIDCORE_SESSION", 0) != 0) return "Not a Hybrid Core session file.";
    s = Session{};
    s.paths.assign(nStems, L"");
    s.chains.assign(nStems, {});
    s.strips.assign(nStems, {});
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "bpm") ls >> s.bpm;
        else if (tag == "target") ls >> s.targetLufs;
        else if (tag == "normalize") { int v = 1; ls >> v; s.normalize = v != 0; }
        else if (tag == "glue") ls >> s.glueMode;
        else if (tag == "preset") ls >> s.preset;
        else if (tag == "loop") { int on = 0; ls >> on >> s.loopA >> s.loopB; s.loopOn = on != 0; }
        else if (tag == "stem") {
            std::size_t i = 0; ls >> i;
            std::string rest; std::getline(ls, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            if (i < nStems) s.paths[i] = widen(rest);
        } else if (tag == "strip") {
            std::size_t i = 0; StripState st; int m = 0, so = 0;
            ls >> i >> st.faderDb >> st.pan >> m >> so;
            st.mute = m != 0; st.solo = so != 0;
            if (i < nStems) s.strips[i] = st;
        } else if (tag == "proc" || tag == "mproc") {
            std::size_t slot = 0, idx = 0, n = 0; ProcState ps; int by = 0;
            ls >> slot >> idx >> ps.id >> by >> n;
            ps.bypass = by != 0;
            ps.values.resize(std::min<std::size_t>(n, 64));
            for (auto& v : ps.values) ls >> v;
            ChainState& c = tag == "mproc" ? s.master : (slot < nStems ? s.chains[slot] : s.master);
            if (c.size() <= idx) c.resize(idx + 1);
            c[idx] = ps;
        }
    }
    return {};
}

} // namespace hc::studio
