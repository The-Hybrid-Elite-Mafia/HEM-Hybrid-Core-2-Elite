# =============================================================================
#  THE HYBRID CORE 2.0 — build. Zero third-party dependencies.
#  Windows (Visual Studio 2022):  cmake -B build -G "Visual Studio 17 2022" -A x64
#                                 cmake --build build --config Release
#  Linux/macOS (offline tools):   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(HybridCore2Elite LANGUAGES CXX VERSION 2.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(hybrid_core INTERFACE)
target_include_directories(hybrid_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  # /arch:AVX2 enables AVX2 + FMA codegen; /fp:precise keeps loudness math deterministic.
  target_compile_options(hybrid_core INTERFACE /arch:AVX2 /O2 /fp:precise /permissive- /W4 /utf-8)
  target_compile_definitions(hybrid_core INTERFACE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(hybrid_core INTERFACE -mavx2 -mfma -O2 -Wall -Wextra)
  find_package(Threads REQUIRED)
  target_link_libraries(hybrid_core INTERFACE Threads::Threads)
endif()

add_executable(hc_tests tests/test_main.cpp)
target_link_libraries(hc_tests PRIVATE hybrid_core)

add_executable(hc_render apps/hc_render.cpp)
target_link_libraries(hc_render PRIVATE hybrid_core)
if(WIN32)
  target_link_libraries(hc_render PRIVATE avrt)
endif()

if(WIN32)
  add_executable(hc_play apps/hc_play.cpp src/platform/wasapi_output.cpp)
  target_link_libraries(hc_play PRIVATE hybrid_core ole32 avrt)

  # THE HYBRID CORE Studio — the faceplate app.
  add_executable(HybridCore2Elite WIN32 studio/main.cpp studio/gfx.cpp src/platform/wasapi_output.cpp studio/studio.rc)
  target_include_directories(HybridCore2Elite PRIVATE studio)
  target_compile_definitions(HybridCore2Elite PRIVATE UNICODE _UNICODE)
  target_link_libraries(HybridCore2Elite PRIVATE hybrid_core d3d11 dxgi d3dcompiler windowscodecs ole32 uuid avrt comdlg32 shell32 gdi32 user32)
  if(MSVC)
    target_link_options(HybridCore2Elite PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)
  else()
    target_link_options(HybridCore2Elite PRIVATE -municode)
  endif()

  # Installer: embeds the built HybridCore2Elite.exe (setup.rc points at ../build-win/HybridCore2Elite.exe;
  # build_windows.sh produces dist/HybridCore2Elite-Setup.exe the same way).
endif()
