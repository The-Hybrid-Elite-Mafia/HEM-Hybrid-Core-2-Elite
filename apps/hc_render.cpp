// =============================================================================
//  apps/hc_render.cpp — offline 13-stem repair + master renderer (any OS).
//
//  hc_render [options] OUT.wav STEM1.wav [STEM2.wav ... STEM13.wav]
//    --target  <LUFS>   integrated loudness target       (default -14)
//    --ceiling <dBTP>   true-peak ceiling                (default -1.0)
//    --shimmer <0..1>   HF shimmer smoothing amount      (default 0.6)
//    --deess   <0..1>   spectral de-esser amount         (default 0.5)
//                       applied to VOCAL stems only (file name contains
//                       voc/vox/voice/lead/adlib/choir/sing/bv) — a de-esser
//                       on a noise-based snare or hats dulls the drums.
//    --deess-all        apply the de-esser to every stem
//    --phase   <0..1>   HF stereo phase repair amount    (default 0.3)
//    --no-repair        bypass the Artifact Destroyer on all stems
//    --float            write 32-bit float (default 24-bit TPDF-dithered)
//
//  Pipeline: each stem → Artifact Destroyer → sum (PDC-aligned)
//            → master trim → True-Peak Limiter → OUT.
//  Loudness is hit by measurement, not guesswork: pass 1 measures the
//  un-limited mix, pass 2 measures what the limiter did, pass 3 renders with
//  the corrected trim and verifies.
// =============================================================================
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include "hc/io/wav.hpp"
#include "hc/platform/thread_priority.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace hc;

namespace {

struct Options {
    double target = -14.0, ceiling = -1.0, shimmer = 0.6, deess = 0.5, phase = 0.3;
    bool repair = true, float32 = false, deessAll = false;
    std::string out;
    std::vector<std::string> stems;
};

int usage() {
    std::fprintf(stderr,
        "usage: hc_render [--target LUFS] [--ceiling dBTP] [--shimmer 0..1] [--deess 0..1]\n"
        "                 [--phase 0..1] [--no-repair] [--float] OUT.wav STEM1.wav [... STEM13.wav]\n");
    return 2;
}

bool isVocal(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (const auto sl = name.find_last_of("/\\"); sl != std::string::npos) name = name.substr(sl + 1);
    for (const char* k : {"voc", "vox", "voice", "lead", "adlib", "choir", "sing", "bv"})
        if (name.find(k) != std::string::npos) return true;
    return false;
}

struct StemStats { double jitter = 0, cut = 0, coh = 0, live = 0; std::uint64_t n = 0; std::uint32_t sibFrames = 0, frames = 0, transients = 0; };

struct PassResult { LoudnessReading r; std::vector<StemStats> stats; };

PassResult renderPass(const Options& o, const std::vector<wav::Audio>& stems, double sr, std::uint64_t len,
                      double trimDb, bool limit, wav::Audio* out) {
    auto eng = std::make_unique<StemEngine>();
    const std::uint32_t blk = 512;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    eng->prepare(sr, blk, std::min<unsigned>(hw > 1 ? hw - 1 : 0, 12), &platform::initRealtimeWorker);
    auto reg = makeBuiltinRegistry();
    std::vector<ArtifactProcessor*> art(stems.size(), nullptr);
    for (std::size_t s = 0; s < stems.size(); ++s) {
        eng->setSource(s, stems[s].ch[0].data(), stems[s].ch[1].data(), stems[s].frames());
        auto ch = std::make_unique<ProcessorChain>();
        auto ap = std::make_unique<ArtifactProcessor>();
        ap->param(ArtifactProcessor::kShimmer).set(float(o.shimmer));
        ap->param(ArtifactProcessor::kSib).set(o.deessAll || isVocal(o.stems[s]) ? float(o.deess) : 0.0f);
        ap->param(ArtifactProcessor::kPhase).set(float(o.phase));
        ap->setBypassed(!o.repair);
        art[s] = ap.get();
        ch->add(std::move(ap));
        eng->publishChain(s, std::move(ch));
    }
    auto mc = std::make_unique<ProcessorChain>();
    auto trim = reg.create("hc.eq6");
    trim->param(18).configure(float(trimDb), -60.0f, 60.0f);   // output gain = loudness trim
    mc->add(std::move(trim));
    auto lim = reg.create("hc.limiter");
    lim->param(LimiterProcessor::kCeiling).set(float(o.ceiling));
    lim->setBypassed(!limit);
    mc->add(std::move(lim));
    eng->publishChain(StemEngine::kMaster, std::move(mc));
    eng->play();

    // Prime one block so pending chains swap in and PDC/latency are known.
    std::vector<float> L(blk), R(blk);
    const std::uint64_t total = len;
    PassResult pr; pr.stats.resize(stems.size());
    std::uint64_t produced = 0, emitted = 0;
    std::uint32_t lat = 0;
    bool latKnown = false;
    if (out) { out->sampleRate = sr; out->ch[0].assign(total, 0.0f); out->ch[1].assign(total, 0.0f); }
    // Measure on the latency-trimmed output with a separate meter so the
    // pre-roll silence doesn't enter the gating.
    LoudnessMeter meter; meter.prepare(sr);
    while (emitted < total) {
        eng->process(L.data(), R.data(), blk);
        if (!latKnown) { lat = eng->totalLatency(); latKnown = true; }
        // Drop the first `lat` frames (pipeline latency), keep the rest.
        std::uint32_t skip = 0;
        if (produced < lat) skip = static_cast<std::uint32_t>(std::min<std::uint64_t>(blk, lat - produced));
        produced += blk;
        const std::uint32_t keep = static_cast<std::uint32_t>(std::min<std::uint64_t>(blk - skip, total - emitted));
        if (keep) {
            AudioBlock b{{L.data() + skip, R.data() + skip}, 2, keep};
            meter.process(b);
            if (out) { std::memcpy(out->ch[0].data() + emitted, b.ch[0], keep * 4); std::memcpy(out->ch[1].data() + emitted, b.ch[1], keep * 4); }
            emitted += keep;
        }
        for (std::size_t s = 0; s < art.size(); ++s) {
            const auto& t = art[s]->telemetry();
            auto& st = pr.stats[s];
            st.jitter += t.hfJitterDb.load(); st.cut += t.shimmerCutDb.load(); st.coh += t.hfCoherence.load();
            st.live += t.hfLiveFraction.load(); ++st.n;
            st.sibFrames = t.sibilantFrames.load(); st.frames = t.frames.load(); st.transients = t.transientFrames.load();
        }
        eng->collectGarbage();
    }
    pr.r = meter.reading();
    eng->shutdown();
    return pr;
}

} // namespace

int main(int argc, char** argv) {
    simd::ScopedFlushDenormals ftz;
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&](double& v) { if (i + 1 >= argc) return false; v = std::atof(argv[++i]); return true; };
        if (a == "--target") { if (!num(o.target)) return usage(); }
        else if (a == "--ceiling") { if (!num(o.ceiling)) return usage(); }
        else if (a == "--shimmer") { if (!num(o.shimmer)) return usage(); }
        else if (a == "--deess") { if (!num(o.deess)) return usage(); }
        else if (a == "--phase") { if (!num(o.phase)) return usage(); }
        else if (a == "--no-repair") o.repair = false;
        else if (a == "--deess-all") o.deessAll = true;
        else if (a == "--float") o.float32 = true;
        else if (a.rfind("--", 0) == 0) return usage();
        else if (o.out.empty()) o.out = a;
        else o.stems.push_back(a);
    }
    if (o.out.empty() || o.stems.empty()) return usage();
    if (o.stems.size() > kNumStems) { std::fprintf(stderr, "error: at most %zu stems\n", kNumStems); return 2; }

    std::vector<wav::Audio> stems(o.stems.size());
    double sr = 0; std::uint64_t len = 0;
    for (std::size_t s = 0; s < o.stems.size(); ++s) {
        if (auto e = wav::read(o.stems[s], stems[s]); !e.empty()) { std::fprintf(stderr, "error: %s\n", e.c_str()); return 1; }
        if (sr == 0) sr = stems[s].sampleRate;
        if (stems[s].sampleRate != sr) {
            std::fprintf(stderr, "error: %s is %.0f Hz but stem 1 is %.0f Hz — all stems must share one rate "
                                 "(sample-rate conversion is not built into this version)\n", o.stems[s].c_str(), stems[s].sampleRate, sr);
            return 1;
        }
        len = std::max<std::uint64_t>(len, stems[s].frames());
    }
    std::printf("HYBRID CORE 2.0 :: hc_render — %zu stem(s), %.0f Hz, %.2f s\n", stems.size(), sr, double(len) / sr);
    const auto t0 = std::chrono::steady_clock::now();

    // Pass 1: measure the repaired, un-limited mix.
    PassResult p1 = renderPass(o, stems, sr, len, 0.0, false, nullptr);
    if (!std::isfinite(p1.r.integrated)) { std::fprintf(stderr, "error: mix is silent (below -70 LUFS gate)\n"); return 1; }
    double trim = o.target - p1.r.integrated;
    std::printf("  pass 1  raw mix: I = %6.2f LUFS, TP = %6.2f dBTP  → trim %+6.2f dB\n", p1.r.integrated, p1.r.truePeakDb, trim);
    // Pass 2: limiter engaged — measure how much loudness it removed.
    PassResult p2 = renderPass(o, stems, sr, len, trim, true, nullptr);
    trim += o.target - p2.r.integrated;
    std::printf("  pass 2  limited: I = %6.2f LUFS  → corrected trim %+6.2f dB\n", p2.r.integrated, trim);
    // Pass 3: final.
    wav::Audio out;
    PassResult p3 = renderPass(o, stems, sr, len, trim, true, &out);
    if (auto e = wav::write(o.out, out, o.float32); !e.empty()) { std::fprintf(stderr, "error: %s\n", e.c_str()); return 1; }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto& r = p3.r;
    std::printf("\n  FINAL MASTER  → %s (%s)\n", o.out.c_str(), o.float32 ? "32-bit float" : "24-bit TPDF");
    std::printf("    Integrated   %7.2f LUFS   (target %.1f)\n", r.integrated, o.target);
    std::printf("    True peak    %7.2f dBTP   (ceiling %.1f)\n", r.truePeakDb, o.ceiling);
    std::printf("    Sample peak  %7.2f dBFS\n", r.samplePeakDb);
    std::printf("    Max short-t  %7.2f LUFS   Max momentary %.2f LUFS\n", r.maxShortTerm, r.maxMomentary);
    std::printf("    LRA          %7.2f LU     PLR %.2f dB\n", r.lra, r.plr);
    if (o.repair) {
        std::printf("\n  ARTIFACT DESTROYER (per stem, averaged over the song)\n");
        std::printf("    %-4s %-26s %7s %9s %9s %8s %7s %7s\n", "stem", "file", "HF live", "jitter dB", "HF dE dB", "HF coh", "ess %", "hits %");
        for (std::size_t s = 0; s < p3.stats.size(); ++s) {
            const auto& st = p3.stats[s];
            const double n = double(std::max<std::uint64_t>(1, st.n)), fr = double(std::max<std::uint32_t>(1, st.frames));
            std::string name = o.stems[s];
            if (const auto sl = name.find_last_of("/\\"); sl != std::string::npos) name = name.substr(sl + 1);
            if (name.size() > 26) name = name.substr(0, 23) + "...";
            std::printf("    %-4zu %-26s %6.0f%% %9.2f %9.2f %8.3f %6.1f%% %6.1f%%%s\n", s + 1, name.c_str(), 100.0 * st.live / n,
                        st.jitter / n, st.cut / n, st.coh / n, 100.0 * st.sibFrames / fr, 100.0 * st.transients / fr,
                        isVocal(o.stems[s]) || o.deessAll ? "  [de-ess on]" : "");
        }
    }
    std::printf("\n  3 passes in %.2f s (%.1fx real-time)\n", secs, 3.0 * double(len) / sr / secs);
    return 0;
}
