/*
 * MorphSnap — Core/ThreadPool.h
 * Thread pool infrastructure for parallel task execution.
 *
 * Features:
 * - Configurable worker thread count
 * - Thread-safe task queue with condition variables
 * - Template enqueue method supporting any callable with return value
 * - std::future-based result retrieval
 * - Graceful shutdown with waitForAll() synchronization
 */
#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <atomic>
#include <type_traits>

namespace morphsnap {

/**
 * ThreadPool provides a pool of worker threads for parallel task execution.
 *
 * Usage:
 *   ThreadPool pool(4);  // 4 worker threads
 *   auto future = pool.enqueue([]() { return 42; });
 *   int result = future.get();
 *   pool.waitForAll();
 *   pool.shutdown();
 */
class ThreadPool {
public:
    /**
     * Create ThreadPool with specified number of worker threads.
     * @param threads Number of worker threads to create (must be > 0)
     * @throws std::invalid_argument if threads == 0
     */
    explicit ThreadPool(size_t threads);

    /**
     * Destructor - calls shutdown() if not already called
     */
    ~ThreadPool();

    // Non-copyable and non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * Enqueue a task for execution by worker threads.
     *
     * @tparam F Callable type
     * @tparam Args Argument types
     * @param f Function/lambda to execute
     * @param args Arguments to pass to function
     * @return std::future<return_type> Future for retrieving result
     * @throws std::runtime_error if ThreadPool has been shut down
     */
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * Wait for all currently queued and executing tasks to complete.
     * Does not prevent new tasks from being enqueued.
     */
    void waitForAll();

    /**
     * Gracefully shutdown the ThreadPool.
     * - Stops accepting new tasks
     * - Waits for all queued tasks to complete
     * - Joins all worker threads
     *
     * Can be called multiple times safely.
     */
    void shutdown();

    /**
     * Get the number of worker threads.
     */
    size_t getNumThreads() const noexcept { return workers.size(); }

    /**
     * Check if ThreadPool has been shut down.
     */
    bool isShutdown() const noexcept { return stop.load(); }

private:
    // Worker threads
    std::vector<std::thread> workers;

    // Task queue
    std::queue<std::function<void()>> tasks;

    // Synchronization primitives
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable finished_condition;

    // Control flags
    std::atomic<bool> stop{false};
    std::atomic<size_t> active_tasks{0};

    // Worker thread function
    void workerThread();
};

// Template implementation
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {

    using return_type = typename std::invoke_result<F, Args...>::type;

    if (stop.load()) {
        throw std::runtime_error("ThreadPool has been shut down");
    }

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // Double-check stop flag under lock
        if (stop.load()) {
            throw std::runtime_error("ThreadPool has been shut down");
        }

        // Increment active task count before enqueuing
        active_tasks.fetch_add(1);

        tasks.emplace([task]() { (*task)(); });
    }

    condition.notify_one();
    return result;
}

} // namespace morphsnap