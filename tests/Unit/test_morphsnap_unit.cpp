/*
 * MorphSnap — Unit Tests
 * Tests for the current morphsnap:: API using Catch2 v3.
 *
 * Coverage:
 *   - LockFreeQueue (SPSC semantics, capacity, FIFO order)
 *   - ParameterState (capture, clear, boundary, RT-safety)
 *   - InterpolationEngine (clock positions, 1D/2D geometry)
 *   - SnapshotBank (capture/recall/clear/thread-safety)
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/LockFreeQueue.h"
#include "Core/ParameterState.h"
#include "Core/SnapshotBank.h"
#include "Core/InterpolationEngine.h"

#include <thread>
#include <vector>
#include <numeric>
#include <cstring>

using Catch::Approx;
using namespace morphsnap;

// ─────────────────────────────────────────────────────────────────────────────
//  LockFreeQueue
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LockFreeQueue: empty on construction", "[lockfree]")
{
    LockFreeQueue<int, 16> q;
    int val{};
    REQUIRE_FALSE(q.pop(val));
    REQUIRE(q.empty());
}

TEST_CASE("LockFreeQueue: push and pop single item", "[lockfree]")
{
    LockFreeQueue<int, 8> q;
    REQUIRE(q.push(42));
    REQUIRE_FALSE(q.empty());

    int val{};
    REQUIRE(q.pop(val));
    REQUIRE(val == 42);
    REQUIRE(q.empty());
}

TEST_CASE("LockFreeQueue: FIFO ordering", "[lockfree]")
{
    LockFreeQueue<int, 32> q;
    for (int i = 0; i < 10; ++i)
        REQUIRE(q.push(i));

    for (int i = 0; i < 10; ++i)
    {
        int val{};
        REQUIRE(q.pop(val));
        REQUIRE(val == i);
    }
}

TEST_CASE("LockFreeQueue: capacity = N-1 usable slots (power-of-2 ring)", "[lockfree]")
{
    // Capacity=8 → 7 usable slots (one slot reserved for full/empty distinction)
    LockFreeQueue<int, 8> q;
    int pushed = 0;
    while (q.push(pushed)) ++pushed;
    REQUIRE(pushed == 7);

    // After filling, push must fail
    REQUIRE_FALSE(q.push(999));
}

TEST_CASE("LockFreeQueue: SPSC producer-consumer correctness", "[lockfree][threading]")
{
    LockFreeQueue<int, 1024> q;
    constexpr int N = 500;
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; ++i)
        {
            while (!q.push(i))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        int got = 0;
        while (got < N)
        {
            int v{};
            if (q.pop(v))
            {
                received.push_back(v);
                ++got;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(static_cast<int>(received.size()) == N);
    for (int i = 0; i < N; ++i)
        REQUIRE(received[i] == i);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ParameterState
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ParameterState: default construction is unoccupied", "[param]")
{
    ParameterState s;
    REQUIRE_FALSE(s.occupied);
    REQUIRE(s.parameterCount == 0);
    REQUIRE(s.name[0] == '\0');
}

TEST_CASE("ParameterState: capture stores values and marks occupied", "[param]")
{
    ParameterState s;
    float src[4] = {0.1f, 0.5f, 0.9f, 0.0f};
    s.capture(src, 4);

    REQUIRE(s.occupied);
    REQUIRE(s.parameterCount == 4);
    REQUIRE(s.data()[0] == Approx(0.1f));
    REQUIRE(s.data()[1] == Approx(0.5f));
    REQUIRE(s.data()[2] == Approx(0.9f));
}

TEST_CASE("ParameterState: capture clamps count to MAX_PARAMETERS", "[param]")
{
    ParameterState s;
    // Passing oversized count must not crash and must clamp
    std::vector<float> big(MAX_PARAMETERS + 100, 0.5f);
    s.capture(big.data(), static_cast<int>(big.size()));
    REQUIRE(s.parameterCount == MAX_PARAMETERS);
    REQUIRE(s.occupied);
}

TEST_CASE("ParameterState: clear resets occupied and name", "[param]")
{
    ParameterState s;
    float src[2] = {0.3f, 0.7f};
    s.capture(src, 2);
    s.setName("My Snapshot");
    REQUIRE(s.occupied);

    s.clear();
    REQUIRE_FALSE(s.occupied);
    REQUIRE(s.parameterCount == 0);
    REQUIRE(s.name[0] == '\0');
}

TEST_CASE("ParameterState: setName never overflows fixed buffer", "[param]")
{
    ParameterState s;
    // 200-char name must be safely truncated to 63 chars + null
    std::string longName(200, 'A');
    s.setName(longName.c_str());

    // Buffer is 64 bytes; last byte must be null
    REQUIRE(s.name[63] == '\0');
    REQUIRE(std::strlen(s.name) == 63);
}

TEST_CASE("ParameterState: size() and data() accessors return correct state", "[param]")
{
    ParameterState s;
    float src[3] = {0.2f, 0.4f, 0.8f};
    s.capture(src, 3);

    REQUIRE(s.size() == 3);
    REQUIRE(s.data() == s.values.data());
}

// ─────────────────────────────────────────────────────────────────────────────
//  InterpolationEngine — geometry
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine: getClockPositions returns 12 entries", "[interp]")
{
    auto positions = InterpolationEngine::getClockPositions();
    REQUIRE(positions.size() == 12);
}

TEST_CASE("InterpolationEngine: clock positions lie on the requested radius circle", "[interp]")
{
    constexpr float radius = 0.85f;
    auto positions = InterpolationEngine::getClockPositions(radius);

    for (const auto& p : positions)
    {
        float r = std::sqrt(p.x * p.x + p.y * p.y);
        REQUIRE(r == Approx(radius).margin(1e-5f));
    }
}

TEST_CASE("InterpolationEngine: 12 o'clock position is at top (y negative in JUCE coords)", "[interp]")
{
    // Slot 0 should be at angle -pi/2 → x≈0, y≈-radius
    auto positions = InterpolationEngine::getClockPositions(1.0f);
    REQUIRE(positions[0].x == Approx(0.0f).margin(1e-5f));
    REQUIRE(positions[0].y == Approx(-1.0f).margin(1e-5f));
}

TEST_CASE("InterpolationEngine: SIMD capability reported consistently", "[interp]")
{
    // hasAVXSupport and hasSSESupport just need to not crash;
    // SSE must be available on any x64 target.
    bool sse = InterpolationEngine::hasSSESupport();
    bool avx = InterpolationEngine::hasAVXSupport();

    // AVX implies SSE
    if (avx) REQUIRE(sse);
    (void)sse; (void)avx;  // suppress unused-variable on non-x64
}

TEST_CASE("InterpolationEngine: interpolateBatch_SIMD matches scalar at boundary", "[interp]")
{
    const size_t N = 17;  // intentionally not a multiple of 4 or 8
    std::vector<float> a(N), b(N), dst_simd(N), dst_scalar(N);

    for (size_t i = 0; i < N; ++i)
    {
        a[i] = static_cast<float>(i) / static_cast<float>(N);
        b[i] = 1.0f - a[i];
    }

    const float t = 0.3f;
    InterpolationEngine::interpolateBatch_SIMD(a.data(), b.data(), dst_simd.data(), t, N);

    // Manual scalar reference
    for (size_t i = 0; i < N; ++i)
        dst_scalar[i] = a[i] * (1.0f - t) + b[i] * t;

    for (size_t i = 0; i < N; ++i)
        REQUIRE(dst_simd[i] == Approx(dst_scalar[i]).margin(1e-5f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  SnapshotBank
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SnapshotBank: no slots occupied after construction", "[bank]")
{
    SnapshotBank bank;
    REQUIRE_FALSE(bank.hasAnyOccupied());
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        REQUIRE_FALSE(bank.isOccupied(i));
}

TEST_CASE("SnapshotBank: captureValues marks slot occupied", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(4);

    std::vector<float> vals = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, vals);

    REQUIRE(bank.isOccupied(0));
    REQUIRE(bank.hasAnyOccupied());
    REQUIRE_FALSE(bank.isOccupied(1));
}

TEST_CASE("SnapshotBank: captureValues preserves values via getSlotValuesCopy", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(3);

    std::vector<float> input = {0.25f, 0.5f, 0.75f};
    bank.captureValues(5, input);

    std::vector<float> output;
    REQUIRE(bank.getSlotValuesCopy(5, output));
    REQUIRE(output.size() == 3);
    REQUIRE(output[0] == Approx(0.25f));
    REQUIRE(output[1] == Approx(0.50f));
    REQUIRE(output[2] == Approx(0.75f));
}

TEST_CASE("SnapshotBank: clearSlot removes a single slot", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(2);

    std::vector<float> v = {0.5f, 0.5f};
    bank.captureValues(0, v);
    bank.captureValues(1, v);

    REQUIRE(bank.hasAnyOccupied());
    bank.clearSlot(0);
    REQUIRE_FALSE(bank.isOccupied(0));
    REQUIRE(bank.isOccupied(1));
    REQUIRE(bank.hasAnyOccupied());
}

TEST_CASE("SnapshotBank: clearAll empties all slots", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(2);

    std::vector<float> v = {0.5f, 0.5f};
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        bank.captureValues(i, v);

    REQUIRE(bank.hasAnyOccupied());
    bank.clearAll();
    REQUIRE_FALSE(bank.hasAnyOccupied());
}

TEST_CASE("SnapshotBank: out-of-range slot indices are ignored safely", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(2);

    std::vector<float> v = {0.5f, 0.5f};
    // Must not crash:
    bank.captureValues(-1, v);
    bank.captureValues(999, v);
    bank.clearSlot(-1);
    bank.clearSlot(999);

    REQUIRE_FALSE(bank.hasAnyOccupied());
}

TEST_CASE("SnapshotBank: getOccupiedSlots returns correct count and indices", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(1);

    std::vector<float> v = {0.5f};
    bank.captureValues(2, v);
    bank.captureValues(7, v);
    bank.captureValues(11, v);

    std::array<int, SnapshotBank::NUM_SLOTS> slots{};
    int count = bank.getOccupiedSlots(slots);

    REQUIRE(count == 3);
    REQUIRE(slots[0] == 2);
    REQUIRE(slots[1] == 7);
    REQUIRE(slots[2] == 11);
}

TEST_CASE("SnapshotBank: tryReadLocked returns true when not contended", "[bank]")
{
    SnapshotBank bank;
    bank.prepare(2);

    std::vector<float> v = {0.3f, 0.7f};
    bank.captureValues(0, v);

    bool called = false;
    bool result = bank.tryReadLocked([&](const auto& slots) {
        called = true;
        REQUIRE(slots[0].occupied);
    });

    REQUIRE(result);
    REQUIRE(called);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SnapshotBank — FastMode / RecallMode
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SnapshotBank: default RecallMode is Fast", "[bank][fastmode]")
{
    SnapshotBank bank;
    REQUIRE(bank.getRecallMode() == RecallMode::Fast);
}

TEST_CASE("SnapshotBank: setRecallMode round-trips", "[bank][fastmode]")
{
    SnapshotBank bank;
    bank.setRecallMode(RecallMode::Full);
    REQUIRE(bank.getRecallMode() == RecallMode::Full);

    bank.setRecallMode(RecallMode::Fast);
    REQUIRE(bank.getRecallMode() == RecallMode::Fast);
}

TEST_CASE("SnapshotBank: captureStateChunk with nullptr plugin is safe", "[bank][fastmode]")
{
    SnapshotBank bank;
    bank.prepare(4);

    // Must not crash when plugin is nullptr
    bank.captureStateChunk(0, nullptr);
    bank.captureStateChunk(-1, nullptr);
    bank.captureStateChunk(999, nullptr);
}

TEST_CASE("SnapshotBank: recallStateChunk with nullptr plugin is safe", "[bank][fastmode]")
{
    SnapshotBank bank;
    bank.prepare(4);

    // Must not crash when plugin is nullptr
    bank.recallStateChunk(0, static_cast<juce::AudioPluginInstance*>(nullptr));
    bank.recallStateChunk(-1, static_cast<juce::AudioPluginInstance*>(nullptr));
}

// ─────────────────────────────────────────────────────────────────────────────
//  InterpolationEngine — 2D (IDW)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine: compute2D with single occupied slot returns that slot", "[interp][2d]")
{
    SnapshotBank bank;
    bank.prepare(3);

    std::vector<float> vals = {0.2f, 0.6f, 0.9f};
    bank.captureValues(0, vals);  // Slot 0 at 12 o'clock

    std::vector<float> output(3, 0.0f);
    InterpolationEngine::compute2D(0.0f, 0.0f, bank, output);

    // With only one occupied slot, output should match that slot's values
    REQUIRE(output[0] == Approx(0.2f).margin(1e-4f));
    REQUIRE(output[1] == Approx(0.6f).margin(1e-4f));
    REQUIRE(output[2] == Approx(0.9f).margin(1e-4f));
}

TEST_CASE("InterpolationEngine: compute2D with zero occupied slots leaves output unchanged", "[interp][2d]")
{
    SnapshotBank bank;
    bank.prepare(3);

    std::vector<float> output = {0.1f, 0.2f, 0.3f};
    InterpolationEngine::compute2D(0.0f, 0.0f, bank, output);

    // No occupied slots → output should remain at its initial values
    REQUIRE(output[0] == Approx(0.1f));
    REQUIRE(output[1] == Approx(0.2f));
    REQUIRE(output[2] == Approx(0.3f));
}

TEST_CASE("InterpolationEngine: compute2D with two equidistant slots returns midpoint", "[interp][2d]")
{
    SnapshotBank bank;
    bank.prepare(2);

    // Slot 0 at 12 o'clock, slot 6 at 6 o'clock — equidistant from center
    std::vector<float> valsA = {0.0f, 0.0f};
    std::vector<float> valsB = {1.0f, 1.0f};
    bank.captureValues(0, valsA);
    bank.captureValues(6, valsB);

    std::vector<float> output(2, 0.0f);
    // Cursor at center — equidistant from both slots
    InterpolationEngine::compute2D(0.0f, 0.0f, bank, output);

    // Should be close to midpoint (0.5, 0.5)
    REQUIRE(output[0] == Approx(0.5f).margin(0.05f));
    REQUIRE(output[1] == Approx(0.5f).margin(0.05f));
}
