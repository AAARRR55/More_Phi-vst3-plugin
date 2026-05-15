/*
 * More-Phi — Core/PerformanceProfiler.cpp
 *
 * Performance profiling implementation with RAII timers and thread-safe statistics tracking.
 */

#include "PerformanceProfiler.h"
#include <algorithm>
#include <cmath>

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
    auto it = stats_.find(name);
    if (it != stats_.end())
    {
        return it->second;
    }
    return ProfileStats{};
}

std::unordered_map<std::string, ProfileStats> PerformanceProfiler::getAllStats() const
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    return stats_;
}

void PerformanceProfiler::reset()
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    stats_.clear();
}

void PerformanceProfiler::reset(const std::string& name)
{
    const juce::SpinLock::ScopedLockType lock(statsSpinLock_);
    stats_.erase(name);
}

// ── Private Methods ──────────────────────────────────────────────────────

void PerformanceProfiler::updateStats(const std::string& name, double timeMs)
{
    auto& stat = stats_[name];

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
    stat.averageTimeMs = stat.totalTimeMs / stat.callCount;
}

} // namespace more_phi
