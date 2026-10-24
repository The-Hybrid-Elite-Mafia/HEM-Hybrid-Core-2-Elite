// =============================================================================
//  app/offline_render.hpp — export engine shared by Hybrid Core Studio.
//
//  A RenderJob is a complete, self-contained snapshot of a mix: stem audio
//  pointers, every processor's parameter values (ChainState), faders, pans,
//  mute/solo, the master chain and the loudness target. Rendering builds a
//  fresh StemEngine from that snapshot, so an export never touches the live
//  playback engine and you can keep listening while it runs.
//
//  Loudness normalisation (when enabled) is measured, not guessed:
//    pass 1  render with trim 0, limiter bypassed  → raw integrated LUFS
//    pass 2  render with trim = target − raw, limiter on → what the limiter took
//    pass 3  render with corrected trim → final audio, measured again
// =============================================================================
#pragma once
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include "hc/io/wav.hpp"
#include "hc/meter/loudness.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hc {

struct ProcState {
    std::string id;
    bool bypass = false;
    std::vector<float> values;   // index = parameter index
};
using ChainState = std::vector<ProcState>;

inline ProcState captureProc(IProcessor& p) {
    ProcState s;
    s.id = std::string(p.info().id);
    s.bypass = p.bypassed();
    s.values.resize(p.numParams());
    for (std::size_t i = 0; i < p.numParams(); ++i) s.values[i] = p.param(i).target();
    return s;
}

// Creates the processor and applies the stored values. nullptr if the id is unknown.
inline std::unique_ptr<IProcessor> restoreProc(const ProcessorRegistry& reg, const ProcState& s) {
    auto p = reg.create(s.id);
    if (!p) return nullptr;
    const std::size_t n = std::min(p->numParams(), s.values.size());
    for (std::size_t i = 0; i < n; ++i) p->param(i).set(s.values[i]);
    p->setBypassed(s.bypass);
    return p;
}

struct StemJob {
    const float* L = nullptr;
    const float* R = nullptr;
    std::uint64_t frames = 0;
    ChainState chain;
    float faderDb = 0.0f, pan = 0.0f;
    bool mute = false, solo = false;
};

struct RenderJob {
    double sampleRate = 48000.0;
    std::vector<StemJob> stems;      // ≤ kNumStems
    ChainState master;
    bool normalize = true;
    double targetLufs = -14.0;
};

struct RenderResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    LoudnessReading reading;
    double trimDb = 0.0;
    wav::Audio audio;
};

namespace detail {

// One full pass. progress(fraction) returns false to cancel.
inline bool renderPass(const RenderJob& job, std::uint64_t len, double trimDb, bool limiterOn, wav::Audio* out,
                       LoudnessReading& reading, const std::function<bool(double)>& progress) {
    auto reg = makeBuiltinRegistry();
    auto eng = std::make_unique<StemEngine>();
    const std::uint32_t blk = 512;
    for (std::size_t s = 0; s < job.stems.size(); ++s) {
        auto& st = eng->stem(s);
        st.faderDb.set(job.stems[s].faderDb);
        st.pan.set(job.stems[s].pan);
        st.mute.store(job.stems[s].mute);
        st.solo.store(job.stems[s].solo);
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    eng->prepare(job.sampleRate, blk, std::min<unsigned>(hw > 1 ? hw - 1 : 0, 12));
    for (std::size_t s = 0; s < job.stems.size(); ++s) {
        const auto& sj = job.stems[s];
        eng->setSource(s, sj.L, sj.R, sj.frames);
        auto ch = std::make_unique<ProcessorChain>();
        for (const auto& ps : sj.chain) if (auto p = restoreProc(reg, ps)) ch->add(std::move(p));
        eng->publishChain(s, std::move(ch));
    }
    auto mc = std::make_unique<ProcessorChain>();
    auto trim = reg.create("hc.eq6");
    trim->param(18).configure(static_cast<float>(trimDb), -60.0f, 60.0f);
    mc->add(std::move(trim));
    for (const auto& ps : job.master) {
        auto p = restoreProc(reg, ps);
        if (!p) continue;
        if (ps.id == "hc.limiter" && !limiterOn) p->setBypassed(true);
        mc->add(std::move(p));
    }
    eng->publishChain(StemEngine::kMaster, std::move(mc));
    eng->play();

    std::vector<float> L(blk), R(blk);
    if (out) { out->sampleRate = job.sampleRate; out->ch[0].assign(len, 0.0f); out->ch[1].assign(len, 0.0f); }
    LoudnessMeter meter;
    meter.prepare(job.sampleRate);
    std::uint64_t produced = 0, emitted = 0;
    std::uint32_t lat = 0;
    bool latKnown = false, alive = true;
    std::uint32_t tick = 0;
    while (emitted < len) {
        eng->process(L.data(), R.data(), blk);
        if (!latKnown) { lat = eng->totalLatency(); latKnown = true; }
        std::uint32_t skip = 0;
        if (produced < lat) skip = static_cast<std::uint32_t>(std::min<std::uint64_t>(blk, lat - produced));
        produced += blk;
        const auto keep = static_cast<std::uint32_t>(std::min<std::uint64_t>(blk - skip, len - emitted));
        if (keep) {
            AudioBlock b{{L.data() + skip, R.data() + skip}, 2, keep};
            meter.process(b);
            if (out) {
                std::memcpy(out->ch[0].data() + emitted, b.ch[0], keep * sizeof(float));
                std::memcpy(out->ch[1].data() + emitted, b.ch[1], keep * sizeof(float));
            }
            emitted += keep;
        }
        if (++tick % 64 == 0) {
            eng->collectGarbage();
            if (progress && !progress(static_cast<double>(emitted) / static_cast<double>(len))) { alive = false; break; }
        }
    }
    reading = meter.reading();
    eng->shutdown();
    return alive;
}

} // namespace detail

// Runs on a worker thread. progress(0..1 overall) returning false cancels.
inline RenderResult renderMix(const RenderJob& job, const std::function<bool(double)>& progress = {}) {
    RenderResult res;
    if (job.stems.empty()) { res.error = "No stems loaded."; return res; }
    std::uint64_t len = 0;
    for (const auto& s : job.stems) len = std::max(len, s.frames);
    if (len == 0) { res.error = "Stems are empty."; return res; }

    const int passes = job.normalize ? 3 : 1;
    int pass = 0;
    auto sub = [&](double f) { return !progress || progress((pass + f) / passes); };
    double trim = 0.0;
    if (job.normalize) {
        LoudnessReading r1;
        if (!detail::renderPass(job, len, 0.0, false, nullptr, r1, sub)) { res.cancelled = true; return res; }
        if (!std::isfinite(r1.integrated)) { res.error = "The mix is silent (below the -70 LUFS gate), so it can't be normalised."; return res; }
        trim = job.targetLufs - r1.integrated;
        ++pass;
        LoudnessReading r2;
        if (!detail::renderPass(job, len, trim, true, nullptr, r2, sub)) { res.cancelled = true; return res; }
        if (std::isfinite(r2.integrated)) trim += job.targetLufs - r2.integrated;
        ++pass;
    }
    if (!detail::renderPass(job, len, trim, true, &res.audio, res.reading, sub)) { res.cancelled = true; return res; }
    res.trimDb = trim;
    res.ok = true;
    if (progress) progress(1.0);
    return res;
}

} // namespace hc
