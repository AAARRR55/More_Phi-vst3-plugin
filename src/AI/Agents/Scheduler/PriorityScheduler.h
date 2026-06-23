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
//
// H-1/M-4 FIX: Replaced single std::priority_queue with four per-priority-level
// std::queue<Entry> instances. This eliminates:
//   - O(log n) push/pop overhead from the heap
//   - O(n log n) starvation-bump scan that held the mutex while draining and
//     rebuilding the entire priority queue
// Starvation promotion is now O(1): splice the Background queue into Normal.
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
    // M4: second-tier escalation. Background promotes to Normal at starvationGuardMs_;
    // under sustained High-priority load Normal would otherwise never run, so after
    // escalationTier2Ms_ a Normal task promotes to High (and High to RealtimeCritical)
    // so memory/telemetry bookkeeping eventually executes instead of waiting forever.
    void escalateStarving();

    struct Entry
    {
        std::function<void()> task;
        TaskPriority priority;
        juce::int64 submitTimeMs = 0;   // for starvation detection

        // Priority ordinal for queue selection (higher = more urgent)
        static constexpr int ordinal(TaskPriority p)
        {
            switch (p)
            {
                case TaskPriority::RealtimeCritical: return 3;
                case TaskPriority::High:             return 2;
                case TaskPriority::Normal:           return 1;
                case TaskPriority::Background:       return 0;
            }
            return 0;
        }
    };

    // H-1/M-4: Per-priority-level queues. Indexed by Entry::ordinal().
    // Background=0, Normal=1, High=2, RealtimeCritical=3.
    static constexpr int kNumPriorityLevels = 4;
    std::queue<Entry> queues_[kNumPriorityLevels];

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<long long> executed_{0};
    std::atomic<long long> starvationBumps_{0};

    long long starvationGuardMs_ = 1000;  // M-3 FIX: reduced from 5000ms to 1000ms
    // M4: Normal→High (and High→RealtimeCritical) promotion threshold. 5x the
    // base guard means a starved task is guaranteed to run within ~5s even when
    // High-priority traffic is continuous.
    long long escalationTier2Ms_ = 5000;
};

} // namespace more_phi::agents
