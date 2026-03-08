#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>

namespace morphsnap {

/**
 * Statistics for profiling a specific operation.
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

        // Non-copyable, movable
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        Timer(Timer&&) = default;
        Timer& operator=(Timer&&) = default;

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

    mutable std::mutex statsMutex_;
    std::unordered_map<std::string, ProfileStats> stats_;
};

/**
 * Convenience macro for creating RAII timers.
 */
#define MORPHSNAP_PROFILE_CONCAT_IMPL(a, b) a##b
#define MORPHSNAP_PROFILE_CONCAT(a, b) MORPHSNAP_PROFILE_CONCAT_IMPL(a, b)
#define MORPHSNAP_PROFILE(profiler, name) auto MORPHSNAP_PROFILE_CONCAT(morphsnapProfileTimer_, __LINE__) = profiler.createTimer(name)

} // namespace morphsnap
