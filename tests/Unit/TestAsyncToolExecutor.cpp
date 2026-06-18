/*
 * More-Phi — Unit/TestAsyncToolExecutor.cpp
 *
 * Tests that the async tool executor bounds its retained-job table: it must
 * reject overflow with a deterministic "queue_full" result instead of growing
 * without bound when all slots are occupied by running jobs, and prune() must
 * reap finished jobs past their TTL.
 */
#include <catch2/catch_test_macros.hpp>

#include "AI/AsyncToolExecutor.h"

#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace more_phi;

namespace {
bool status_is(const AsyncToolExecutor& exec, const juce::String& id, const std::string& wanted)
{
    return exec.status(id).value("status", std::string{}) == wanted;
}

bool wait_for_status(const AsyncToolExecutor& exec, const juce::String& id, const std::string& wanted,
                     int timeoutMs = 1000)
{
    for (int i = 0; i < timeoutMs / 5; ++i)
    {
        if (status_is(exec, id, wanted))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status_is(exec, id, wanted);
}
} // namespace

TEST_CASE("AsyncToolExecutor caps growth with queue_full at capacity", "[async-executor]")
{
    AsyncToolExecutor exec;
    exec.setMaxJobs(2);

    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();

    auto blocking = [gate]() -> nlohmann::json {
        gate.wait();
        return nlohmann::json{ { "success", true } };
    };

    const juce::String id1 = exec.submit("t", blocking);
    const juce::String id2 = exec.submit("t", blocking);

    REQUIRE(wait_for_status(exec, id1, "running"));
    REQUIRE(wait_for_status(exec, id2, "running"));

    // Both slots are running -> nothing evictable. The third submit must be
    // rejected deterministically rather than allowed to grow the table.
    const juce::String id3 = exec.submit("t", []() -> nlohmann::json {
        return nlohmann::json{ { "success", true } };
    });
    const auto s3 = exec.status(id3);
    REQUIRE(s3.value("status", std::string{}) == "failed");
    REQUIRE(s3.value("error", std::string{}) == "queue_full");

    release.set_value();

    // Let the running jobs finish before the executor is destroyed so the
    // detached worker threads exit cleanly.
    REQUIRE(wait_for_status(exec, id1, "completed"));
    REQUIRE(wait_for_status(exec, id2, "completed"));
}

TEST_CASE("AsyncToolExecutor prune reaps finished jobs", "[async-executor]")
{
    AsyncToolExecutor exec;
    const juce::String id = exec.submit("t", []() -> nlohmann::json {
        return nlohmann::json{ { "success", true } };
    });

    REQUIRE(wait_for_status(exec, id, "completed"));

    exec.prune(std::chrono::seconds(0));

    REQUIRE(exec.status(id).value("error", std::string{}) == "job_not_found");
}
