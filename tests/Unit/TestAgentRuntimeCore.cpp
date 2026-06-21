#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "AI/Agents/Scheduler/PriorityScheduler.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

TEST_CASE("PriorityScheduler runs submitted tasks", "[agents][scheduler]")
{
    PriorityScheduler scheduler;
    scheduler.start(2);

    std::atomic<int> counter{0};
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);

    // Spin briefly until both complete or timeout.
    for (int i = 0; i < 200 && counter.load(std::memory_order_relaxed) < 2; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(counter.load(std::memory_order_relaxed) == 2);
    scheduler.stop();
}

TEST_CASE("PriorityScheduler honors priority ordering under single worker", "[agents][scheduler]")
{
    // With one worker and tasks submitted before start, higher priority runs first.
    PriorityScheduler scheduler;

    std::vector<int> order;
    juce::SpinLock orderLock;

    auto record = [&](int tag) {
        juce::SpinLock::ScopedLockType lock(orderLock);
        order.push_back(tag);
    };

    scheduler.submit([&] { record(1); }, TaskPriority::Background);
    scheduler.submit([&] { record(2); }, TaskPriority::High);
    scheduler.submit([&] { record(3); }, TaskPriority::RealtimeCritical);

    scheduler.start(1); // single worker so ordering is deterministic from the queue

    for (int i = 0; i < 200 && order.size() < 3; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(order.size() == 3);
    // RealtimeCritical (3) before High (2) before Background (1)
    REQUIRE(order[0] == 3);
    REQUIRE(order[1] == 2);
    REQUIRE(order[2] == 1);

    scheduler.stop();
}
