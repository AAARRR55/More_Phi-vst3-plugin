/*
 * More-Phi — Core/MorePhiDiagnostics.cpp
 */
#include "Core/MorePhiDiagnostics.h"
#include "Plugin/PluginProcessor.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
#endif

namespace more_phi {

namespace {
constexpr int kSnapshotIntervalMs = 10000;  // full snapshot every 10s
constexpr int kWatchdogIntervalMs = 250;    // stall detection
constexpr double kStallThresholdMs = 800.0;

struct ThreadCpuSample { juce::int64 totalCpuMs = 0; int threadCount = 0; };

ThreadCpuSample sampleProcessThreads()
{
    ThreadCpuSample s;
#if JUCE_WINDOWS
    const ::DWORD pid = ::GetCurrentProcessId();
    const ::HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return s;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (::Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid) continue;
            ++s.threadCount;
            const ::HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (h == nullptr) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (::GetThreadTimes(h, &created, &exited, &kernel, &user))
            {
                const auto toMs = [](const FILETIME& f) {
                    ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime;
                    return static_cast<juce::int64>(u.QuadPart / 10000);
                };
                s.totalCpuMs += toMs(kernel) + toMs(user);
            }
            ::CloseHandle(h);
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
#endif
    return s;
}
}  // namespace

MorePhiDiagnostics::MorePhiDiagnostics(MorePhiProcessor& owner, PerformanceProfiler& profiler)
    : owner_(owner), profiler_(profiler)
{
#ifdef MORE_PHI_ENABLE_PROFILING
    const auto dir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile("MorePhi");
    dir.createDirectory();
    logFile_ = dir.getChildFile("diagnostics-"
        + juce::String(juce::Time::currentTimeMillis()) + ".log");
#endif
}

void MorePhiDiagnostics::start() noexcept
{
#ifdef MORE_PHI_ENABLE_PROFILING
    startTimer(kWatchdogIntervalMs);
#else
    juce::ignoreUnused(kSnapshotIntervalMs);
#endif
}

void MorePhiDiagnostics::stop() noexcept { stopTimer(); }

void MorePhiDiagnostics::onTraceEnter(Trace& t) noexcept
{
    t.enterMs = juce::Time::getMillisecondCounterHiRes();
    activeEnterMs_.store(t.enterMs, std::memory_order_relaxed);
    activeLabel_.store(t.label, std::memory_order_relaxed);
}

void MorePhiDiagnostics::onTraceExit(Trace& t) noexcept
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    t.exitMs = now;
    lastExitMs_.store(now, std::memory_order_relaxed);
    const int idx = traceHead_.load(std::memory_order_relaxed);
    traceLabels_[idx] = t.label;
    traceDurationsMs_[idx] = now - t.enterMs;
    traceHead_.store((idx + 1) % kTraceRingSize, std::memory_order_relaxed);
    activeLabel_.store(nullptr, std::memory_order_relaxed);
}

void MorePhiDiagnostics::timerCallback()
{
#ifdef MORE_PHI_ENABLE_PROFILING
    const double now = juce::Time::getMillisecondCounterHiRes();
    static thread_local double lastFireMs = 0.0;
    const double lag = lastFireMs > 0.0 ? juce::jmax(0.0, now - lastFireMs - kWatchdogIntervalMs) : 0.0;
    lastFireMs = now;

    // Every ~10s (every 40th watchdog tick), write a full snapshot. On any stall
    // (>800ms late), write immediately with the trace ring so we see the culprit.
    static int snapshotTick = 0;
    const bool dueSnapshot = (++snapshotTick % (kSnapshotIntervalMs / kWatchdogIntervalMs)) == 0;
    const bool stall = lag > kStallThresholdMs;
    if (! dueSnapshot && ! stall)
        return;

    if (firstSnapshotMs_ == 0) { firstSnapshotMs_ = juce::Time::currentTimeMillis(); lastSnapshotMs_ = firstSnapshotMs_; }
    const juce::int64 curMs = juce::Time::currentTimeMillis();
    const juce::int64 wallDelta = curMs - lastSnapshotMs_;
    lastSnapshotMs_ = curMs;

    juce::String out;
    out << (stall ? "!!! STALL DETECTED !!!\n" : "");
    out << "=== MorePhi diagnostics @ "
        << juce::Time::getCurrentTime().toString(true, true, true, false)
        << " (uptime "
        << juce::RelativeTime::milliseconds(curMs - firstSnapshotMs_).inMinutes()
        << " min) ===\n";
    out << "message_thread_lag_ms: " << juce::String(lag, 1)
        << " [watchdog expected every " << kWatchdogIntervalMs << "ms]\n\n";

    if (stall)
    {
        const char* active = activeLabel_.load(std::memory_order_relaxed);
        out << "--- MESSAGE THREAD WAS BLOCKED ---\n";
        out << "Active operation when watchdog fired: "
            << (active != nullptr ? active : "<none/idle>") << "\n";
        const double enter = activeEnterMs_.load(std::memory_order_relaxed);
        if (enter > 0.0 && active != nullptr)
            out << "  (started " << juce::String(now - enter, 0) << "ms ago)\n";
        out << "\nRecent completed operations (label : duration_ms):\n";
        const int head = traceHead_.load(std::memory_order_relaxed);
        for (int i = 0; i < kTraceRingSize; ++i)
        {
            const int idx = (head + kTraceRingSize - 1 - i) % kTraceRingSize;
            if (traceLabels_[idx] != nullptr)
                out << "  " << traceLabels_[idx] << " : " << juce::String(traceDurationsMs_[idx], 1) << "ms\n";
        }
        out << "\n";
    }

    {
        const ThreadCpuSample cur = sampleProcessThreads();
        const juce::int64 totalDelta = cur.totalCpuMs - prevTotalCpuMs_;
        prevTotalCpuMs_ = cur.totalCpuMs;
        const double totalPct = wallDelta > 0 ? (100.0 * static_cast<double>(totalDelta) / static_cast<double>(wallDelta)) : 0.0;
        out << "--- process thread CPU ---\n";
        out << "  thread_count:      " << cur.threadCount << "\n";
        out << "  total_process_cpu: " << juce::String(totalPct, 1) << "%  (" << totalDelta << " ms)\n\n";
    }

    out << owner_.getProfilingReport();

    juce::FileOutputStream os(logFile_);
    if (os.openedOk())
    {
        if (logFile_.getSize() == 0) os << "MorePhi diagnostic log (profiling ON)\n";
        os << out << "\n";
        os.flush();
    }
#endif
}

} // namespace more_phi
