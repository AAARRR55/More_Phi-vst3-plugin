#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace more_phi {

/**
 * Priority levels for pipeline tasks.
 */
enum class TaskPriority : int
{
    LOW    = 0,  // Metadata, logging
    NORMAL = 1,  // Regular batch processing
    HIGH   = 2   // Validation retries, urgent work
};

/**
 * A single unit of work submitted to the pipeline worker pool.
 */
struct PipelineTask
{
    using WorkFn = std::function<void()>;

    WorkFn       work;
    TaskPriority priority = TaskPriority::NORMAL;
    uint64_t     batchId  = 0;

    bool operator<(const PipelineTask& other) const noexcept
    {
        // std::priority_queue is a max-heap, so higher priority comes first.
        return priority < other.priority;
    }
};

/**
 * TaskQueue — Thread-safe, priority-aware MPMC task queue.
 *
 * Multiple producer threads can enqueue work (e.g., the DispatchThread);
 * multiple consumer threads (WorkerPool members) can dequeue.
 * Tasks are dequeued in priority order (HIGH first).
 *
 * Supports bounded capacity with backpressure: tryPush() returns false
 * when the queue is full, allowing callers to throttle.
 */
class TaskQueue
{
public:
    explicit TaskQueue(size_t maxCapacity = 4096)
        : maxCapacity_(maxCapacity) {}

    ~TaskQueue() { shutdown(); }

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    /**
     * Enqueue a task. Blocks if the queue is full.
     * @return false if the queue has been shut down.
     */
    bool push(PipelineTask task)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        notFull_.wait(lk, [this]() {
            return queue_.size() < maxCapacity_ || shutdown_.load(std::memory_order_relaxed);
        });
        if (shutdown_.load(std::memory_order_relaxed))
            return false;

        queue_.push(std::move(task));
        lk.unlock();
        notEmpty_.notify_one();
        return true;
    }

    /**
     * Try to enqueue without blocking.
     * @return false if queue is full or shut down.
     */
    bool tryPush(PipelineTask task)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_.load(std::memory_order_relaxed) || queue_.size() >= maxCapacity_)
            return false;

        queue_.push(std::move(task));
        notEmpty_.notify_one();
        return true;
    }

    /**
     * Dequeue a task. Blocks until a task is available or shutdown is called.
     * @return false if the queue is shut down and empty.
     */
    bool pop(PipelineTask& task)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        notEmpty_.wait(lk, [this]() {
            return !queue_.empty() || shutdown_.load(std::memory_order_relaxed);
        });
        if (queue_.empty())
            return false;

        task = std::move(const_cast<PipelineTask&>(queue_.top()));
        queue_.pop();
        lk.unlock();
        notFull_.notify_one();
        return true;
    }

    /** Signal all waiting threads to exit. */
    void shutdown()
    {
        shutdown_.store(true, std::memory_order_release);
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    /** Reset after a shutdown so the queue can be reused. */
    void reset()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        shutdown_.store(false, std::memory_order_release);
        // Drain any leftover tasks
        while (!queue_.empty())
            queue_.pop();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    size_t capacity() const noexcept { return maxCapacity_; }

    float fillRatio() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return maxCapacity_ > 0
            ? static_cast<float>(queue_.size()) / static_cast<float>(maxCapacity_)
            : 0.0f;
    }

    bool isShutdown() const noexcept
    {
        return shutdown_.load(std::memory_order_relaxed);
    }

private:
    size_t maxCapacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::priority_queue<PipelineTask> queue_;
    std::atomic<bool> shutdown_{false};
};

} // namespace more_phi
