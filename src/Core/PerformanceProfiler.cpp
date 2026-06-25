/*
 * More-Phi — Core/PerformanceProfiler.cpp
 *
 * Performance profiling implementation with RAII timers and thread-safe statistics tracking.
 */

#include "PerformanceProfiler.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace more_phi {

// ── Timer Implementation ─────────────────────────────────────────────────

PerformanceProfiler::Timer::Timer(PerformanceProfiler& profiler, const std::string& name)
    : profiler_(profiler)
    , name_(name)
    , startTime_(std::chrono::high_resolution_clock::now())
{
}

PerformanceProfiler::Timer::~Timer()
{
    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = endTime - startTime_;
    profiler_.recordTime(name_, elapsed.count());
}

// ── PerformanceProfiler Implementation ────────────────────────────────────

PerformanceProfiler::Timer PerformanceProfiler::createTimer(const std::string& name)
{
    return Timer(*this, name);
}

void PerformanceProfiler::prepare()
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    records_.reserve(64);
    records_.max_load_factor(0.7f);
}

void PerformanceProfiler::registerSection(const std::string& name)
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    records_[name]; // inserts a default-constructed SectionRecord (message thread only).
                    // The ring std::array is value-initialized (all zeros) here, ONCE.
}

void PerformanceProfiler::recordTime(const std::string& name, double timeMs)
{
    // C-2 FIX: Use SpinLock with tryEnter so the audio thread never blocks.
    // If the reader thread holds the lock, we skip this measurement rather
    // than risking priority inversion or a system call (kernel transition).
    const juce::SpinLock::ScopedTryLockType lock(statsSpinLock_);
    if (lock.isLocked())
        updateStats(name, timeMs);
}

ProfileStats PerformanceProfiler::getStats(const std::string& name) const
{
    // Reader methods run on message thread only — SpinLock is fine here.
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    auto it = records_.find(name);
    if (it != records_.end())
    {
        return computeStats(it->second);
    }
    return ProfileStats{};
}

std::unordered_map<std::string, ProfileStats> PerformanceProfiler::getAllStats() const
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    std::unordered_map<std::string, ProfileStats> out;
    out.reserve(records_.size());
    for (const auto& [name, record] : records_)
        out.emplace(name, computeStats(record));
    return out;
}

void PerformanceProfiler::reset()
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    records_.clear();
}

void PerformanceProfiler::reset(const std::string& name)
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    records_.erase(name);
}

// ── Private Methods ──────────────────────────────────────────────────────

void PerformanceProfiler::updateStats(const std::string& name, double timeMs)
{
    // C16 FIX: never use operator[] on the audio thread — it allocates on first
    // insert. Use find() so we only update pre-registered sections.
    auto it = records_.find(name);
    jassert(it != records_.end()); // section must be registered from the message thread
    if (it == records_.end())
        return;

    auto& record = it->second;
    auto& stat = record.stats;

    if (stat.callCount == 0)
    {
        // First measurement
        stat.minTimeMs = timeMs;
        stat.maxTimeMs = timeMs;
    }
    else
    {
        // Update min/max
        stat.minTimeMs = std::min(stat.minTimeMs, timeMs);
        stat.maxTimeMs = std::max(stat.maxTimeMs, timeMs);
    }

    // Update running totals
    stat.callCount++;
    stat.totalTimeMs += timeMs;
    stat.averageTimeMs = stat.totalTimeMs / static_cast<double>(stat.callCount);

    // AUDIT-FIX (M4): push into the trailing ring for percentile computation.
    // Fixed-index write + masked advance — no allocation, O(1). If the try-lock
    // in recordTime() failed we wouldn't be here, so this is the only writer.
    record.ring[record.ringHead] = timeMs;
    record.ringHead = (record.ringHead + 1) & (kRingSamples - 1); // power-of-two mask
    if (record.ringCount < kRingSamples)
        ++record.ringCount;
}

ProfileStats PerformanceProfiler::computeStats(const SectionRecord& record) const
{
    // Message-thread only (caller holds the blocking lock). Allocation of a
    // sort scratch here is fine — never runs on the audio thread.
    ProfileStats out = record.stats;

    const std::size_t n = record.ringCount;
    if (n == 0)
    {
        out.p50Ms = out.p95Ms = out.p99Ms = 0.0;
        return out;
    }

    std::vector<double> window(record.ring.begin(), record.ring.begin() + static_cast<std::ptrdiff_t>(n));
    std::sort(window.begin(), window.end());

    // Nearest-rank percentiles (inclusive). p50 is the median. For n==1 all
    // three collapse to the single sample, which is the correct "we only have
    // one observation" answer.
    const auto pct = [&](double q) -> double {
        if (n == 1)
            return window.front();
        // Clamp rank to [1, n]; nearest-rank uses ceil(q/100 * n).
        std::size_t rank = static_cast<std::size_t>(std::ceil((q / 100.0) * static_cast<double>(n)));
        if (rank < 1)
            rank = 1;
        if (rank > n)
            rank = n;
        return window[rank - 1];
    };

    out.p50Ms = pct(50.0);
    out.p95Ms = pct(95.0);
    out.p99Ms = pct(99.0);
    return out;
}

} // namespace more_phi
