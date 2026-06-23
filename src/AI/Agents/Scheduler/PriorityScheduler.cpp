// src/AI/Agents/Scheduler/PriorityScheduler.cpp
#include "AI/Agents/Scheduler/PriorityScheduler.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept
{
    return juce::Time::currentTimeMillis();
}
} // namespace

PriorityScheduler::PriorityScheduler() = default;

PriorityScheduler::~PriorityScheduler()
{
    stop();
}

void PriorityScheduler::start(unsigned numWorkers)
{
    if (running_.exchange(true))
        return;
    workers_.reserve(numWorkers);
    for (unsigned i = 0; i < numWorkers; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

void PriorityScheduler::stop()
{
    if (! running_.exchange(false))
        return;
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
    // Drain anything left unexecuted so we don't keep dangling lambdas.
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < kNumPriorityLevels; ++i)
        while (! queues_[i].empty())
            queues_[i].pop();
}

void PriorityScheduler::submit(std::function<void()> task, TaskPriority priority)
{
    if (! task)
        return;
    const int ord = Entry::ordinal(priority);
    std::lock_guard<std::mutex> lock(mutex_);
    queues_[ord].push(Entry{ std::move(task), priority, nowMs() });
    cv_.notify_one();
}

void PriorityScheduler::workerLoop()
{
    while (true)
    {
        Entry entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                if (! running_.load()) return true;
                for (int i = 0; i < kNumPriorityLevels; ++i)
                    if (! queues_[i].empty()) return true;
                return false;
            });
            if (! running_.load())
            {
                bool anyLeft = false;
                for (int i = 0; i < kNumPriorityLevels; ++i)
                    if (! queues_[i].empty()) { anyLeft = true; break; }
                if (! anyLeft)
                    return;
            }

            bumpStarvingBackground();
            escalateStarving();

            // H-1/M-4: Check queues in priority order (highest first).
            // O(1) per level — no heap rebuild.
            bool found = false;
            for (int ord = kNumPriorityLevels - 1; ord >= 0; --ord)
            {
                if (! queues_[ord].empty())
                {
                    entry = queues_[ord].front();
                    queues_[ord].pop();
                    found = true;
                    break;
                }
            }
            if (! found)
                continue;
        }
        try
        {
            entry.task();
        }
        catch (...)
        {
            // Agent tasks must not crash the pool. Swallow; agents record their own errors.
        }
        executed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PriorityScheduler::bumpStarvingBackground()
{
    // H-1/M-4 FIX: O(1) starvation promotion — splice the entire Background
    // queue (ordinal 0) into the Normal queue (ordinal 1) when any entry in
    // it has waited longer than starvationGuardMs_. Much cheaper than the
    // previous O(n log n) drain+scan+rebuild under the same lock.
    //
    // Called under mutex_.
    constexpr int kBackgroundOrd = Entry::ordinal(TaskPriority::Background);
    constexpr int kNormalOrd = Entry::ordinal(TaskPriority::Normal);

    if (queues_[kBackgroundOrd].empty() || starvationGuardMs_ <= 0)
        return;

    const auto t = nowMs();
    const auto& oldest = queues_[kBackgroundOrd].front();
    if ((t - oldest.submitTimeMs) <= starvationGuardMs_)
        return;  // nothing starving yet

    // Splice the entire Background queue into Normal, preserving FIFO order.
    // All entries that were Background get promoted to Normal priority.
    while (! queues_[kBackgroundOrd].empty())
    {
        auto e = queues_[kBackgroundOrd].front();
        queues_[kBackgroundOrd].pop();
        e.priority = TaskPriority::Normal;
        queues_[kNormalOrd].push(std::move(e));
    }
    starvationBumps_.fetch_add(1, std::memory_order_relaxed);
}

void PriorityScheduler::escalateStarving()
{
    // M4: second-tier escalation. bumpStarvingBackground() only lifts Background→Normal
    // at starvationGuardMs_. Under sustained High-priority load, Normal (and even High)
    // tasks can sit behind an unbroken stream of more-urgent work and effectively
    // starve. Promote any entry that has waited longer than escalationTier2Ms_ up by
    // one level so memory compaction / telemetry / memory-recall bookkeeping is
    // guaranteed to run within ~5s regardless of High traffic. Called under mutex_.
    if (escalationTier2Ms_ <= 0)
        return;

    const auto t = nowMs();

    // Promote Normal → High for old-enough entries. std::queue is FIFO, so if the
    // FRONT is not yet old enough, nothing behind it can be either (they arrived
    // later). We process as many leading entries as qualify.
    auto drainAged = [&](int fromOrd, int toOrd) {
        if (fromOrd == toOrd || fromOrd < 0 || toOrd >= kNumPriorityLevels)
            return;
        while (! queues_[fromOrd].empty())
        {
            auto& front = queues_[fromOrd].front();
            if ((t - front.submitTimeMs) <= escalationTier2Ms_)
                break;
            auto e = queues_[fromOrd].front();
            queues_[fromOrd].pop();
            e.priority = [toOrd]() -> TaskPriority {
                switch (toOrd)
                {
                    case 3: return TaskPriority::RealtimeCritical;
                    case 2: return TaskPriority::High;
                    case 1: return TaskPriority::Normal;
                    default: return TaskPriority::Background;
                }
            }();
            queues_[toOrd].push(std::move(e));
        }
    };

    constexpr int kNormalOrd          = Entry::ordinal(TaskPriority::Normal);
    constexpr int kHighOrd            = Entry::ordinal(TaskPriority::High);
    constexpr int kRealtimeCriticalOrd= Entry::ordinal(TaskPriority::RealtimeCritical);

    drainAged(kNormalOrd, kHighOrd);
    drainAged(kHighOrd, kRealtimeCriticalOrd);
}

PriorityScheduler::Stats PriorityScheduler::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.depthBackground       = static_cast<int>(queues_[0].size());
    s.depthNormal           = static_cast<int>(queues_[1].size());
    s.depthHigh             = static_cast<int>(queues_[2].size());
    s.depthRealtimeCritical = static_cast<int>(queues_[3].size());
    s.executed = executed_.load(std::memory_order_relaxed);
    s.starvationBumps = starvationBumps_.load(std::memory_order_relaxed);
    return s;
}

} // namespace more_phi::agents
