// src/AI/Agents/Scheduler/PriorityScheduler.h
#pragma once

#include "AI/Agents/IAgent.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace more_phi::agents {

// Message-thread-domain priority queue + worker pool. NEVER used from the audio thread.
class PriorityScheduler
{
public:
    PriorityScheduler();
    ~PriorityScheduler();

    void start(unsigned numWorkers = 2);
    void stop();

    void submit(std::function<void()> task, TaskPriority priority);

    // Observability
    struct Stats
    {
        int depthBackground = 0;
        int depthNormal = 0;
        int depthHigh = 0;
        int depthRealtimeCritical = 0;
        long long executed = 0;
        long long starvationBumps = 0;
    };
    Stats stats() const;

private:
    void workerLoop();
    void bumpStarvingBackground();

    struct Entry
    {
        std::function<void()> task;
        TaskPriority priority;
        juce::int64 submitTimeMs = 0;   // for starvation detection
    };
    struct EntryCompare
    {
        bool operator()(const Entry& a, const Entry& b) const
        {
            return static_cast<int>(a.priority) < static_cast<int>(b.priority); // higher enum value = higher prio
        }
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Entry, std::vector<Entry>, EntryCompare> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<long long> executed_{0};
    std::atomic<long long> starvationBumps_{0};

    long long starvationGuardMs_ = 5000;
};

} // namespace more_phi::agents
