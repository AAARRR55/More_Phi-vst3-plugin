/*
 * More-Phi — tests/Unit/TestConcurrency.cpp
 *
 * Runtime concurrency and real-time safety tests:
 *   - LockFreeQueue single-producer/single-consumer correctness
 *   - LockFreeQueue multi-producer push serializes safely
 *   - LockFreeQueue capacity boundary
 */

#include <catch2/catch_test_macros.hpp>

#include "Core/LockFreeQueue.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace more_phi;

TEST_CASE("LockFreeQueue push/pop single-producer single-consumer", "[concurrency][lockfree]")
{
    LockFreeQueue<int, 16> queue;

    REQUIRE(queue.empty());

    for (int i = 0; i < 10; ++i)
        REQUIRE(queue.push(i));

    REQUIRE(queue.sizeApprox() == 10);

    for (int i = 0; i < 10; ++i)
    {
        int value = -1;
        REQUIRE(queue.pop(value));
        REQUIRE(value == i);
    }

    REQUIRE(queue.empty());
}

TEST_CASE("LockFreeQueue respects capacity boundary", "[concurrency][lockfree]")
{
    LockFreeQueue<int, 8> queue;  // usable capacity = 7

    for (int i = 0; i < 7; ++i)
        REQUIRE(queue.push(i));

    REQUIRE_FALSE(queue.push(999));

    int popped = -1;
    REQUIRE(queue.pop(popped));
    REQUIRE(popped == 0);

    REQUIRE(queue.push(7));
    REQUIRE(queue.sizeApprox() == 7);
}

TEST_CASE("LockFreeQueue multi-producer push does not corrupt data", "[concurrency][lockfree]")
{
    constexpr int capacity = 1024;
    LockFreeQueue<int, capacity> queue;

    constexpr int producers = 4;
    constexpr int itemsPerProducer = 200;

    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p)
    {
        threads.emplace_back([&queue, p]()
        {
            for (int i = 0; i < itemsPerProducer; ++i)
            {
                int value = p * itemsPerProducer + i;
                while (!queue.push(value))
                {
                    // Spin until space available
                }
            }
        });
    }

    for (auto& t : threads)
        t.join();

    REQUIRE(queue.sizeApprox() == producers * itemsPerProducer);

    std::vector<int> received;
    received.reserve(producers * itemsPerProducer);
    int value = -1;
    while (queue.pop(value))
        received.push_back(value);

    REQUIRE(received.size() == producers * itemsPerProducer);

    std::sort(received.begin(), received.end());
    for (size_t i = 0; i < received.size(); ++i)
        REQUIRE(received[i] == static_cast<int>(i));
}
