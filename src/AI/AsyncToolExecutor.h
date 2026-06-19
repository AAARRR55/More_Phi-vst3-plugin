/*
 * More-Phi — AI/AsyncToolExecutor.h
 * Runs long-running MCP tools on background threads and allows clients to
 * poll status/result by job ID.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace more_phi {

class AsyncToolExecutor
{
public:
    struct Job
    {
        juce::String id;
        juce::String toolName;
        juce::String status;          // "queued", "running", "completed", "failed"
        juce::String errorMessage;
        nlohmann::json result = nlohmann::json::object();
        std::chrono::steady_clock::time_point createdAt;
        std::chrono::steady_clock::time_point completedAt;
    };

    /** Submit work and return the assigned job ID immediately.
     *
     *  B1 FIX (2026-06-19 audit): @p instancePrefix namespaces the job ID so a
     *  process-wide shared executor cannot leak one plugin instance's job
     *  status/result to another. Without it, job IDs were a bare global counter
     *  (async_1, async_2, ...) that any connected MCP client could enumerate to
     *  poll another instance's async jobs. The prefix is folded into both the
     *  generated ID and the internal lookup key.
     */
    juce::String submit(const juce::String& toolName,
                        std::function<nlohmann::json()> work,
                        const juce::String& instancePrefix = {});

    /** Return the current job status (read-only snapshot). */
    nlohmann::json status(const juce::String& jobId) const;

    /** Return the final result if completed; otherwise status-only. */
    nlohmann::json result(const juce::String& jobId) const;

    /** Remove completed/failed jobs older than the TTL. */
    void prune(std::chrono::seconds ttl = std::chrono::seconds(300));

    /** Cap the number of retained jobs. */
    void setMaxJobs(size_t maxJobs);

private:
    void runJob(const juce::String& jobId, std::function<nlohmann::json()> work);

    mutable std::mutex mutex_;
    std::map<std::string, Job> jobs_;
    std::atomic<uint64_t> nextId_{1};
    size_t maxJobs_ = 64;
};

} // namespace more_phi
