/*
 * More-Phi — Unit Tests — Sidechain Trigger
 * Tests for MIDIRouter::processSidechain edge detection and round-robin.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "MIDI/MIDIRouter.h"

#include <vector>
#include <cmath>

using Catch::Approx;
using namespace more_phi;

// Helper: create an AudioBuffer with constant amplitude
static juce::AudioBuffer<float> makeConstantBuffer(int numSamples, float amplitude)
{
    juce::AudioBuffer<float> buf(1, numSamples);
    for (int s = 0; s < numSamples; ++s)
        buf.setSample(0, s, amplitude);
    return buf;
}

// ── Callback helpers (plain C function pointers + void* context) ────────────

struct TriggerCapture {
    int value = -1;
};

struct TriggerCollector {
    std::vector<int> slots;
};

struct TriggerCounter {
    int count = 0;
};

static void captureSlot(int slot, void* ctx) {
    static_cast<TriggerCapture*>(ctx)->value = slot;
}

static void collectSlot(int slot, void* ctx) {
    static_cast<TriggerCollector*>(ctx)->slots.push_back(slot);
}

static void countTrigger(int /*slot*/, void* ctx) {
    ++static_cast<TriggerCounter*>(ctx)->count;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MIDIRouter — Sidechain Trigger
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sidechain: silent buffer does not trigger", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCapture cap;
    router.setSnapshotCallback(&captureSlot, &cap);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto buf = makeConstantBuffer(512, 0.0f);
    router.processSidechain(buf);

    REQUIRE(cap.value == -1);
}

TEST_CASE("Sidechain: loud buffer fires trigger exactly once", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCollector col;
    router.setSnapshotCallback(&collectSlot, &col);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto buf = makeConstantBuffer(512, 0.5f);
    router.processSidechain(buf);

    REQUIRE(col.slots.size() == 1);
    REQUIRE(col.slots[0] == 0);  // First trigger → slot 0
}

TEST_CASE("Sidechain: sustained loud does not re-trigger", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCounter counter;
    router.setSnapshotCallback(&countTrigger, &counter);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto buf = makeConstantBuffer(512, 0.5f);

    // Process 3 consecutive loud blocks — should trigger only on the first
    router.processSidechain(buf);
    router.processSidechain(buf);
    router.processSidechain(buf);

    REQUIRE(counter.count == 1);
}

TEST_CASE("Sidechain: loud-silent-loud triggers on second rising edge", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCollector col;
    router.setSnapshotCallback(&collectSlot, &col);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto loud  = makeConstantBuffer(512, 0.5f);
    auto quiet = makeConstantBuffer(512, 0.0f);

    router.processSidechain(loud);   // Rising edge → trigger slot 0
    router.processSidechain(quiet);  // Below threshold → reset gate
    router.processSidechain(loud);   // Rising edge → trigger slot 1

    REQUIRE(col.slots.size() == 2);
    REQUIRE(col.slots[0] == 0);
    REQUIRE(col.slots[1] == 1);
}

TEST_CASE("Sidechain: round-robin cycles through 12 slots", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCollector col;
    router.setSnapshotCallback(&collectSlot, &col);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto loud  = makeConstantBuffer(256, 0.5f);
    auto quiet = makeConstantBuffer(256, 0.0f);

    // Trigger 13 times — should cycle 0..11 then back to 0
    for (int i = 0; i < 13; ++i)
    {
        router.processSidechain(loud);
        router.processSidechain(quiet);
    }

    REQUIRE(col.slots.size() == 13);
    for (int i = 0; i < 12; ++i)
        REQUIRE(col.slots[i] == i);
    REQUIRE(col.slots[12] == 0);  // Wrapped around
}

TEST_CASE("Sidechain: disabled does not fire triggers", "[midi][sidechain]")
{
    MIDIRouter router;
    TriggerCapture cap;
    router.setSnapshotCallback(&captureSlot, &cap);
    router.setSidechainEnabled(false);  // Disabled
    router.setSidechainThreshold(0.1f);

    auto buf = makeConstantBuffer(512, 0.5f);
    router.processSidechain(buf);

    REQUIRE(cap.value == -1);
}

TEST_CASE("Sidechain: threshold accessor round-trips", "[midi][sidechain]")
{
    MIDIRouter router;
    router.setSidechainThreshold(0.42f);
    REQUIRE(router.getSidechainThreshold() == Approx(0.42f));
}
