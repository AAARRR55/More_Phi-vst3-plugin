#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>

namespace morphsnap {

/**
 * WatchdogTimer — Detects and recovers from hung worker threads.
 *
 * The watchdog runs on its own thread and monitors a "heartbeat" counter.
 * If the heartbeat is not incremented within the configured timeout,
 * the watchdog fires a callback so the owner can kill/restart the hung task.
 *
 * Usage:
 *   WatchdogTimer wd(std::chrono::milliseconds(500), [](){ LOG("HUNG!"); });
 *   wd.start();
 *   // In worker loop: wd.kick();
 *   wd.stop();
 */
class WatchdogTimer
{
public:
    using Clock     = std::chrono::steady_clock;
    using Duration  = Clock::duration;
    using Callback  = std::function<void()>;

    /**
     * @param timeout   Maximum allowed time between heartbeats.
     * @param onTimeout Callback invoked when the timeout expires.
     */
    explicit WatchdogTimer(Duration timeout, Callback onTimeout)
        : timeout_(timeout), onTimeout_(std::move(onTimeout)) {}

    ~WatchdogTimer() { stop(); }

    // Non-copyable, non-movable (owns a thread)
    WatchdogTimer(const WatchdogTimer&) = delete;
    WatchdogTimer& operator=(const WatchdogTimer&) = delete;

    /** Start the watchdog monitoring thread. */
    void start()
    {
        if (running_.load(std::memory_order_relaxed))
            return;

        running_.store(true, std::memory_order_release);
        lastKick_ = Clock::now();
        thread_ = std::thread([this]() { run(); });
    }

    /** Stop the watchdog thread (blocks until joined). */
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

    /** Call this periodically from the monitored thread to reset the timer. */
    void kick()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        lastKick_ = Clock::now();
        timeoutFired_.store(false, std::memory_order_relaxed);
    }

    /** Returns true if the timeout callback has been fired since the last kick. */
    bool hasTimedOut() const noexcept
    {
        return timeoutFired_.load(std::memory_order_relaxed);
    }

    /** Change the timeout duration (takes effect on the next check cycle). */
    void setTimeout(Duration d)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        timeout_ = d;
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
            std::unique_lock<std::mutex> lk(mutex_);
            // Sleep for half the timeout so we check frequently enough
            auto halfTimeout = timeout_ / 2;
            cv_.wait_for(lk, halfTimeout, [this]() {
                return !running_.load(std::memory_order_relaxed);
            });

            if (!running_.load(std::memory_order_relaxed))
                break;

            auto elapsed = Clock::now() - lastKick_;
            if (elapsed >= timeout_ && !timeoutFired_.load(std::memory_order_relaxed))
            {
                timeoutFired_.store(true, std::memory_order_release);
                lk.unlock(); // Release lock before callback
                if (onTimeout_)
                    onTimeout_();
            }
        }
    }

    Duration timeout_;
    Callback onTimeout_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> timeoutFired_{false};
    Clock::time_point lastKick_;
};

} // namespace morphsnap
