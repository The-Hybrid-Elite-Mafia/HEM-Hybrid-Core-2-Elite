#!/usr/bin/env bash
# Builds the Windows app + installer with MinGW-w64 (cross-compile from Linux).
# On Windows with Visual Studio, use CMake instead (see CMakeLists.txt).
set -euo pipefail
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++-posix
RC=x86_64-w64-mingw32-windres
FLAGS="-std=c++23 -O2 -mavx2 -mfma -Wall -Wextra -municode -mwindows -static"
mkdir -p build-win dist
python3 studio/skin_layout.py
$RC -I studio studio/studio.rc -O coff -o build-win/studio_res.o
$CXX $FLAGS -Iinclude -Istudio studio/main.cpp studio/gfx.cpp src/platform/wasapi_output.cpp build-win/studio_res.o \
     -o build-win/HybridCore2Elite.exe -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -luuid -lavrt -lcomdlg32 -lshell32 -lgdi32 -luser32
x86_64-w64-mingw32-strip build-win/HybridCore2Elite.exe
( cd installer && $RC -I . setup.rc -O coff -o ../build-win/setup_res.o )
$CXX $FLAGS installer/setup.cpp build-win/setup_res.o -o "dist/HybridCore2Elite-Setup.exe" -lole32 -luuid -lshell32 -ladvapi32
x86_64-w64-mingw32-strip "dist/HybridCore2Elite-Setup.exe"
ls -la dist/
