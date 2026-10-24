@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set STEMS=
for %%f in (demo_stems\*.wav) do set STEMS=!STEMS! "%%f"
echo Rendering 13 demo stems to demo_master.wav...
bin-windows\hc_render.exe --target -14 --ceiling -1 demo_master.wav !STEMS! > render_report.txt 2>&1
type render_report.txt
echo.
echo Report saved to render_report.txt
pause
