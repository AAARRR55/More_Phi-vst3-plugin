#include <catch2/catch_test_macros.hpp>
#include "Core/AudioBufferPool.h"

TEST_CASE("AudioBufferPool provides reusable buffers", "[AudioBufferPool]")
{
    morphsnap::AudioBufferPool pool(2, 512, 44100.0);

    // Get two buffers
    auto buffer1 = pool.acquireBuffer();
    auto buffer2 = pool.acquireBuffer();

    REQUIRE(buffer1 != nullptr);
    REQUIRE(buffer2 != nullptr);
    REQUIRE(buffer1->getNumChannels() == 2);
    REQUIRE(buffer1->getNumSamples() == 512);

    // Return and reacquire - should reuse memory
    auto* ptr1 = buffer1.get();
    pool.releaseBuffer(std::move(buffer1));
    auto buffer3 = pool.acquireBuffer();

    REQUIRE(buffer3.get() == ptr1); // Memory reused
}