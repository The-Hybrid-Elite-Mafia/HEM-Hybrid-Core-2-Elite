// =============================================================================
//  tests/test_main.cpp — numeric verification of every engine. No test
//  framework (zero dependencies): each check prints PASS/FAIL with the
//  measured value; the process exits non-zero on any failure.
// =============================================================================
#include "hc/dsp/fft.hpp"
#include "hc/dsp/stft.hpp"
#include "hc/dsp/dynamics.hpp"
#include "hc/meter/loudness.hpp"
#include "hc/meter/spectrum.hpp"
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include <chrono>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace hc;
static int g_fail = 0;
static void check(bool ok, const char* name, double value, const char* unit = "") {
    std::printf("[%s] %-58s %12.6g %s\n", ok ? "PASS" : "FAIL", name, value, unit);
    if (!ok) ++g_fail;
}
static constexpr double kPi = std::numbers::pi;

// ----------------------------------------------------------------------------
static void testComplexFFT() {
    for (std::size_t n : {8u, 64u, 1024u, 4096u}) {
        ComplexFFT fft(n);
        std::mt19937 rng(1); std::normal_distribution<float> g;
        std::vector<float> re(n), im(n), r0(n), i0(n);
        for (std::size_t i = 0; i < n; ++i) { re[i] = r0[i] = g(rng); im[i] = i0[i] = g(rng); }
        fft.forward(re.data(), im.data());
        double maxErr = 0, maxMag = 0;
        for (std::size_t k = 0; k < n; ++k) {
            std::complex<double> s = 0;
            for (std::size_t t = 0; t < n; ++t) s += std::complex<double>(r0[t], i0[t]) * std::polar(1.0, -2 * kPi * double(k * t % n) / double(n));
            maxErr = std::max(maxErr, std::abs(s - std::complex<double>(re[k], im[k])));
            maxMag = std::max(maxMag, std::abs(s));
        }
        char name[96]; std::snprintf(name, sizeof name, "ComplexFFT n=%zu vs naive DFT (rel. error)", n);
        check(maxErr / maxMag < 1e-5, name, maxErr / maxMag);
        fft.inverse(re.data(), im.data());
        double rt = 0; for (std::size_t i = 0; i < n; ++i) rt = std::max(rt, double(std::fabs(re[i] - r0[i]) + std::fabs(im[i] - i0[i])));
        std::snprintf(name, sizeof name, "ComplexFFT n=%zu round trip (max abs error)", n);
        check(rt < 1e-4, name, rt);
    }
}

static void testRealFFT() {
    for (std::size_t n : {16u, 2048u, 8192u}) {
        RealFFT fft(n);
        std::mt19937 rng(2); std::normal_distribution<float> g;
        std::vector<float> x(n), re(n / 2 + 1), im(n / 2 + 1), y(n);
        for (auto& v : x) v = g(rng);
        fft.forward(x.data(), re.data(), im.data());
        double maxErr = 0, maxMag = 0;
        for (std::size_t k = 0; k <= n / 2; k += (n > 64 ? 37 : 1)) {
            std::complex<double> s = 0;
            for (std::size_t t = 0; t < n; ++t) s += double(x[t]) * std::polar(1.0, -2 * kPi * double(k * t % n) / double(n));
            maxErr = std::max(maxErr, std::abs(s - std::complex<double>(re[k], im[k])));
            maxMag = std::max(maxMag, std::abs(s));
        }
        char name[96]; std::snprintf(name, sizeof name, "RealFFT n=%zu vs naive DFT (rel. error)", n);
        check(maxErr / maxMag < 1e-5, name, maxErr / maxMag);
        fft.inverse(re.data(), im.data(), y.data());
        double rt = 0; for (std::size_t i = 0; i < n; ++i) rt = std::max(rt, double(std::fabs(y[i] - x[i])));
        std::snprintf(name, sizeof name, "RealFFT n=%zu round trip (max abs error)", n);
        check(rt < 1e-4, name, rt);
    }
}

struct Identity final : ISpectralKernel { void processFrame(SpectralFrame&) noexcept override {} };

static void testStftIdentity() {
    StftEngine st; st.prepare(48000, 2048, 4);
    const std::size_t total = 48000;
    std::vector<float> L(total), R(total), oL(total), oR(total);
    std::mt19937 rng(3); std::normal_distribution<float> g(0, 0.3f);
    for (std::size_t i = 0; i < total; ++i) { L[i] = g(rng); R[i] = g(rng); }
    oL = L; oR = R;
    Identity id;
    std::size_t pos = 0; const std::size_t blocks[] = {64, 100, 512, 1, 333, 2048};
    for (int bi = 0; pos < total; ++bi) {
        std::size_t n = std::min(blocks[bi % 6], total - pos);
        AudioBlock b{{oL.data() + pos, oR.data() + pos}, 2, (std::uint32_t)n};
        st.process(b, id); pos += n;
    }
    const std::size_t lat = st.latency();
    double err = 0, sig = 0;
    for (std::size_t i = lat; i < total; ++i) { err += std::pow(oL[i] - L[i - lat], 2) + std::pow(oR[i] - R[i - lat], 2); sig += L[i - lat] * L[i - lat] + R[i - lat] * R[i - lat]; }
    check(10 * std::log10(err / sig) < -100, "STFT/OLA identity, odd block sizes (error, dB)", 10 * std::log10(err / sig), "dB");
    check(lat == 2048, "STFT latency == N", double(lat), "samples");
}

static void testKWeighting() {
    BiquadCoeffs pre, rlb; kWeightingCoeffs(48000, pre, rlb);
    // BS.1770-4 table values at 48 kHz.
    const double e = std::max({std::fabs(pre.b0 - 1.53512485958697), std::fabs(pre.b1 + 2.69169618940638), std::fabs(pre.b2 - 1.19839281085285),
                               std::fabs(pre.a1 + 1.69065929318241), std::fabs(pre.a2 - 0.73248077421585),
                               std::fabs(rlb.a1 + 1.99004745483398), std::fabs(rlb.a2 - 0.99007225036621)});
    check(e < 1e-8, "K-weighting coeffs @48k vs BS.1770 table (max abs diff)", e);
}

static LoudnessReading measure(std::vector<float>& L, std::vector<float>& R, double sr) {
    LoudnessMeter m; m.prepare(sr);
    for (std::size_t p = 0; p < L.size(); p += 1024) {
        std::uint32_t n = (std::uint32_t)std::min<std::size_t>(1024, L.size() - p);
        AudioBlock b{{L.data() + p, R.data() + p}, 2, n};
        m.process(b);
    }
    return m.reading();
}
static void sine(std::vector<float>& v, double sr, double f, double dbfs, double phase = 0, std::size_t offset = 0, std::size_t len = SIZE_MAX) {
    const double a = std::pow(10.0, dbfs / 20.0);
    for (std::size_t i = offset; i < std::min(v.size(), offset + len); ++i) v[i] = float(a * std::sin(2 * kPi * f * double(i) / sr + phase));
}

static void testLoudness() {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        std::vector<float> L(std::size_t(sr * 20)), R;
        sine(L, sr, 1000, -23); R = L;
        const auto r = measure(L, R, sr);
        char name[96]; std::snprintf(name, sizeof name, "1 kHz stereo sine @ -23 dBFS, %g Hz → I = -23.0 LUFS", sr);
        check(std::fabs(r.integrated + 23.0) < 0.1, name, r.integrated, "LUFS");
        std::snprintf(name, sizeof name, "  same: M and S = -23.0 LUFS (%g Hz)", sr);
        check(std::fabs(r.momentary + 23.0) < 0.1 && std::fabs(r.shortTerm + 23.0) < 0.1, name, r.shortTerm, "LUFS");
    }
    {   // Relative gate: 10 s @ -36, 60 s @ -23, 10 s @ -36 (dBFS sine, stereo).
        // Blocks at -36 are 13 LU below the ungated mean → gated out → I ≈ -23.
        const double sr = 48000; std::vector<float> L(std::size_t(sr * 80)), R;
        sine(L, sr, 1000, -36, 0, 0, std::size_t(sr * 10));
        sine(L, sr, 1000, -23, 0, std::size_t(sr * 10), std::size_t(sr * 60));
        sine(L, sr, 1000, -36, 0, std::size_t(sr * 70), std::size_t(sr * 10));
        R = L;
        const auto r = measure(L, R, sr);
        check(std::fabs(r.integrated + 23.0) < 0.1, "Relative gate: -36/-23/-36 dBFS programme → I = -23.0", r.integrated, "LUFS");
    }
    {   // LRA: 20 s @ -20 dBFS then 20 s @ -30 → LRA ≈ 10 LU (EBU 3342 case-1 style).
        const double sr = 48000; std::vector<float> L(std::size_t(sr * 40)), R;
        sine(L, sr, 1000, -20, 0, 0, std::size_t(sr * 20));
        sine(L, sr, 1000, -30, 0, std::size_t(sr * 20), std::size_t(sr * 20));
        R = L;
        const auto r = measure(L, R, sr);
        check(std::fabs(r.lra - 10.0) < 0.2, "LRA: 20 s @ -20 dBFS + 20 s @ -30 dBFS → 10 LU", r.lra, "LU");
    }
}

static void testTruePeak() {
    // fs/4 sine with 45° phase: every sample lands at ±0.7071·A, the real peak is A.
    const double sr = 48000; std::vector<float> L(48000), R;
    sine(L, sr, 12000, 0.0, kPi / 4);
    // 10 ms raised-cosine fade-in: a hard-started sine has a REAL inter-sample
    // overshoot at its edge (the meter correctly reports it); we want steady state.
    for (std::size_t i = 0; i < 480; ++i) L[i] *= float(0.5 - 0.5 * std::cos(kPi * double(i) / 480.0));
    R = L;
    const auto r = measure(L, R, sr);
    check(std::fabs(r.samplePeakDb + 3.01) < 0.05, "fs/4 sine @45°: sample peak = -3.01 dBFS", r.samplePeakDb, "dBFS");
    check(std::fabs(r.truePeakDb) < 0.1, "fs/4 sine @45°: true peak = 0.0 dBTP", r.truePeakDb, "dBTP");
    // 997 Hz full-scale sine: TP must not exceed 0 dBTP by more than filter ripple.
    sine(L, sr, 997, 0.0); R = L;
    const auto r2 = measure(L, R, sr);
    check(std::fabs(r2.truePeakDb) < 0.05, "997 Hz 0 dBFS sine: true peak ≈ 0.0 dBTP", r2.truePeakDb, "dBTP");
    // Sweep: full-scale sines 20 Hz – 20 kHz, worst-case |TP error|.
    double worst = 0, worstF = 0;
    for (double f = 20; f <= 20000; f *= 1.12) {
        std::vector<float> A(24000), B;
        sine(A, sr, f, 0.0, 0.3); B = A;
        TruePeakOversampler tp; tp.prepare(sr);
        float m = 0; for (std::size_t i = 0; i < A.size(); ++i) { float v = tp.pushFrame(A[i], B[i]); if (i > 200) m = std::max(m, v); }
        const double e = std::fabs(20 * std::log10(m));
        if (e > worst) { worst = e; worstF = f; }
    }
    std::printf("        worst-case sweep error at %.0f Hz\n", worstF);
    check(worst < 0.25, "True-peak sweep 20 Hz–20 kHz @0 dBFS: worst |error|", worst, "dB");
}

static void testLimiter() {
    const double sr = 48000; const std::size_t n = std::size_t(sr * 10);
    std::vector<float> L(n), R(n);
    std::mt19937 rng(4); std::normal_distribution<float> g(0, 0.5f);   // brutally hot noise + transients
    for (std::size_t i = 0; i < n; ++i) { L[i] = g(rng) * (i % 9600 < 200 ? 4.0f : 1.0f); R[i] = g(rng); }
    LookaheadLimiter lim; lim.prepare(sr, 1.5f); lim.setCeilingDb(-1.0f); lim.setRelease(60);
    for (std::size_t p = 0; p < n; p += 512) {
        AudioBlock b{{L.data() + p, R.data() + p}, 2, (std::uint32_t)std::min<std::size_t>(512, n - p)};
        lim.process(b);
    }
    const auto r = measure(L, R, sr);
    check(r.truePeakDb <= -1.0, "Limiter ceiling -1.0 dBTP on +14 dB-hot noise: measured TP", r.truePeakDb, "dBTP");
    check(r.samplePeakDb <= -1.0 + 1e-3, "Limiter: sample peak ≤ ceiling", r.samplePeakDb, "dBFS");
}

static void testArtifactLatencyAndTransparency() {
    // With all amounts at 0 the destroyer must be transparent (only delay).
    ArtifactProcessor ap;
    for (std::size_t i = 0; i < ap.numParams(); ++i) ap.param(i).set(ap.param(i).target());
    ap.param(ArtifactProcessor::kShimmer).set(0); ap.param(ArtifactProcessor::kSib).set(0); ap.param(ArtifactProcessor::kPhase).set(0);
    ap.prepare(48000, 512);
    const std::size_t n = 48000; std::vector<float> L(n), R(n), oL, oR;
    std::mt19937 rng(5); std::normal_distribution<float> g(0, 0.2f);
    for (std::size_t i = 0; i < n; ++i) { L[i] = g(rng); R[i] = 0.5f * L[i] + g(rng); }
    oL = L; oR = R;
    for (std::size_t p = 0; p < n; p += 512) { AudioBlock b{{oL.data() + p, oR.data() + p}, 2, (std::uint32_t)std::min<std::size_t>(512, n - p)}; ap.process(b); }
    const std::size_t lat = ap.latency(); double err = 0, sig = 0;
    for (std::size_t i = lat; i < n; ++i) { err += std::pow(oL[i] - L[i - lat], 2) + std::pow(oR[i] - R[i - lat], 2); sig += L[i - lat] * L[i - lat] + R[i - lat] * R[i - lat]; }
    check(10 * std::log10(err / sig) < -90, "Artifact Destroyer, amounts = 0 → transparent (error dB)", 10 * std::log10(err / sig), "dB");
}

static void testArtifactShimmerReduction() {
    // Synthetic "codec shimmer": a steady 12 kHz-band noise whose per-frame
    // level jitters ±6 dB randomly (what neural-codec HF sounds like), plus a
    // clean 1 kHz tone. The destroyer should reduce HF frame-to-frame jitter
    // while leaving the 1 kHz tone level untouched.
    const double sr = 48000; const std::size_t n = std::size_t(sr * 6);
    std::vector<float> L(n), R(n);
    std::mt19937 rng(6); std::normal_distribution<float> g(0, 1); std::uniform_real_distribution<float> u(-6, 6);
    Biquad bp; bp.setCoeffs(designBiquad(FilterType::BandPass, sr, 12000, 2.0, 0));
    float lvl = 1;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 480 == 0) lvl = std::pow(10.f, u(rng) / 20.f) * 0.05f;  // new random level every 10 ms
        const float hf = float(bp.tick(g(rng))) * lvl;
        L[i] = 0.25f * std::sin(2 * float(kPi) * 1000.f * float(i) / float(sr)) + hf; R[i] = L[i];
    }
    auto jitter = [&](const std::vector<float>& x, std::size_t from) {
        // mean |Δ dB| of 12 kHz-band energy across consecutive 10 ms windows
        Biquad b2; b2.setCoeffs(designBiquad(FilterType::BandPass, sr, 12000, 2.0, 0));
        double prev = 0, acc = 0; int cnt = 0; double e = 0;
        for (std::size_t i = from; i < x.size(); ++i) {
            const double y = b2.tick(x[i]); e += y * y;
            if ((i - from) % 480 == 479) { const double db = 10 * std::log10(e + 1e-20); if (cnt++) acc += std::fabs(db - prev); prev = db; e = 0; }
        }
        return acc / (cnt - 1);
    };
    auto tone = [&](const std::vector<float>& x, std::size_t from) {
        Biquad b2; b2.setCoeffs(designBiquad(FilterType::BandPass, sr, 1000, 8.0, 0));
        double e = 0; for (std::size_t i = from; i < x.size(); ++i) { const double y = b2.tick(x[i]); e += y * y; }
        return 10 * std::log10(e / double(x.size() - from));
    };
    auto hfE = [&](const std::vector<float>& x, std::size_t from) {
        Biquad b2; b2.setCoeffs(designBiquad(FilterType::BandPass, sr, 12000, 2.0, 0));
        double e = 0; for (std::size_t i = from; i < x.size(); ++i) { const double y = b2.tick(x[i]); e += y * y; }
        return 10 * std::log10(e / double(x.size() - from));
    };
    const double jIn = jitter(L, 48000), tIn = tone(L, 48000), hfIn = hfE(L, 48000);
    ArtifactProcessor ap; ap.param(ArtifactProcessor::kShimmer).set(1.0f); ap.param(ArtifactProcessor::kSib).set(0); ap.param(ArtifactProcessor::kPhase).set(0);
    ap.prepare(sr, 512);
    for (std::size_t p = 0; p + 512 <= n; p += 512) { AudioBlock b{{L.data() + p, R.data() + p}, 2, 512}; ap.process(b); }
    const double jOut = jitter(L, 48000 + ap.latency()), tOut = tone(L, 48000 + ap.latency()), hfOut = hfE(L, 48000 + ap.latency());
    check(jOut < jIn * 0.8, "Shimmer smoother: HF frame-to-frame jitter in → out (dB/frame)", jOut, "(lower is better)");
    std::printf("        jitter in = %.2f dB/frame, out = %.2f dB/frame\n", jIn, jOut);
    check(std::fabs(tOut - tIn) < 0.25, "Shimmer smoother: 1 kHz tone level change", tOut - tIn, "dB");
    check(std::fabs(hfOut - hfIn) < 1.0, "Shimmer smoother: long-term 12 kHz band energy change", hfOut - hfIn, "dB");
}

static void testEngine() {
    auto eng = std::make_unique<StemEngine>();
    const double sr = 48000; const std::uint32_t blk = 256;
    eng->prepare(sr, blk, 4);
    const std::size_t n = 48128;   // whole number of 256-frame blocks
    std::vector<std::vector<float>> src(kNumStems, std::vector<float>(n));
    for (std::size_t s = 0; s < kNumStems; ++s) for (std::size_t i = 0; i < n; ++i) src[s][i] = 0.01f * float(s + 1) * std::sin(float(i) * 0.001f * float(s + 1));
    for (std::size_t s = 0; s < kNumStems; ++s) eng->setSource(s, src[s].data(), src[s].data(), n);
    eng->play();
    std::vector<float> oL(n), oR(n);
    for (std::size_t p = 0; p < n; p += blk) eng->process(oL.data() + p, oR.data() + p, blk);
    double err = 0;
    for (std::size_t i = 0; i < n; ++i) { double sum = 0; for (auto& v : src) sum += v[i]; err = std::max(err, std::fabs(sum - oL[i])); }
    check(err < 1e-5, "13-stem engine, empty chains, 4 workers: master == Σ stems", err);

    // Latency compensation: put an Artifact Destroyer (latency 2048) on stem 0 only.
    auto ch = std::make_unique<ProcessorChain>();
    auto ap = std::make_unique<ArtifactProcessor>();
    ap->param(ArtifactProcessor::kShimmer).set(0); ap->param(ArtifactProcessor::kSib).set(0); ap->param(ArtifactProcessor::kPhase).set(0);
    ch->add(std::move(ap));
    eng->publishChain(0, std::move(ch));
    eng->seek(0);
    for (std::size_t p = 0; p < n; p += blk) eng->process(oL.data() + p, oR.data() + p, blk);
    const std::uint32_t lat = eng->totalLatency();
    err = 0;
    for (std::size_t i = lat + 1024; i < n; ++i) { double sum = 0; for (auto& v : src) sum += v[i - lat]; err = std::max(err, std::fabs(sum - oL[i])); }
    check(lat == 1024, "PDC: total latency after adding STFT plugin to stem 0", lat, "samples");
    check(err < 1e-4, "PDC: all 13 stems time-aligned at master (max error)", err);
    eng->collectGarbage();
    eng->shutdown();
}

static void benchEngine() {
    // Full chain on every stem: EQ (2 bands active) + Comp + Artifact Destroyer;
    // master: EQ + Limiter. Measures real-time factor on THIS machine.
    const double sr = 48000; const std::uint32_t blk = 256;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    auto eng = std::make_unique<StemEngine>();
    eng->prepare(sr, blk, hw > 1 ? hw - 1 : 0);
    auto reg = makeBuiltinRegistry();
    const std::size_t n = std::size_t(sr * 10);
    std::vector<float> noise(n);
    std::mt19937 rng(7); std::normal_distribution<float> g(0, 0.1f);
    for (auto& v : noise) v = g(rng);
    for (std::size_t s = 0; s < kNumStems; ++s) {
        eng->setSource(s, noise.data(), noise.data(), n);
        auto ch = std::make_unique<ProcessorChain>();
        auto eq = reg.create("hc.eq6"); eq->param(4).set(2.0f); eq->param(13).set(-1.5f);
        ch->add(std::move(eq)); ch->add(reg.create("hc.comp")); ch->add(reg.create("hc.artifact"));
        eng->publishChain(s, std::move(ch));
    }
    auto mc = std::make_unique<ProcessorChain>(); mc->add(reg.create("hc.eq6")); mc->add(reg.create("hc.limiter"));
    eng->publishChain(StemEngine::kMaster, std::move(mc));
    eng->play();
    std::vector<float> oL(blk), oR(blk);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t p = 0; p + blk <= n; p += blk) eng->process(oL.data(), oR.data(), blk);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[BENCH] 13 stems × (EQ+Comp+ArtifactDestroyer) + master (EQ+TP limiter), %u workers + conductor:\n"
                "        10.0 s of audio in %.3f s  →  %.1fx real-time on this %u-core cloud VM\n",
                hw > 1 ? hw - 1 : 0, secs, 10.0 / secs, hw);
    eng->shutdown();
}

int main() {
    simd::ScopedFlushDenormals ftz;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("THE HYBRID CORE 2.0 — engine verification (%s)\n\n", HC_AVX2 ? "AVX2 build" : "SCALAR build");
    testComplexFFT(); testRealFFT(); testStftIdentity(); testKWeighting();
    testLoudness(); testTruePeak(); testLimiter();
    testArtifactLatencyAndTransparency(); testArtifactShimmerReduction();
    testEngine(); benchEngine();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
