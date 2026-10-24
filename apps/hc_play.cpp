// =============================================================================
//  apps/hc_play.cpp — real-time 13-stem playback through WASAPI (Windows).
//
//  hc_play [--exclusive] STEM1.wav [... STEM13.wav]
//
//  Threads:
//    WASAPI render thread (MMCSS Pro Audio)  → StemEngine::process (conductor)
//    N DSP workers (MMCSS Pro Audio)         → the 13 stem chains
//    Analysis thread (normal priority)       → spectrum FFTs, garbage collection
//    Main thread                             → console meters at 10 Hz
//  (The GPU Graphics Engine replaces the console in phase 2 and reads the
//   exact same TripleBuffer snapshots / spectrum frames.)
// =============================================================================
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include "hc/io/wav.hpp"
#include "hc/meter/spectrum.hpp"
#include "hc/platform/thread_priority.hpp"
#if defined(_WIN32)
#include "hc/platform/wasapi_output.hpp"
#include <conio.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace hc;

#if !defined(_WIN32)
int main() { std::fprintf(stderr, "hc_play needs Windows (WASAPI). Use hc_render on this OS.\n"); return 1; }
#else

static void render(void* user, float* L, float* R, std::uint32_t n) noexcept {
    static_cast<StemEngine*>(user)->process(L, R, n);
}

// HF roll-off: highest frequency whose averaged level is within 60 dB of the
// spectrum's maximum — a quick read on how much real top end a stem has.
static float rolloffHz(const SpectrumAnalyzer& a) {
    const float* db = a.averageDb();
    float mx = -200;
    for (std::size_t k = 1; k < SpectrumAnalyzer::kBins; ++k) mx = std::max(mx, db[k]);
    for (std::size_t k = SpectrumAnalyzer::kBins - 1; k > 0; --k)
        if (db[k] > mx - 60.0f) return float(k) * float(a.sampleRate() / SpectrumAnalyzer::kFft);
    return 0;
}

int main(int argc, char** argv) {
    bool exclusive = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--exclusive") exclusive = true; else files.push_back(a);
    }
    if (files.empty() || files.size() > kNumStems) {
        std::fprintf(stderr, "usage: hc_play [--exclusive] STEM1.wav [... STEM13.wav]\n");
        return 2;
    }
    std::vector<wav::Audio> stems(files.size());
    double sr = 0;
    for (std::size_t s = 0; s < files.size(); ++s) {
        if (auto e = wav::read(files[s], stems[s]); !e.empty()) { std::fprintf(stderr, "error: %s\n", e.c_str()); return 1; }
        if (sr == 0) sr = stems[s].sampleRate;
        if (stems[s].sampleRate != sr) { std::fprintf(stderr, "error: all stems must share one sample rate\n"); return 1; }
    }

    WasapiOutput dev;
    WasapiOutput::Config cfg;
    cfg.mode = exclusive ? WasapiOutput::Mode::Exclusive : WasapiOutput::Mode::SharedLowLatency;
    cfg.sampleRate = sr;
    cfg.maxBlock = 1024;
    if (auto e = dev.open(cfg); !e.empty()) { std::fprintf(stderr, "audio device: %s\n", e.c_str()); return 1; }
    std::printf("%s\n", dev.info().description.c_str());

    auto eng = std::make_unique<StemEngine>();
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    eng->prepare(sr, cfg.maxBlock, std::min(hw - 2, 12u), &platform::initRealtimeWorker);
    auto reg = makeBuiltinRegistry();
    for (std::size_t s = 0; s < stems.size(); ++s) {
        eng->setSource(s, stems[s].ch[0].data(), stems[s].ch[1].data(), stems[s].frames());
        auto ch = std::make_unique<ProcessorChain>();
        ch->add(reg.create("hc.artifact"));
        ch->add(reg.create("hc.eq6"));
        ch->add(reg.create("hc.comp"));
        eng->publishChain(s, std::move(ch));
    }
    auto mc = std::make_unique<ProcessorChain>();
    mc->add(reg.create("hc.eq6"));
    mc->add(reg.create("hc.limiter"));
    eng->publishChain(StemEngine::kMaster, std::move(mc));

    // Analysis thread: spectrum + garbage collection, never touches audio.
    std::atomic<bool> run{true};
    std::vector<SpectrumAnalyzer> spec(stems.size() + 1);
    for (auto& a : spec) a.prepare(sr);
    std::thread analysis([&] {
        while (run.load()) {
            for (std::size_t s = 0; s < stems.size(); ++s) spec[s].drain(eng->stem(s).tap);
            spec.back().drain(eng->masterTap());
            eng->collectGarbage();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    });

    eng->play();
    if (auto e = dev.start(&render, eng.get()); !e.empty()) { std::fprintf(stderr, "start: %s\n", e.c_str()); run = false; analysis.join(); return 1; }
    std::printf("Playing %zu stems. Latency (plugins) %u samples. Press any key to stop.\n", stems.size(), eng->totalLatency());

    while (!_kbhit() && eng->playhead() < eng->length() + eng->totalLatency()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto& tb = eng->masterSnapshot();
        tb.update();
        const auto& m = tb.read_buffer();
        std::printf("\r%6.1fs  M %6.1f  S %6.1f  I %6.1f LUFS  TP %6.1f dBTP  LRA %4.1f  HF-rolloff %5.0f Hz  glitches %llu   ",
                    double(eng->playhead()) / sr, m.loudness.momentary, m.loudness.shortTerm, m.loudness.integrated,
                    m.loudness.truePeakDb, m.loudness.lra, rolloffHz(spec.back()),
                    static_cast<unsigned long long>(dev.glitchCount()));
        std::fflush(stdout);
    }
    dev.stop();
    run = false;
    analysis.join();
    eng->shutdown();
    std::printf("\nStopped.\n");
    return 0;
}
#endif
