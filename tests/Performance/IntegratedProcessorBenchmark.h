/*
 * More-Phi — Integrated Processor Benchmark (CPU & Memory Audit)
 *
 * Drives the REAL more_phi::MorePhiProcessor headlessly through the audit
 * scenarios (idle / active / parameter automation / neural isolation / combined)
 * and emits both a human-readable stdout table and a machine-readable JSON file.
 *
 * This harness fills the gaps confirmed in the recon pass:
 *   - No integrated MorePhiProcessor.processBlock benchmark existed
 *     (BenchmarkSuite.cpp explicitly excludes full-plugin processBlock).
 *   - No in-process ORT (SonicMasterDecisionRunner) latency benchmark existed
 *     (only the Python inference_server times PyTorch).
 *   - No parameter-automation DELTA measurement (idle-vs-active) at the
 *     full-plugin level, and no per-component RSS attribution.
 *
 * Reuses the HighResTimer / MemorySnapshot / TimingStats design proven in the
 * (orphaned) ComprehensiveProfilingHarness.cpp, but drives the full processor
 * instead of isolated engines.
 */
#ifndef MORE_PHI_INTEGRATED_PROCESSOR_BENCHMARK_H
#define MORE_PHI_INTEGRATED_PROCESSOR_BENCHMARK_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

// Platform-specific memory measurement
#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__) || defined(__linux__)
    #include <sys/resource.h>
    #include <unistd.h>
#endif

namespace more_phi::audit {

// ═══════════════════════════════════════════════════════════════════════════
// High-resolution timer with memory fences (matches the harness convention)
// ═══════════════════════════════════════════════════════════════════════════
class HighResTimer
{
public:
    void start()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        start_ = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    double elapsedUs() const
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto end = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }
    double elapsedMs() const { return elapsedUs() / 1000.0; }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Process memory snapshot (real RSS, not sizeof estimates)
// ═══════════════════════════════════════════════════════════════════════════
struct MemorySnapshot
{
    std::size_t workingSetBytes  = 0;
    std::size_t privateBytes     = 0;  // Windows only
    std::size_t peakWorkingSet   = 0;
    bool        supported        = false;

    double workingSetMB() const { return static_cast<double>(workingSetBytes) / (1024.0 * 1024.0); }
    double privateMB()    const { return static_cast<double>(privateBytes)    / (1024.0 * 1024.0); }
    double peakMB()       const { return static_cast<double>(peakWorkingSet)  / (1024.0 * 1024.0); }
};

inline MemorySnapshot takeMemorySnapshot()
{
    MemorySnapshot snap;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmcEx{};
    pmcEx.cb = sizeof(pmcEx);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmcEx),
                             sizeof(pmcEx)))
    {
        snap.workingSetBytes = pmcEx.WorkingSetSize;
        snap.privateBytes    = pmcEx.PrivateUsage;
        snap.peakWorkingSet  = pmcEx.PeakWorkingSetSize;
        snap.supported       = true;
    }
#elif defined(__APPLE__) || defined(__linux__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
#  if defined(__APPLE__)
        snap.workingSetBytes = static_cast<std::size_t>(usage.ru_maxrss);
#  else
        snap.workingSetBytes = static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#  endif
        snap.supported = true;
    }
#endif
    return snap;
}

// ═══════════════════════════════════════════════════════════════════════════
// Percentile statistics
// ═══════════════════════════════════════════════════════════════════════════
struct TimingStats
{
    double avgUs = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    int    samples = 0;

    static TimingStats fromSamples(std::vector<double> timings)
    {
        if (timings.empty()) return {};
        std::sort(timings.begin(), timings.end());
        const double total = std::accumulate(timings.begin(), timings.end(), 0.0);
        TimingStats s;
        s.samples = static_cast<int>(timings.size());
        s.avgUs   = total / static_cast<double>(s.samples);
        s.minUs   = timings.front();
        s.maxUs   = timings.back();

        const auto pctl = [&](double p) -> double {
            const double rank = p / 100.0 * static_cast<double>(timings.size() - 1);
            const std::size_t lo = static_cast<std::size_t>(std::floor(rank));
            const std::size_t hi = static_cast<std::size_t>(std::ceil(rank));
            if (lo == hi) return timings[lo];
            const double frac = rank - static_cast<double>(lo);
            return timings[lo] * (1.0 - frac) + timings[hi] * frac;
        };
        s.p50Us = pctl(50.0);
        s.p95Us = pctl(95.0);
        s.p99Us = pctl(99.0);
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// One measured scenario result
// ═══════════════════════════════════════════════════════════════════════════
struct ScenarioResult
{
    std::string id;            // "S2"
    std::string name;          // "Active, null plugin"
    int         blockSize  = 0;
    int         sampleRate = 0;
    int         passes     = 0;

    // CPU (per-block timings, averaged across passes)
    TimingStats cpu;

    double bufferTimeUs = 0.0;  // realtime budget for one buffer
    double cpuPercent   = 0.0;  // % of one core (avgUs / bufferTimeUs)

    // Memory
    std::size_t workingSetDeltaBytes = 0;  // sustained, vs scenario-entry baseline
    std::size_t peakWorkingSetBytes  = 0;

    // Per-section profiling (parsed from getProfilingReport); empty if N/A.
    // AUDIT-2026-06-25: added p50/p95/p99 + maxUs — the in-tree profiler now
    // emits trailing-window percentiles per section (M4 ring-buffer upgrade).
    struct SectionStat
    {
        std::string name;
        double avgUs = 0.0;
        double maxUs = 0.0;
        double p50Us = 0.0;
        double p95Us = 0.0;
        double p99Us = 0.0;
        double pct   = 0.0;
        std::uint64_t calls = 0;
    };
    std::vector<SectionStat> sections;

    bool skipped = false;
    std::string skipReason;
};

// Aggregate across multiple passes: keep per-pass timings, then fold.
struct ScenarioResultSet
{
    std::string id;
    std::string name;
    int blockSize  = 0;
    int sampleRate = 0;
    int passes     = 0;

    std::vector<double> perPassAvgUs;   // one avg per pass (for variance)
    TimingStats         folded;          // across all per-block samples from all passes
    double              bufferTimeUs = 0.0;
    double              cpuPercent   = 0.0;
    std::size_t         workingSetDeltaBytes = 0;
    std::size_t         peakWorkingSetBytes  = 0;
    std::vector<ScenarioResult::SectionStat> sections;
    bool skipped = false;
    std::string skipReason;
};

} // namespace more_phi::audit

#endif // MORE_PHI_INTEGRATED_PROCESSOR_BENCHMARK_H
