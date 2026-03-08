/*
 * MorphSnap — Core/ThreadPool.cpp
 * Implementation of thread pool for parallel task execution.
 */
#include "ThreadPool.h"
#include <chrono>

namespace morphsnap {

ThreadPool::ThreadPool(size_t threads) {
    // Reserve space for worker threads
    workers_.reserve(threads);

    // Create worker threads
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerThread() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);

            // Wait for a task or stop signal
            condition_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });

            // Exit if stopping and no tasks left
            if (stop_.load() && tasks_.empty()) {
                return;
            }

            // Get next task
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
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
    }
}

void ThreadPool::waitForAll() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (activeTasks_.load() == 0 && tasks_.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);

        // Set stop flag
        stop_.store(true);
    }

    // Wake up all worker threads
    condition_.notify_all();

    // Join all worker threads
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

size_t ThreadPool::getNumThreads() const {
    return workers_.size();
}

} // namespace morphsnap