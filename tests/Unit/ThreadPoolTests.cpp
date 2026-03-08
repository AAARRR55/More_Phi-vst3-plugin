#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Core/ThreadPool.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace morphsnap;

TEST_CASE("ThreadPool creates correct number of worker threads", "[ThreadPool]") {
    ThreadPool pool(4);

    // Allow some time for threads to start up
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // ThreadPool should be ready to accept tasks
    REQUIRE_NOTHROW(pool.waitForAll());
}

TEST_CASE("ThreadPool executes tasks concurrently", "[ThreadPool]") {
    const int numWorkers = 4;
    const int numTasks = 8;
    ThreadPool pool(numWorkers);

    std::atomic<int> counter{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> currentConcurrent{0};

    std::vector<std::future<void>> futures;

    for (int i = 0; i < numTasks; ++i) {
        auto future = pool.enqueue([&counter, &maxConcurrent, &currentConcurrent]() {
            // Track concurrent execution
            int current = currentConcurrent.fetch_add(1) + 1;
            int expected = maxConcurrent.load();
            while (expected < current && !maxConcurrent.compare_exchange_weak(expected, current)) {
                // Try again
            }

            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Increment counter
            counter.fetch_add(1);

            currentConcurrent.fetch_sub(1);
        });
        futures.push_back(std::move(future));
    }

    // Wait for all tasks to complete
    pool.waitForAll();

    // Verify all tasks completed
    REQUIRE(counter.load() == numTasks);

    // Verify concurrent execution occurred (should be > 1 for 4 workers and 8 tasks)
    REQUIRE(maxConcurrent.load() > 1);
    REQUIRE(maxConcurrent.load() <= numWorkers);
}

TEST_CASE("ThreadPool enqueue returns std::future with return value", "[ThreadPool]") {
    ThreadPool pool(2);

    auto future1 = pool.enqueue([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 42;
    });

    auto future2 = pool.enqueue([]() -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return "Hello ThreadPool";
    });

    REQUIRE(future1.get() == 42);
    REQUIRE(future2.get() == "Hello ThreadPool");
}

TEST_CASE("ThreadPool handles exception in tasks", "[ThreadPool]") {
    ThreadPool pool(2);

    auto future = pool.enqueue([]() -> int {
        throw std::runtime_error("Task exception");
        return 42;
    });

    REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("ThreadPool shutdown stops all workers", "[ThreadPool]") {
    ThreadPool pool(4);

    std::atomic<bool> taskStarted{false};
    std::atomic<bool> shouldContinue{true};

    // Enqueue a long-running task
    auto future = pool.enqueue([&taskStarted, &shouldContinue]() {
        taskStarted = true;
        while (shouldContinue.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Wait for task to start
    while (!taskStarted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Signal task to complete and shutdown
    shouldContinue = false;

    // Wait for task completion and then shutdown
    future.get();
    REQUIRE_NOTHROW(pool.shutdown());
}

TEST_CASE("ThreadPool waitForAll blocks until all tasks complete", "[ThreadPool]") {
    ThreadPool pool(2);

    std::atomic<int> completedTasks{0};
    const int numTasks = 4;

    // Enqueue tasks with different durations
    for (int i = 0; i < numTasks; ++i) {
        pool.enqueue([&completedTasks, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5 + i * 2));
            completedTasks.fetch_add(1);
        });
    }

    // waitForAll should block until all tasks complete
    pool.waitForAll();

    // All tasks should be completed
    REQUIRE(completedTasks.load() == numTasks);
}