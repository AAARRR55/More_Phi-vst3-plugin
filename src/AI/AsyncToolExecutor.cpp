/*
 * More-Phi — AI/AsyncToolExecutor.cpp
 * Background execution and polling for long-running MCP tools.
 */
#include "AsyncToolExecutor.h"
#include <thread>

namespace more_phi {

juce::String AsyncToolExecutor::submit(const juce::String& toolName,
                                       std::function<nlohmann::json()> work,
                                       const juce::String& instancePrefix)
{
    // Periodic housekeeping: reap finished jobs past their TTL so the table
    // cannot grow without bound across a long-lived session.
    prune(std::chrono::seconds(300));

    const auto idNumber = nextId_.fetch_add(1, std::memory_order_relaxed);
    // B1 FIX: namespace the job ID with the submitting instance's prefix so
    // cross-instance job enumeration (async_1, async_2, ...) cannot leak
    // another instance's job status/result. Empty prefix preserves the legacy
    // bare-counter ID for backward compatibility (e.g. legacy tests).
    juce::String jobId = (instancePrefix.isNotEmpty() ? (instancePrefix + "-") : juce::String{})
                       + "async_" + juce::String(static_cast<juce::uint64>(idNumber));

    bool spawn = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Enforce cap by evicting oldest completed/failed jobs first.
        while (jobs_.size() >= maxJobs_ && !jobs_.empty())
        {
            auto oldestCompleted = jobs_.end();
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
            {
                if (it->second.status == "completed" || it->second.status == "failed")
                {
                    if (oldestCompleted == jobs_.end() ||
                        it->second.createdAt < oldestCompleted->second.createdAt)
                    {
                        oldestCompleted = it;
                    }
                }
            }
            if (oldestCompleted != jobs_.end())
                jobs_.erase(oldestCompleted);
            else
                break; // No completed jobs to evict.
        }

        const auto now = std::chrono::steady_clock::now();
        Job job;
        job.id = jobId;
        job.toolName = toolName;
        job.createdAt = now;

        if (jobs_.size() >= maxJobs_)
        {
            // At capacity with no evictable (finished) jobs: reject instead of
            // allowing the table to grow without bound while every slot runs.
            job.status = "failed";
            job.errorMessage = "queue_full";
            job.completedAt = now;
            job.result = nlohmann::json{ { "success", false }, { "error", "queue_full" } };
            spawn = false;
        }
        else
        {
            job.status = "queued";
        }

        jobs_[jobId.toStdString()] = std::move(job);
    }

    if (spawn)
    {
        try
        {
            std::thread([this, jobId, work = std::move(work)]() mutable {
                runJob(jobId, std::move(work));
            }).detach();
        }
        catch (...)
        {
            // Thread launch failed: surface a deterministic failure so callers
            // polling status do not hang waiting for a job that will never run.
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = jobs_.find(jobId.toStdString());
            if (it != jobs_.end())
            {
                it->second.status = "failed";
                it->second.errorMessage = "thread_launch_failed";
                it->second.completedAt = std::chrono::steady_clock::now();
                it->second.result = nlohmann::json{ { "success", false },
                                                    { "error", "thread_launch_failed" } };
            }
        }
    }

    return jobId;
}

void AsyncToolExecutor::runJob(const juce::String& jobId, std::function<nlohmann::json()> work)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(jobId.toStdString());
        if (it != jobs_.end())
            it->second.status = "running";
    }

    nlohmann::json outcome;
    try
    {
        outcome = work();
    }
    catch (const std::exception& e)
    {
        outcome = nlohmann::json{{"success", false}, {"error", "exception"}, {"details", e.what()}};
    }
    catch (...)
    {
        outcome = nlohmann::json{{"success", false}, {"error", "unknown_exception"}};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(jobId.toStdString());
    if (it != jobs_.end())
    {
        it->second.result = outcome;
        it->second.completedAt = std::chrono::steady_clock::now();
        const bool success = outcome.is_object() && outcome.value("success", false);
        it->second.status = success ? "completed" : "failed";
        if (!success && outcome.contains("error") && outcome["error"].is_string())
            it->second.errorMessage = juce::String(outcome["error"].get<std::string>());
    }
}

nlohmann::json AsyncToolExecutor::status(const juce::String& jobId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId.toStdString());
    if (it == jobs_.end())
        return nlohmann::json{{"success", false}, {"error", "job_not_found"}};

    const auto& job = it->second;
    nlohmann::json j{
        {"success", true},
        {"job_id", job.id.toStdString()},
        {"tool", job.toolName.toStdString()},
        {"status", job.status.toStdString()},
        {"created_at_ms", static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                job.createdAt.time_since_epoch()).count())}
    };
    if (job.status == "failed" && job.errorMessage.isNotEmpty())
        j["error"] = job.errorMessage.toStdString();
    return j;
}

nlohmann::json AsyncToolExecutor::result(const juce::String& jobId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId.toStdString());
    if (it == jobs_.end())
        return nlohmann::json{{"success", false}, {"error", "job_not_found"}};

    const auto& job = it->second;
    if (job.status != "completed" && job.status != "failed")
    {
        nlohmann::json j{
            {"success", true},
            {"job_id", job.id.toStdString()},
            {"tool", job.toolName.toStdString()},
            {"status", job.status.toStdString()},
            {"result_available", false},
            {"created_at_ms", static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    job.createdAt.time_since_epoch()).count())}
        };
        if (job.status == "failed" && job.errorMessage.isNotEmpty())
            j["error"] = job.errorMessage.toStdString();
        return j;
    }

    nlohmann::json j = job.result;
    j["job_id"] = job.id.toStdString();
    j["status"] = job.status.toStdString();
    j["result_available"] = true;
    return j;
}

void AsyncToolExecutor::prune(std::chrono::seconds ttl)
{
    const auto cutoff = std::chrono::steady_clock::now() - ttl;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end();)
    {
        if ((it->second.status == "completed" || it->second.status == "failed") &&
            it->second.completedAt < cutoff)
        {
            it = jobs_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void AsyncToolExecutor::setMaxJobs(size_t maxJobs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    maxJobs_ = maxJobs;
}

} // namespace more_phi
