/*
 * More-Phi — Core/ThreadPool.cpp
 * Implementation of thread pool for parallel task execution.
 */
#include "ThreadPool.h"
#include <chrono>

namespace more_phi {

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
        {
            // C1 FIX: RAII guard ensures activeTasks_ is decremented even if task throws.
            struct ActiveTaskGuard {
                std::atomic<size_t>& counter;
                explicit ActiveTaskGuard(std::atomic<size_t>& c) : counter(c) {}
                ~ActiveTaskGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
            } guard(activeTasks_);

            try {
                task();
            } catch (...) {
                // Task exceptions are propagated through std::future
                // No need to handle here - just continue processing
            }
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

        // FIX C20: Set stop flag and discard pending tasks so workers don't
        // process them after shutdown. Each discarded task had already
        // incremented activeTasks_, so decrement to keep the counter balanced.
        stop_.store(true);
        const size_t discarded = tasks_.size();
        std::queue<std::function<void()>> empty;
        std::swap(tasks_, empty);
        activeTasks_.fetch_sub(discarded);
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

} // namespace more_phi