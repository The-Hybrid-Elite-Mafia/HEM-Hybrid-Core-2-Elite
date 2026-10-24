#!/usr/bin/env bash
# Builds the Windows app + installer with MinGW-w64 (cross-compile from Linux).
# On Windows with Visual Studio, use CMake instead (see CMakeLists.txt).
#
# ASIO: if Steinberg's ASIO SDK is unpacked under external/asiosdk (any sub-folder holding
# common/asio.h), it is compiled in (GPLv3 option) and the app offers ASIO drivers. The SDK is
# never committed — .github/workflows/release.yml downloads it. Without it the app is WASAPI-only.
set -euo pipefail
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++-posix
RC=x86_64-w64-mingw32-windres
FLAGS="-std=c++23 -O2 -mavx2 -mfma -Wall -Wextra -municode -mwindows -static"
mkdir -p build-win dist
python3 studio/skin_layout.py

ASIO_FLAGS=()
ASIO_OBJS=()
SDK=$(dirname "$(find external/asiosdk -path '*/common/asio.h' 2>/dev/null | head -1)" 2>/dev/null || true)
if [ -n "${SDK:-}" ] && [ "$SDK" != "." ] && [ -f "$SDK/asio.h" ]; then
  SDK=$(dirname "$SDK")
  echo "ASIO SDK: $SDK"
  ASIO_FLAGS=(-DHC_HAS_ASIO_SDK=1 -I"$SDK/common" -I"$SDK/host" -I"$SDK/host/pc")
  for f in common/asio.cpp host/asiodrivers.cpp host/pc/asiolist.cpp; do
    o="build-win/asiosdk_$(basename "$f" .cpp).o"
    # Steinberg's own sources: built as they are (C++14, ANSI, warnings off).
    $CXX -std=c++14 -O2 -w -I"$SDK/common" -I"$SDK/host" -I"$SDK/host/pc" -c "$SDK/$f" -o "$o"
    ASIO_OBJS+=("$o")
  done
else
  echo "ASIO SDK: not found (WASAPI-only build)"
fi

$RC -I studio studio/studio.rc -O coff -o build-win/studio_res.o
$CXX $FLAGS "${ASIO_FLAGS[@]}" -Iinclude -Istudio studio/main.cpp studio/gfx.cpp src/platform/wasapi_output.cpp src/platform/asio_output.cpp \
     "${ASIO_OBJS[@]}" build-win/studio_res.o \
     -o build-win/HybridCore2Elite.exe -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -luuid -lavrt -lcomdlg32 -lshell32 -lgdi32 -luser32 -ladvapi32
x86_64-w64-mingw32-strip build-win/HybridCore2Elite.exe
( cd installer && $RC -I . setup.rc -O coff -o ../build-win/setup_res.o )
$CXX $FLAGS installer/setup.cpp build-win/setup_res.o -o "dist/HybridCore2Elite-Setup.exe" -lole32 -luuid -lshell32 -ladvapi32
x86_64-w64-mingw32-strip "dist/HybridCore2Elite-Setup.exe"
ls -la dist/
