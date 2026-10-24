# HYBRID CORE 2 ELITE — Hybrid Elite Mafia

The producer desk: a 13-stem repair, mixing and mastering studio for Windows, written from scratch in C++23.
Built from THE HYBRID CORE 2.0 (repo `hybrid-core`, its own separate app — this one installs next to it and never touches it).
It uses no JUCE, no Qt and no third-party libraries — only the C++ standard library and Windows itself
(WASAPI audio, Direct3D 11 graphics).

## Install

1. Download **`HybridCore2Elite-Setup.exe`** (built by `build_windows.sh` into `dist/`).
2. Run it. Windows may show "Windows protected your PC" because the file isn't code-signed.
   Click **More info → Run anyway**.
3. Click **Yes**. It installs to `%LOCALAPPDATA%\Programs\HEM Hybrid Core 2 Elite`, adds Start menu and desktop
   shortcuts (HYBRID CORE 2 ELITE). No admin rights are needed.

To uninstall: **Settings → Apps → HYBRID CORE 2 ELITE → Uninstall**. Your stems, sessions and exports
are never touched.

## Using it

The window is a night-city studio desk with six monitors, and every monitor is a live screen:

| Top row | STFT DESTROYER · NOW PLAYING (wide) · RESONANCE |
|---|---|
| **Bottom row** | **TRANSIENT · CONSOLE (tracks + transport) · MASTERING** |

Click a monitor and it fills the window to work on it; **Esc** or **DESK** (top-right) goes back to the desk;
**Tab / Shift+Tab** or `<` `>` step through the screens. On the desk, the audio interface plays / stops and the
crown is the about box. DESTROYER, RESONANCE and TRANSIENT work on one stem at a time (each stem has its own
copy); `< STEM 7 - VOCALS >` at their top-left picks which. MASTERING is the whole mix. NOW PLAYING shows the
song, clock, bar/beat, tempo, loop, master loudness and true peaks, and an overview of the whole song —
click or drag it to move the playhead. F11 = full screen.

| Part | What it does |
|---|---|
| Track rows 1–13 | Drag the stems — or their whole folder — onto the window. Or click the box in the middle (≡ → Load Stems, Ctrl+L): the picker shows the WAVs; click any one and its whole folder loads, or select just the ones you want. Click a name to select it; drag left/right for volume; double-click for 0 dB; right-click for load/remove. M = mute, S = solo. |
| Transport | Every button is drawn live (lights, presses, hover) — nothing painted. ≡ menu · ⏮ rewind · ▶ play · ■ stop · ● export master · ⟳ LOOP · time · tempo (drag to set) · master level · analyze (per-stem LUFS on the lanes) · ⚙ audio settings · ⊞ full screen |
| HEM STFT DESTROYER | Artifact repair for the selected stem: LIGHT/MEDIUM/HEAVY, STFT resolution, Threshold, Bleed (HF stereo phase fix), Decay. Live spectrogram. The switch or the title toggles bypass. |
| HEM RESONANCE | Dynamic harsh-resonance suppressor: Sensitivity, Frequency, Bandwidth, Reduction, Compensation, SOFT/NORMAL/FAST. The screen shows the spectrum and the live cut curve. |
| HEM TRANSIENT | Transient designer: Attack, Sustain, Punch/Speed, Gain, Mix. Live activity screens. The LED toggles bypass. |
| HEM MASTERING | Master bus: Width → glue (MODE: CLEAN/PUNCH/LOUD) → true-peak limiter (Ceiling, Threshold, Release). LUFS M/S and L/R true-peak meters; Integrated, Short-term, PSR, PLR, LRA. The selector and TARGET set the export loudness. |
| Plug-in tabs | The per-stem monitors have tabs: **DESTROYER / EQ**, **RESONANCE / DE-ESSER**, **TRANSIENT / COMP / GATE**. Every stem has its own copy of all seven. Audio order: Gate → Destroyer → Resonance → EQ → De-esser → Comp → Transient. EQ is on (flat); Gate, De-esser and Comp start OFF (ON/OFF top-right of each). |
| HEM EQ | Six bands (HPF, LOW shelf, BELL 1, BELL 2, HIGH shelf, LPF) over the stem's live spectrum. Drag a dot: left/right = frequency, up/down = boost/cut; wheel on a dot = width (Q); double-click = reset. **FIND PING**: while the ringing plays, finds the tone and cuts it with BELL 2 (−9 dB, narrow). |
| HEM COMPRESSOR | Threshold, Ratio, Knee, Attack, Release, Makeup; in→out curve and gain-reduction bar. |
| HEM GATE | Shuts the stem between hits: Threshold, Range, Attack, Hold, Release, Key HPF (so kick / bass bleed doesn't open it). Level bar, OPEN / SHUT light. |
| HEM DE-ESSER | Hat tamer / de-esser: a high shelf that only comes down when the highs spike. Frequency, Threshold, Max Cut, LISTEN (hear only what's removed). |

Loop: drag across the bar numbers to set the section (snaps to whole bars; Shift = free), click the bar numbers to jump; LOOP or **L** turns it on/off; with no section set it loops the whole song. The wrap is sample-exact inside the audio engine (no gap). Export always renders the whole song. Saved in the session.

Keys: Space play/stop · Home rewind · L loop · Tab next screen · Esc / F9 back to the desk · Ctrl+L load · Ctrl+E export · Ctrl+O / Ctrl+S open/save session.
Knobs: drag up/down (Shift = fine), mouse wheel, double-click = default.

Export writes a 24-bit WAV. With a target set, it renders three passes (measure → limit → verify) so the
master lands on the target LUFS under the true-peak ceiling.

## Build

- **From Linux (the tested path):** `./build_windows.sh` → `dist/HybridCore-Setup.exe` (MinGW-w64).
- **Windows / Visual Studio 2022:** `cmake -B build -G "Visual Studio 17 2022" -A x64` then
  `cmake --build build --config Release`. This adds a `HybridCore` target. The MSVC build of the Studio
  app has **not** been tested yet; the MinGW build has.
- **Engine tests (any OS):** build and run `hc_tests` — it should end `ALL PASSED`.

## Status

- Tested: engine DSP (FFT, STFT, BS.1770 loudness, true peak, limiter, Artifact Destroyer, Resonance,
  Transient, Width), the 13-stem engine (sanitizers clean), the installer and uninstaller, and the app
  rendering and playing 13 stems under Wine.
- Not built yet: ASIO (it needs Steinberg's SDK; the faceplate art mentions ASIO, but playback is WASAPI).
- Stems must all have the same sample rate (there's no resampler yet).

## License

GPLv3 (see `LICENSE`). The ASIO output uses Steinberg's ASIO SDK under its GPLv3 option; the SDK itself
is not in this repository — the release build (`.github/workflows/release.yml`) downloads it. Local builds
without it are Windows-audio (WASAPI) only.
