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
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
    // Drain anything left unexecuted so we don't keep dangling lambdas.
    while (! queue_.empty())
        queue_.pop();
}

void PriorityScheduler::submit(std::function<void()> task, TaskPriority priority)
{
    if (! task)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(Entry{ std::move(task), priority, nowMs() });
    cv_.notify_one();
}

void PriorityScheduler::workerLoop()
{
    while (true)
    {
        Entry entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return ! running_.load() || ! queue_.empty(); });
            if (! running_.load() && queue_.empty())
                return;
            bumpStarvingBackground();
            if (queue_.empty())
                continue;
            entry = queue_.top();
            queue_.pop();
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
    // Called under mutex_. If the oldest Background entry has waited past the guard,
    // promote it to Normal so it can't starve indefinitely under sustained high-prio load.
    if (queue_.empty() || starvationGuardMs_ <= 0)
        return;
    // We cannot mutate std::priority_queue in place; rebuild if needed.
    std::vector<Entry> snapshot;
    snapshot.reserve(queue_.size());
    const auto t = nowMs();
    bool promoted = false;
    while (! queue_.empty())
    {
        Entry e = queue_.top();
        queue_.pop();
        if (e.priority == TaskPriority::Background && (t - e.submitTimeMs) > starvationGuardMs_)
        {
            e.priority = TaskPriority::Normal;
            promoted = true;
        }
        snapshot.push_back(std::move(e));
    }
    for (auto& e : snapshot)
        queue_.push(std::move(e));
    if (promoted)
        starvationBumps_.fetch_add(1, std::memory_order_relaxed);
}

PriorityScheduler::Stats PriorityScheduler::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    auto q = queue_;   // copy; priority_queue only exposes top()/pop()
    while (! q.empty())
    {
        switch (q.top().priority)
        {
            case TaskPriority::Background:        ++s.depthBackground; break;
            case TaskPriority::Normal:            ++s.depthNormal; break;
            case TaskPriority::High:              ++s.depthHigh; break;
            case TaskPriority::RealtimeCritical:  ++s.depthRealtimeCritical; break;
        }
        q.pop();
    }
    s.executed = executed_.load(std::memory_order_relaxed);
    s.starvationBumps = starvationBumps_.load(std::memory_order_relaxed);
    return s;
}

} // namespace more_phi::agents
