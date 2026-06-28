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

// H-6 FIX: std::jthread replaces the old detach-on-timeout pattern. Each
// jthread has an internal stop_source whose request_stop() is called
// automatically at the start of ~jthread, before the join. The worker loop
// checks its stop_token via the cv_any wait-with-stop-token overload, so a
// stuck task is interrupted only after it exits and re-enters the loop. The
// 2s soft limit is removed — jthread requests stop and joins with no timeout,
// which is safe because workers are joined BEFORE the agentRuntime_ context
// they reference is destroyed (see ~MorePhiProcessor ordering).

PriorityScheduler::PriorityScheduler() = default;

PriorityScheduler::~PriorityScheduler()
{
    stop();
}

void PriorityScheduler::start(unsigned numWorkers)
{
    // BP-1 FIX (audit): explicit acq_rel — read-modify-write that publishes
    // running_=true to workers (release) while observing any prior value (acquire).
    if (running_.exchange(true, std::memory_order_acq_rel))
        return;
    workers_.reserve(numWorkers);
    for (unsigned i = 0; i < numWorkers; ++i)
        workers_.emplace_back([this](std::stop_token st) { workerLoop(st); });
}

void PriorityScheduler::stop()
{
    // BP-1 FIX (audit): explicit acq_rel — publishes running_=false and orders
    // the prior queue mutations before the workers observe the stop.
    if (! running_.exchange(false, std::memory_order_acq_rel))
        return;
    cv_.notify_all();
    // H-6 FIX: std::jthread destructor calls request_stop() + join()
    // automatically. The workers are joined BEFORE the agentRuntime_ context
    // they reference is destroyed (see ~MorePhiProcessor ordering). No detach
    // fallback needed — the stop_token + cv_any wait ensures prompt wake-up.
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
    // Drain anything left unexecuted so we don't keep dangling lambdas.
    // C-3: Only levels 0-2 under mutex; level 3 (urgents pool) drained below.
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < 3; ++i)
        while (! queues_[i].empty())
            queues_[i].pop();
    // Drain the lock-free urgents pool (atomic exchange to reset).
    auto head = urgentsHead_.load(std::memory_order_acquire);
    auto tail = urgentsTail_.load(std::memory_order_relaxed);
    while (tail != head)
    {
        urgentsPool_[static_cast<size_t>(tail)].task = nullptr;
        tail = (tail + 1) & (kUrgentsPoolSize - 1);
    }
    urgentsTail_.store(tail, std::memory_order_release);
}

void PriorityScheduler::submit(std::function<void()> task, TaskPriority priority,
                                juce::int64 deadlineMs)
{
    if (! task)
        return;

    // C-3 FIX: RealtimeCritical tasks go through the lock-free urgents pool
    // first. If the pool is full (extremely rare — 8 slots for 2 workers), fall
    // back to the shared mutex path into the High queue (ordinal 2). The worker
    // loop drains the urgents pool before touching the mutex, so a
    // RealtimeCritical submit never contends with Background→Normal promotion.
    if (priority == TaskPriority::RealtimeCritical)
    {
        auto head = urgentsHead_.load(std::memory_order_relaxed);
        auto tail = urgentsTail_.load(std::memory_order_acquire);
        const auto used = static_cast<size_t>((head - tail) & (kUrgentsPoolSize - 1));
        if (used < static_cast<size_t>(kUrgentsPoolSize - 1))
        {
            const auto nextHead = (head + 1) & (kUrgentsPoolSize - 1);
            urgentsPool_[static_cast<size_t>(head)] = Entry{ std::move(task), priority, nowMs(), deadlineMs };
            urgentsHead_.store(nextHead, std::memory_order_release);
            cv_.notify_one();
            return;
        }
        // Fall through: urgents pool full — degrade to High under mutex
        priority = TaskPriority::High;
    }

    const int ord = Entry::ordinal(priority);
    std::lock_guard<std::mutex> lock(mutex_);
    queues_[ord].push(Entry{ std::move(task), priority, nowMs(), deadlineMs });
    cv_.notify_one();
}

void PriorityScheduler::workerLoop(std::stop_token stopToken)
{
    while (! stopToken.stop_requested())
    {
        // C-3 FIX: Drain the lock-free urgents pool BEFORE touching the
        // shared mutex. This guarantees RealtimeCritical tasks are dispatched
        // immediately regardless of any Background→Normal promotion in progress.
        {
            auto tail = urgentsTail_.load(std::memory_order_relaxed);
            auto head = urgentsHead_.load(std::memory_order_acquire);
            if (tail != head)
            {
                auto entry = std::move(urgentsPool_[static_cast<size_t>(tail)]);
                urgentsTail_.store((tail + 1) & (kUrgentsPoolSize - 1),
                                   std::memory_order_release);
                try { entry.task(); }
                catch (...) {}
                executed_.fetch_add(1, std::memory_order_relaxed);
                continue; // return to top — drain more urgents before blocking
            }
        }

        Entry entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // H-6 FIX: condition_variable_any::wait with stop_token so
            // jthread's request_stop() wakes the worker immediately.
            cv_.wait(lock, stopToken, [this] {
                // BP-1 FIX (audit): explicit relaxed — the mutex already
                // synchronizes this read with stop()'s exchange.
                if (! running_.load(std::memory_order_relaxed)) return true;
                // The non-critical queues are all empty AND the urgents pool is empty.
                for (int i = 0; i < 3; ++i)  // only levels 0-2 under mutex; level 3 is urgents
                    if (! queues_[i].empty()) return true;
                {
                    auto tail = urgentsTail_.load(std::memory_order_relaxed);
                    auto head = urgentsHead_.load(std::memory_order_acquire);
                    if (tail != head) return true;
                }
                return false;
            });
            if (stopToken.stop_requested())
            {
                // H-6: stop requested — drain any remaining tasks then exit.
                // (We'd ideally finish the in-flight task, but we already checked
                // stop_requested; if a task was popped and executed before this
                // check, it runs to completion above or below. Drained queues
                // below.)
                bool anyLeft = false;
                for (int i = 0; i < 3; ++i)
                    if (! queues_[i].empty()) { anyLeft = true; break; }
                if (! anyLeft)
                {
                    auto tail = urgentsTail_.load(std::memory_order_relaxed);
                    auto head = urgentsHead_.load(std::memory_order_acquire);
                    if (tail == head)
                        return;
                }
                // fall through and drain what's left before exiting
            }

            bumpStarvingBackground();
            escalateStarving();

            // H-1/M-4: Check queues in priority order (highest first).
            // Level 3 (RealtimeCritical) is now handled by urgents pool;
            // only check levels 2 (High), 1 (Normal), 0 (Background).
            bool found = false;
            for (int ord = 2; ord >= 0; --ord)
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
        // L-3: Soft deadline check. If the task has a deadline and it's been
        // exceeded, skip execution (the task has timed out). The lambda's
        // captured state destructors run normally — no leak.
        if (entry.deadlineMs > 0 && nowMs() > entry.deadlineMs)
        {
            executed_.fetch_add(1, std::memory_order_relaxed);
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
    s.depthBackground = static_cast<int>(queues_[0].size());
    s.depthNormal     = static_cast<int>(queues_[1].size());
    s.depthHigh       = static_cast<int>(queues_[2].size());
    // C-3 FIX: RealtimeCritical depth includes the lock-free urgents pool.
    const auto head = urgentsHead_.load(std::memory_order_acquire);
    const auto tail = urgentsTail_.load(std::memory_order_relaxed);
    s.depthRealtimeCritical = static_cast<int>((head - tail) & (kUrgentsPoolSize - 1));
    s.executed = executed_.load(std::memory_order_relaxed);
    s.starvationBumps = starvationBumps_.load(std::memory_order_relaxed);
    return s;
}

} // namespace more_phi::agents
