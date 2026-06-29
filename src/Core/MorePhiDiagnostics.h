/*
 * More-Phi — Core/MorePhiDiagnostics.h
 *
 * Diagnostic-only. Near-no-op when MORE_PHI_ENABLE_PROFILING is undefined.
 *
 * When profiling IS enabled:
 *  - A fast watchdog timer (250 ms) detects message-thread stalls. If it fires
 *    more than ~800 ms late, the most recent traced operation is logged as the
 *    likely culprit.
 *  - MsgThreadTrace() is an RAII tracer: drop one in any timerCallback /
 *    maintenance operation and its entry/exit timestamps are recorded. When a
 *    stall is detected, the trace buffer shows exactly which operation was
 *    running when the message thread blocked.
 *  - Also records total process CPU% (sum across threads) to rule in/out a spin.
 *
 * Writes to <userAppData>/MorePhi/diagnostics-<pid>.log every snapshot. No
 * behaviour change.
 */
#pragma once

#include "Core/PerformanceProfiler.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>

namespace more_phi {

class MorePhiProcessor;

class MorePhiDiagnostics : private juce::Timer
{
public:
    explicit MorePhiDiagnostics(MorePhiProcessor& owner, PerformanceProfiler& profiler);

    void start() noexcept;
    void stop()  noexcept;

    // RAII tracer. Use via the MSG_TRACE macro below. Records entry/exit on the
    // current (message) thread. No-op when profiling is off.
    struct Trace
    {
        const char* label;
        double enterMs;
        double exitMs;
        std::atomic<Trace*> self;  // points to the active instance on this thread
    };

    // Called by the tracer on enter/exit. Lock-free on the hot path.
    void onTraceEnter(Trace& t) noexcept;
    void onTraceExit(Trace& t) noexcept;

private:
    void timerCallback() override;

    MorePhiProcessor&     owner_;
    PerformanceProfiler&  profiler_;
    juce::File            logFile_;
    juce::int64           firstSnapshotMs_ = 0;
    juce::int64           lastSnapshotMs_  = 0;
    juce::int64           prevTotalCpuMs_  = 0;

    // Most recent trace on the message thread (updated by onTraceEnter/Exit).
    std::atomic<const char*> activeLabel_   { nullptr };
    std::atomic<double>      activeEnterMs_ { 0.0 };
    std::atomic<double>      lastExitMs_    { 0.0 };
    // Ring of the last completed traces (label + duration).
    static constexpr int kTraceRingSize = 16;
    const char* traceLabels_[kTraceRingSize] = {};
    double      traceDurationsMs_[kTraceRingSize] = {};
    std::atomic<int> traceHead_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorePhiDiagnostics)
};

// ponytail: minimal RAII tracer. Profiling-off compiles to nothing.
#ifdef MORE_PHI_ENABLE_PROFILING
  #define MSG_TRACE(diagnostics, lbl) \
      ::more_phi::MorePhiDiagnostics::Trace _mt_{ (lbl), 0.0, 0.0, {} }; \
      (diagnostics).onTraceEnter(_mt_); \
      struct _MtGuard_ { \
          ::more_phi::MorePhiDiagnostics::Trace& t; ::more_phi::MorePhiDiagnostics& d; \
          ~_MtGuard_() { d.onTraceExit(t); } \
      } _mt_guard_{ _mt_, (diagnostics) }
#else
  #define MSG_TRACE(diagnostics, lbl) ((void)0)
#endif

} // namespace more_phi
