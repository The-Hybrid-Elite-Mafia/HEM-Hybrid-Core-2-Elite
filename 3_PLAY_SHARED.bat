@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set STEMS=
for %%f in (demo_stems\*.wav) do set STEMS=!STEMS! "%%f"
echo Real-time playback, WASAPI shared low-latency. Press any key to stop.
bin-windows\hc_play.exe !STEMS!
echo.
pause
