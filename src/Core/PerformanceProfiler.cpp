#include "PerformanceProfiler.h"
#include <algorithm>
#include <limits>

namespace morphsnap {

// Timer implementation
PerformanceProfiler::Timer::Timer(PerformanceProfiler& profiler, const std::string& name)
    : profiler_(profiler)
    , name_(name)
    , startTime_(std::chrono::high_resolution_clock::now())
{
}

PerformanceProfiler::Timer::~Timer()
{
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime_);
    double timeMs = duration.count() / 1000.0;
    profiler_.recordTime(name_, timeMs);
}

// PerformanceProfiler implementation
PerformanceProfiler::Timer PerformanceProfiler::createTimer(const std::string& name)
{
    return Timer(*this, name);
}

void PerformanceProfiler::recordTime(const std::string& name, double timeMs)
{
    updateStats(name, timeMs);
}

ProfileStats PerformanceProfiler::getStats(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    auto it = stats_.find(name);
    if (it != stats_.end()) {
        return it->second;
    }
    return ProfileStats{}; // Return default stats if not found
}

std::unordered_map<std::string, ProfileStats> PerformanceProfiler::getAllStats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void PerformanceProfiler::reset()
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.clear();
}

void PerformanceProfiler::reset(const std::string& name)
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.erase(name);
}

void PerformanceProfiler::updateStats(const std::string& name, double timeMs)
{
    std::lock_guard<std::mutex> lock(statsMutex_);

    auto& stat = stats_[name];

    if (stat.callCount == 0) {
        // First measurement
        stat.callCount = 1;
        stat.totalTimeMs = timeMs;
        stat.averageTimeMs = timeMs;
        stat.minTimeMs = timeMs;
        stat.maxTimeMs = timeMs;
    } else {
        // Update existing measurements
        stat.callCount++;
        stat.totalTimeMs += timeMs;
        stat.averageTimeMs = stat.totalTimeMs / stat.callCount;
        stat.minTimeMs = std::min(stat.minTimeMs, timeMs);
        stat.maxTimeMs = std::max(stat.maxTimeMs, timeMs);
    }
}

} // namespace morphsnap