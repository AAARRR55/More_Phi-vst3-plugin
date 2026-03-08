#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace morphsnap {

/**
 * SystemLoadLevel — drives adaptive throttling in the pipeline.
 */
enum class SystemLoadLevel
{
    LOW,       // < 30% CPU — full speed
    MEDIUM,    // 30-70% — normal
    HIGH,      // 70-90% — throttle capture rate 50%
    CRITICAL   // > 90% — throttle capture rate 75%, reduce batch size
};

/**
 * Snapshot of system resource usage.
 */
struct ResourceSnapshot
{
    float cpuUsagePercent   = 0.0f;  // Overall CPU usage [0, 100]
    uint64_t memUsedBytes   = 0;
    uint64_t memTotalBytes  = 0;
    float memUsagePercent   = 0.0f;  // [0, 100]
    float queueFillRatio    = 0.0f;  // [0, 1]
    SystemLoadLevel level   = SystemLoadLevel::LOW;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * ResourceMonitor — Periodically samples CPU, RAM, and queue metrics.
 *
 * Publishes a SystemLoadLevel that other pipeline components (e.g.,
 * DispatchThread, CaptureStage) can query to adapt their throughput.
 */
class ResourceMonitor
{
public:
    using QueueFillFn = std::function<float()>;

    /**
     * @param pollInterval How often to sample system metrics.
     * @param memLimitBytes Optional hard memory limit (0 = no limit).
     */
    explicit ResourceMonitor(
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(1000),
        uint64_t memLimitBytes = 0)
        : pollInterval_(pollInterval), memLimitBytes_(memLimitBytes) {}

    ~ResourceMonitor() { stop(); }

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    /** Register function that returns queue fill ratio [0,1]. */
    void setQueueFillProvider(QueueFillFn fn)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        queueFillFn_ = std::move(fn);
    }

    void start()
    {
        if (running_.load(std::memory_order_relaxed))
            return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { run(); });
    }

    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel))
            return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cv_.notify_all();
        }
        if (thread_.joinable())
            thread_.join();
    }

    /** Get the latest snapshot (lock-free read of atomic level). */
    ResourceSnapshot getSnapshot() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return latestSnapshot_;
    }

    /** Quick check of current load level (lock-free). */
    SystemLoadLevel getLoadLevel() const noexcept
    {
        return loadLevel_.load(std::memory_order_relaxed);
    }

    bool isRunning() const noexcept
    {
        return running_.load(std::memory_order_relaxed);
    }

private:
    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            sample();
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait_for(lk, pollInterval_, [this]() {
                return !running_.load(std::memory_order_relaxed);
            });
        }
    }

    void sample()
    {
        ResourceSnapshot snap;
        snap.timestamp = std::chrono::steady_clock::now();

        // --- CPU usage (Windows) ---
#ifdef _WIN32
        FILETIME idleTime, kernelTime, userTime;
        if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
        {
            auto toU64 = [](const FILETIME& ft) -> uint64_t {
                return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            uint64_t idle   = toU64(idleTime);
            uint64_t kernel = toU64(kernelTime);
            uint64_t user   = toU64(userTime);

            uint64_t totalDelta = (kernel + user) - (prevKernel_ + prevUser_);
            uint64_t idleDelta  = idle - prevIdle_;

            if (totalDelta > 0)
                snap.cpuUsagePercent = 100.0f * (1.0f - static_cast<float>(idleDelta) / static_cast<float>(totalDelta));

            prevIdle_   = idle;
            prevKernel_ = kernel;
            prevUser_   = user;
        }

        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            snap.memTotalBytes  = memStatus.ullTotalPhys;
            snap.memUsedBytes   = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
            snap.memUsagePercent = 100.0f * static_cast<float>(snap.memUsedBytes)
                                          / static_cast<float>(snap.memTotalBytes);
        }
#else
        // Fallback: zero values — platform-specific implementations can be added
        snap.cpuUsagePercent = 0.0f;
        snap.memUsagePercent = 0.0f;
#endif

        // --- Queue fill ---
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (queueFillFn_)
                snap.queueFillRatio = queueFillFn_();
        }

        // --- Classify load level ---
        float composite = snap.cpuUsagePercent * 0.6f
                        + snap.memUsagePercent * 0.3f
                        + snap.queueFillRatio * 100.0f * 0.1f;

        if (composite > 90.0f)
            snap.level = SystemLoadLevel::CRITICAL;
        else if (composite > 70.0f)
            snap.level = SystemLoadLevel::HIGH;
        else if (composite > 30.0f)
            snap.level = SystemLoadLevel::MEDIUM;
        else
            snap.level = SystemLoadLevel::LOW;

        // Publish
        {
            std::lock_guard<std::mutex> lk(mutex_);
            latestSnapshot_ = snap;
        }
        loadLevel_.store(snap.level, std::memory_order_release);
    }

    std::chrono::milliseconds pollInterval_;
    uint64_t memLimitBytes_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;

    std::atomic<bool> running_{false};
    std::atomic<SystemLoadLevel> loadLevel_{SystemLoadLevel::LOW};
    ResourceSnapshot latestSnapshot_;
    QueueFillFn queueFillFn_;

    // CPU measurement state (Windows)
    uint64_t prevIdle_   = 0;
    uint64_t prevKernel_ = 0;
    uint64_t prevUser_   = 0;
};

} // namespace morphsnap
