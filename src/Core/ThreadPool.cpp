/*
 * MorphSnap — Core/ThreadPool.cpp
 * Implementation of thread pool for parallel task execution.
 */
#include "ThreadPool.h"

namespace morphsnap {

ThreadPool::ThreadPool(size_t threads) {
    if (threads == 0) {
        throw std::invalid_argument("ThreadPool requires at least one thread");
    }

    // Reserve space for worker threads
    workers.reserve(threads);

    // Create worker threads
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerThread() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            // Wait for a task or stop signal
            condition.wait(lock, [this] { return stop.load() || !tasks.empty(); });

            // Exit if stopping and no tasks left
            if (stop.load() && tasks.empty()) {
                return;
            }

            // Get next task
            if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
            } else {
                continue; // Spurious wakeup
            }
        }

        // Execute task outside of lock
        try {
            task();
        } catch (...) {
            // Task exceptions are propagated through std::future
            // No need to handle here - just continue processing
        }

        // Decrement active task count and notify waiters
        size_t remaining = active_tasks.fetch_sub(1) - 1;
        if (remaining == 0) {
            finished_condition.notify_all();
        }
    }
}

void ThreadPool::waitForAll() {
    std::unique_lock<std::mutex> lock(queue_mutex);

    // Wait until no active tasks and queue is empty
    finished_condition.wait(lock, [this] {
        return active_tasks.load() == 0 && tasks.empty();
    });
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // Set stop flag
        stop.store(true);
    }

    // Wake up all worker threads
    condition.notify_all();

    // Join all worker threads
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace morphsnap