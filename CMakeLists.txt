# =============================================================================
#  HYBRID CORE 2 ELITE — build. Zero third-party dependencies.
#  Started from THE HYBRID CORE 2.0 (github.com/The-Hybrid-Elite-Mafia/hybrid-core, commit a77d3cd).
#  Windows (Visual Studio 2022):  cmake -B build -G "Visual Studio 17 2022" -A x64
#                                 cmake --build build --config Release
#  Linux/macOS (offline tools):   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(HybridCore2Elite LANGUAGES CXX VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 20)
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
endif()
