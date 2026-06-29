#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <mutex>
#include <juce_core/juce_core.h>

namespace more_phi {

/**
 * Statistics for profiling a specific operation.
 *
 * These are RUNNING totals accumulated since the section was first registered,
 * PLUS per-section percentiles (p50/p95/p99) computed over a trailing ring of
 * the most recent samples.
 *
 * AUDIT-FIX (M4, implemented): a per-section fixed-size ring (kRingSamples,
 * power-of-two) is pushed in updateStats() under the EXISTING try-lock — no new
 * allocation, no new lock, so the C-2/C-16 audio-thread no-alloc/no-block
 * contract is preserved. Percentiles are computed in getStats()/getAllStats()
 * (message-thread only) by sorting a copy of the populated ring window.
 *
 * Consequences a consumer must know:
 *   - averageTimeMs is still dominated by early history late in a long session.
 *     For a "what just happened" view, snapshot reset() between reporting
 *     windows. p50/p95/p99 are trailing-window and reflect the most recent
 *     kRingSamples samples, so they are NOT subject to the same early-history
 *     drift.
 *   - p50/p95/p99 default to 0.0 until at least one sample lands in the ring.
 *   - The nested-section double-count caveat (container vs leaf sections) is
 *     noted in MorePhiProcessor::getProfilingReport.
 *   - processBlock-level percentiles are the responsibility of the CALLER's
 *     timing harness (e.g. the audit harness's HighResTimer), which times the
 *     whole block and computes population percentiles across all passes. The
 *     profiler's ring is per-section, not per-block.
 */
struct ProfileStats {
    size_t callCount = 0;
    double totalTimeMs = 0.0;
    double averageTimeMs = 0.0;
    double minTimeMs = 0.0;
    double maxTimeMs = 0.0;
    double p50Ms = 0.0; // median over the trailing ring window
    double p95Ms = 0.0;
    double p99Ms = 0.0;
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
     * Per-section trailing ring buffer. The ring is allocated once in
     * registerSection() (message thread, before audio starts); the audio thread
     * only does a fixed-index write + modulo advance under the existing
     * try-lock, so no allocation and no blocking (C-2/C-16 invariants preserved).
     *
     * kRingSamples is a power of two so `head & (kRingSamples - 1)` replaces a
     * modulo with a mask (one cycle vs ~20 for integer div). Total ring memory
     * is ~kRingSamples * sizeof(double) * numSections ≈ 2048 * 8 * ~18 ≈ 288 KB.
     *
     * Defined before computeStats() so the latter can use it by value without a
     * forward declaration.
     */
    static constexpr std::size_t kRingSamples = 2048;

    struct SectionRecord
    {
        ProfileStats stats;
        std::array<double, kRingSamples> ring{};
        std::size_t ringHead = 0;  // next write index
        std::size_t ringCount = 0; // populated count (caps at kRingSamples)
    };

    /**
     * Update statistics for the given operation with a new time measurement.
     */
    void updateStats(const std::string& name, double timeMs);

    /**
     * Compute the public ProfileStats (including percentiles) from a record.
     * Message-thread only — allocates a sort scratch.
     */
    ProfileStats computeStats(const SectionRecord& record) const;

    // SpinLock for recordTime() — never blocks audio thread (uses tryEnter).
    // std::mutex retained for reader methods (message thread only, blocking OK).
    mutable juce::SpinLock statsSpinLock_;
    std::unordered_map<std::string, SectionRecord> records_;
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
