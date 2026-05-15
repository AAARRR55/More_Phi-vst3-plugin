#include <catch2/catch_test_macros.hpp>

#include "Core/SnapshotBank.h"

#include <cstring>

using namespace more_phi;

TEST_CASE("SnapshotBank state chunks are copied, persisted, and cleared with their slot", "[state][snapshot][state_chunk]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    juce::StringArray names;
    names.add("Gain");
    names.add("Cutoff");
    names.add("Resonance");
    names.add("Mix");
    bank.captureValuesWithNames(2, values, 4, names);

    juce::MemoryBlock chunk("opaque-state", 12);
    bank.captureStateChunk(2, chunk);

    REQUIRE(bank.hasStateChunk(2));

    juce::MemoryBlock copied;
    REQUIRE(bank.copyStateChunk(2, copied));
    REQUIRE(copied.getSize() == chunk.getSize());
    REQUIRE(std::memcmp(copied.getData(), chunk.getData(), chunk.getSize()) == 0);

    auto xml = bank.toXml();
    REQUIRE(xml != nullptr);

    SnapshotBank restored;
    restored.prepare(4);
    restored.fromXml(*xml);
    REQUIRE(restored.hasStateChunk(2));
    REQUIRE(restored.findParameterIndex(2, "Cutoff") == 1);

    restored.clearSlot(2);
    REQUIRE_FALSE(restored.hasStateChunk(2));
    REQUIRE_FALSE(restored.copyStateChunk(2, copied));

    REQUIRE_FALSE(restored.hasStateChunk(-1));
    REQUIRE_FALSE(restored.copyStateChunk(SnapshotBank::NUM_SLOTS, copied));
}
