// =============================================================================
//  io/wav.hpp — RIFF/WAVE reader/writer (no libsndfile). NON-RT only.
//  Reads: PCM 16/24/32-bit int, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE.
//  Writes: 24-bit PCM (TPDF-dithered) or 32-bit float.
//  Mono files are duplicated to stereo; >2 channels: first two are used.
// =============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace hc::wav {

struct Audio {
    double sampleRate = 0;
    std::vector<float> ch[2];
    std::uint64_t frames() const noexcept { return ch[0].size(); }
};

namespace detail {
inline std::uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
inline std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline void put32(std::ofstream& f, std::uint32_t v) { unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; f.write((char*)b, 4); }
inline void put16(std::ofstream& f, std::uint16_t v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; f.write((char*)b, 2); }
} // namespace detail

// Returns empty string on success, else an error message.
inline std::string read(const std::string& path, Audio& out) {
    using namespace detail;
    std::ifstream f(path, std::ios::binary);
    if (!f) return "cannot open " + path;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return path + ": not a RIFF/WAVE file";
    std::uint16_t fmt = 0, chans = 0, bits = 0; std::uint32_t sr = 0;
    const unsigned char* data = nullptr; std::size_t dataLen = 0;
    for (std::size_t p = 12; p + 8 <= d.size();) {
        const std::uint32_t len = u32(&d[p + 4]);
        const unsigned char* body = &d[p + 8];
        const std::size_t avail = d.size() - (p + 8);
        if (!std::memcmp(&d[p], "fmt ", 4) && len >= 16 && avail >= 16) {
            fmt = u16(body); chans = u16(body + 2); sr = u32(body + 4); bits = u16(body + 14);
            if (fmt == 0xFFFE && len >= 26 && avail >= 26) fmt = u16(body + 24);   // SubFormat GUID's first 2 bytes
        } else if (!std::memcmp(&d[p], "data", 4)) {
            data = body; dataLen = std::min<std::size_t>(len, avail);
        }
        p += 8 + len + (len & 1);
    }
    if (!data || !chans || !sr) return path + ": missing fmt/data chunk";
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64))))
        return path + ": unsupported format (fmt " + std::to_string(fmt) + ", " + std::to_string(bits) + "-bit)";
    const std::size_t bps = bits / 8, frameBytes = bps * chans, n = dataLen / frameBytes;
    out.sampleRate = sr;
    out.ch[0].resize(n); out.ch[1].resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::uint16_t c = 0; c < std::min<std::uint16_t>(chans, 2); ++c) {
            const unsigned char* s = data + i * frameBytes + c * bps;
            float v = 0;
            if (fmt == 3 && bits == 32) { std::memcpy(&v, s, 4); }
            else if (fmt == 3) { double dv; std::memcpy(&dv, s, 8); v = static_cast<float>(dv); }
            else if (bits == 16) v = static_cast<std::int16_t>(u16(s)) / 32768.0f;
            else if (bits == 24) { std::int32_t x = (s[0] << 8) | (s[1] << 16) | (std::int32_t(s[2]) << 24); v = static_cast<float>((x >> 8) / 8388608.0); }
            else v = static_cast<float>(static_cast<std::int32_t>(u32(s)) / 2147483648.0);
            out.ch[c][i] = v;
        }
        if (chans == 1) out.ch[1][i] = out.ch[0][i];
    }
    return {};
}

inline std::string write(const std::string& path, const Audio& a, bool float32 = false) {
    using namespace detail;
    std::ofstream f(path, std::ios::binary);
    if (!f) return "cannot create " + path;
    const std::uint16_t bits = float32 ? 32 : 24, chans = 2, bps = bits / 8;
    const std::uint64_t n = a.frames();
    const std::uint64_t dataLen = n * chans * bps;
    if (dataLen > 0xFFFFFFF0ull) return "file too large for RIFF (use RF64 — not implemented)";
    f.write("RIFF", 4); put32(f, static_cast<std::uint32_t>(36 + dataLen)); f.write("WAVE", 4);
    f.write("fmt ", 4); put32(f, 16); put16(f, float32 ? 3 : 1); put16(f, chans);
    put32(f, static_cast<std::uint32_t>(a.sampleRate)); put32(f, static_cast<std::uint32_t>(a.sampleRate) * chans * bps);
    put16(f, chans * bps); put16(f, bits);
    f.write("data", 4); put32(f, static_cast<std::uint32_t>(dataLen));
    std::mt19937 rng(0x48454D);   // deterministic TPDF dither
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<unsigned char> buf;
    buf.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(dataLen, 1u << 24)));
    for (std::uint64_t i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) {
            const float v = a.ch[c][i];
            if (float32) { unsigned char b[4]; std::memcpy(b, &v, 4); buf.insert(buf.end(), b, b + 4); }
            else {
                // TPDF dither at 1 LSB (sum of two uniform ±0.5 LSB) before quantising to 24-bit.
                double x = std::round(static_cast<double>(v) * 8388608.0 + u(rng) + u(rng));
                x = std::fmin(8388607.0, std::fmax(-8388608.0, x));
                const std::int32_t q = static_cast<std::int32_t>(x);
                buf.push_back(static_cast<unsigned char>(q)); buf.push_back(static_cast<unsigned char>(q >> 8)); buf.push_back(static_cast<unsigned char>(q >> 16));
            }
            if (buf.size() >= (1u << 24)) { f.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size())); buf.clear(); }
        }
    f.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return f ? std::string{} : "write failed: " + path;
}

} // namespace hc::wav
