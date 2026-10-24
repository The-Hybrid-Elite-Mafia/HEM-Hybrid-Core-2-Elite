// =============================================================================
//  platform/thread_priority.hpp — real-time thread registration.
//
//  Windows: MMCSS "Pro Audio" task + AVRT_PRIORITY_CRITICAL. MMCSS gives the
//  thread a guaranteed slice of each scheduling period and protects it from
//  being starved by UI/background work — this is what DAWs use on Windows.
//  Falls back to THREAD_PRIORITY_TIME_CRITICAL if MMCSS is unavailable.
//  Other OSes: no-op (the offline renderer doesn't need it).
// =============================================================================
#pragma once
#include "hc/dsp/simd.hpp"
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <avrt.h>
  #if defined(_MSC_VER)
    #pragma comment(lib, "avrt.lib")
  #endif
#endif

namespace hc::platform {

struct RealtimeThreadScope {
#if defined(_WIN32)
    HANDLE task = nullptr;
    DWORD taskIndex = 0;
#endif
    RealtimeThreadScope() noexcept {
#if defined(_WIN32)
        task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    }
    ~RealtimeThreadScope() {
#if defined(_WIN32)
        if (task) AvRevertMmThreadCharacteristics(task);
#endif
    }
    RealtimeThreadScope(const RealtimeThreadScope&) = delete;
    RealtimeThreadScope& operator=(const RealtimeThreadScope&) = delete;
};

// For WorkerPool::start(): registers each DSP worker for its whole lifetime.
inline void initRealtimeWorker(unsigned /*index*/) {
    static thread_local RealtimeThreadScope scope;   // lives until the worker exits
    static thread_local simd::ScopedFlushDenormals ftz;
    (void)scope; (void)ftz;
}

} // namespace hc::platform
