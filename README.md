# HYBRID CORE 2 ELITE — Hybrid Elite Mafia

The desk studio for AI stems: 13 stems in, AI artifacts cleaned out, mixed, mastered to a loudness target.

Started from **THE HYBRID CORE 2.0** (github.com/The-Hybrid-Elite-Mafia/hybrid-core, commit a77d3cd), a separate
project that keeps its own name and history. This repository is its own app; changes here never go back there.

---


Scratch-built 13-stem audio engine, artifact-repair DSP and mastering tools in pure C++20.
No JUCE, no Qt, no third-party libraries — only the C++ standard library and the Windows SDK.

## Build on Windows (Visual Studio 2022)

    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

Produces `hc_tests.exe`, `hc_render.exe`, `hc_play.exe` in `build\Release`.
Prebuilt Windows x64 binaries (cross-compiled with MinGW-w64, static) are in `bin-windows/`.

## Run

    hc_tests                                   (verification suite — should end "ALL PASSED")
    hc_render --target -14 --ceiling -1 master.wav stem1.wav ... stem13.wav
    hc_play [--exclusive] stem1.wav ... stem13.wav

hc_render options: `--shimmer 0..1`, `--deess 0..1`, `--phase 0..1`, `--no-repair`,
`--deess-all`, `--float`. All stems must share one sample rate.

## Status

- Verified: FFT, STFT/OLA, loudness (BS.1770-4/R128), true peak, limiter, 13-stem engine,
  delay compensation, Artifact Destroyer (synthetic tests), ASan/UBSan/TSan clean.
- Compiled but NOT yet run on real hardware: the WASAPI backend (`hc_play`).
- Not yet tested on real Suno stems.
- Not built yet: GPU Graphics Engine, ASIO backend (needs Steinberg's ASIO SDK).

See the Technical Blueprint doc for architecture and DSP theory.
