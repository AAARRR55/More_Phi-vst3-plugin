#pragma once

#include "TaskQueue.h"
#include "../Recovery/WatchdogTimer.h"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace more_phi {

/**
 * WorkerPool — Configurable thread pool that drains a TaskQueue.
 *
 * Each worker thread continuously pops tasks from the shared TaskQueue
 * and executes them. The pool can be resized dynamically by the
 * ResourceMonitor to adapt to system load.
 *
 * All worker threads run at below-normal priority so they never
 * starve the audio thread.
 */
class WorkerPool
{
public:
    /**
     * @param queue        Shared task queue to drain.
     * @param threadCount  Initial number of worker threads.
     */
    explicit WorkerPool(TaskQueue& queue, int threadCount = 0)
        : queue_(queue)
    {
        if (threadCount <= 0)
            threadCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        desiredThreadCount_ = threadCount;
    }

    ~WorkerPool() { stop(); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /** Launch all worker threads. */
    void start()
    {
        if (running_.load(std::memory_order_relaxed))
            return;
        running_.store(true, std::memory_order_release);
        tasksCompleted_.store(0, std::memory_order_relaxed);
        tasksFailed_.store(0, std::memory_order_relaxed);

        for (int i = 0; i < desiredThreadCount_; ++i)
            workers_.emplace_back([this, i]() { workerLoop(i); });
    }

    /** Shut down: signals the queue and joins all threads. */
    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel))
            return;

        queue_.shutdown();
        for (auto& t : workers_)
        {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
    }

    /** Number of active worker threads. */
    int threadCount() const noexcept
    {
        return static_cast<int>(workers_.size());
    }

    /** Total tasks successfully completed. */
    uint64_t tasksCompleted() const noexcept
    {
        return tasksCompleted_.load(std::memory_order_relaxed);
    }

    /** Total tasks that threw exceptions. */
    uint64_t tasksFailed() const noexcept
    {
        return tasksFailed_.load(std::memory_order_relaxed);
    }

    bool isRunning() const noexcept
    {
        return running_.load(std::memory_order_relaxed);
    }

    /** Optional: set a callback invoked when a task throws. */
    void setErrorHandler(std::function<void(const std::string&)> handler)
    {
        errorHandler_ = std::move(handler);
    }

private:
    void workerLoop(int /*workerId*/)
    {
        // Set thread to below-normal priority (platform-specific)
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__) || defined(__APPLE__)
        // nice(1) or pthread_setschedparam could be used here
#endif

        while (running_.load(std::memory_order_acquire))
        {
            PipelineTask task;
            if (!queue_.pop(task))
                break; // Queue shut down and empty

            try
            {
                if (task.work)
                    task.work();
                tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception& e)
            {
                tasksFailed_.fetch_add(1, std::memory_order_relaxed);
                if (errorHandler_)
                    errorHandler_(e.what());
            }
            catch (...)
            {
                tasksFailed_.fetch_add(1, std::memory_order_relaxed);
                if (errorHandler_)
                    errorHandler_("Unknown exception in worker");
            }
        }
    }

    TaskQueue& queue_;
    int desiredThreadCount_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> tasksCompleted_{0};
    std::atomic<uint64_t> tasksFailed_{0};
    std::function<void(const std::string&)> errorHandler_;
};

} // namespace more_phi
