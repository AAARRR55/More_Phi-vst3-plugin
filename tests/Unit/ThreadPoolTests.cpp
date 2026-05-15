#include <catch2/catch_test_macros.hpp>
#include "Core/ThreadPool.h"
#include <chrono>
#include <atomic>
#include <thread>

TEST_CASE("ThreadPool executes tasks concurrently", "[ThreadPool]")
{
    more_phi::ThreadPool pool(4);
    std::atomic<int> counter{0};

    // Submit 8 tasks that increment counter
    for (int i = 0; i < 8; ++i)
    {
        pool.enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter.fetch_add(1);
        });
    }

    pool.waitForAll();
    REQUIRE(counter.load() == 8);
}