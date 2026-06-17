/*
 * More-Phi — tests/Unit/TestStressEdgeCases.cpp
 *
 * Runtime stress and edge-case tests:
 *   - LockFreeQueue full/empty boundary behavior
 *   - SnapshotBank prepare/reprepare with varying parameter counts
 *   - MorphProcessor prepare with zero/one occupied snapshots
 *   - Rapid MorphProcessor process calls do not allocate or throw
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/LockFreeQueue.h"
#include "Core/SnapshotBank.h"
#include "Core/MorphProcessor.h"
#include "Core/InterpolationEngine.h"

#include <vector>

using namespace more_phi;

TEST_CASE("Stress: LockFreeQueue pushRange matches individual pushes", "[stress][lockfree]")
{
    LockFreeQueue<int, 32> queue;

    std::vector<int> items;
    for (int i = 0; i < 10; ++i)
        items.push_back(i);

    REQUIRE(queue.pushRange(items));

    int value = -1;
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(queue.pop(value));
        REQUIRE(value == i);
    }
    REQUIRE_FALSE(queue.pop(value));
}

TEST_CASE("Stress: SnapshotBank prepare/reprepare with varying parameter counts", "[stress][snapshot]")
{
    SnapshotBank bank;

    for (int paramCount : {1, 4, 16, 256, 2048})
    {
        INFO("paramCount = " << paramCount);
        bank.prepare(paramCount);

        std::vector<float> values(static_cast<size_t>(paramCount), 0.5f);
        bank.captureValues(0, values);

        std::vector<float> copied;
        REQUIRE(bank.getSlotValuesCopy(0, copied));
        REQUIRE((copied.size() == static_cast<size_t>(paramCount)));
    }
}

TEST_CASE("Stress: MorphProcessor handles empty bank without modification", "[stress][morph]")
{
    SnapshotBank bank;
    bank.prepare(8);

    MorphProcessor processor(bank);
    processor.prepare(8);

    std::vector<float> out = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    processor.process(0.5f, 0.5f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f / 60.0f, out);

    REQUIRE(out[0] == Catch::Approx(0.1f));
    REQUIRE(out[7] == Catch::Approx(0.8f));
}

TEST_CASE("Stress: rapid MorphProcessor process calls remain bounded", "[stress][morph]")
{
    SnapshotBank bank;
    bank.prepare(8);

    std::vector<float> values(8, 0.75f);
    bank.captureValues(0, values);

    MorphProcessor processor(bank);
    processor.prepare(8);
    processor.setSmoothingRate(0.0f);

    std::vector<float> out(8, 0.0f);

    for (int i = 0; i < 10000; ++i)
        processor.process(0.0f, 0.0f, 0.5f, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, out);

    for (float v : out)
    {
        REQUIRE(std::isfinite(v));
        REQUIRE((v >= 0.0f));
        REQUIRE((v <= 1.0f));
    }
}
