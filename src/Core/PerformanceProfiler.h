#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include <juce_core/juce_core.h>

namespace more_phi {

/**
 * Statistics for profiling a specific operation.
 *
 * AUDIT-FIX (M4, documented slice): these are RUNNING totals accumulated since
 * the section was first registered. Consequences a consumer must know:
 *   - averageTimeMs is dominated by early history late in a long session. For a
 *     "what just happened" view, snapshot reset() between reporting windows.
 *   - Only mean/min/max are tracked here — no per-section p50/p95/p99. A ring
 *     buffer of the last N samples would add them but doubles the per-section
 *     allocation and adds a write to the audio-thread hot path (the C-2/C-16
 *     no-alloc/no-block fixes must be preserved). Upgrade path: add a
 *     std::array<double, kRingSamples> per section, push in updateStats() under
 *     the existing try-lock, compute percentiles in getAllStats().
 *   - The nested-section double-count caveat (container vs leaf sections) is
 *     noted in MorePhiProcessor::getProfilingReport.
 */
struct ProfileStats {
    size_t callCount = 0;
    double totalTimeMs = 0.0;
    double averageTimeMs = 0.0;
    double minTimeMs = 0.0;
    double maxTimeMs = 0.0;
};

/**
 * Performance profiler for measuring and tracking execution times.
 * Thread-safe for recording times from multiple threads.
 */
class PerformanceProfiler {
public:
    /**
     * RAII timer that records execution time on destruction.
     */
    class Timer {
    public:
        Timer(PerformanceProfiler& profiler, const std::string& name);
        ~Timer();

        // Non-copyable, non-movable (destructor records timing — a moved-from
        // Timer would record a spurious zero-duration measurement)
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        Timer(Timer&&) = delete;
        Timer& operator=(Timer&&) = delete;

    private:
        PerformanceProfiler& profiler_;
        std::string name_;
        std::chrono::high_resolution_clock::time_point startTime_;
    };

    PerformanceProfiler() = default;
    ~PerformanceProfiler() = default;

    // Non-copyable, movable
    PerformanceProfiler(const PerformanceProfiler&) = delete;
    PerformanceProfiler& operator=(const PerformanceProfiler&) = delete;
    PerformanceProfiler(PerformanceProfiler&&) = default;
    PerformanceProfiler& operator=(PerformanceProfiler&&) = default;

    /**
     * Pre-allocate profiling buckets and set max load factor.
     * Call from the message thread before audio starts.
     */
    void prepare();

    /**
     * Register a section name so the audio thread can record it without
     * allocating. Call from the message thread before audio starts.
     */
    void registerSection(const std::string& name);

    /**
     * Create an RAII timer for the given operation name.
     */
    Timer createTimer(const std::string& name);

    /**
     * Manually record execution time for an operation.
     */
    void recordTime(const std::string& name, double timeMs);

    /**
     * Get statistics for a specific operation.
     */
    ProfileStats getStats(const std::string& name) const;

    /**
     * Get statistics for all tracked operations.
     */
    std::unordered_map<std::string, ProfileStats> getAllStats() const;

    /**
     * Reset all statistics.
     */
    void reset();

    /**
     * Reset statistics for a specific operation.
     */
    void reset(const std::string& name);

private:
    /**
     * Update statistics for the given operation with a new time measurement.
     */
    void updateStats(const std::string& name, double timeMs);

    // SpinLock for recordTime() — never blocks audio thread (uses tryEnter).
    // std::mutex retained for reader methods (message thread only, blocking OK).
    mutable juce::SpinLock statsSpinLock_;
    std::unordered_map<std::string, ProfileStats> stats_;
};

/**
 * Convenience macro for creating RAII timers.
 * Compiles to a no-op unless MORE_PHI_ENABLE_PROFILING is defined (opt-in).
 * This avoids chrono syscalls on the audio thread in release builds.
 */
#define MORE_PHI_PROFILE_CONCAT_IMPL(a, b) a##b
#define MORE_PHI_PROFILE_CONCAT(a, b) MORE_PHI_PROFILE_CONCAT_IMPL(a, b)
#ifdef MORE_PHI_ENABLE_PROFILING
  #define MORE_PHI_PROFILE(profiler, name) \
      more_phi::PerformanceProfiler::Timer MORE_PHI_PROFILE_CONCAT(morephiProfileTimer_, __LINE__)(profiler, name)
#else
  #define MORE_PHI_PROFILE(profiler, name)  (void)0
#endif

} // namespace more_phi
