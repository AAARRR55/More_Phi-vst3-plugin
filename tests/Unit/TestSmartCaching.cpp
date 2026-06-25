#include <catch2/catch_test_macros.hpp>

#include "Core/SnapshotBank.h"

using namespace more_phi;

TEST_CASE("SnapshotBank: setCachedPluginUID/getCachedPluginUID round-trip", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));

    // Initially empty
    REQUIRE(bank.getCachedPluginUID(0).isEmpty());

    bank.setCachedPluginUID(0, "Serum|Xfer Records");
    REQUIRE(bank.getCachedPluginUID(0) == "Serum|Xfer Records");

    bank.setCachedPluginUID(0, "Vital|Matt Tytel");
    REQUIRE(bank.getCachedPluginUID(0) == "Vital|Matt Tytel");
}

TEST_CASE("SnapshotBank: pluginUIDMatches", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));

    bank.setCachedPluginUID(0, "Serum|Xfer Records");

    REQUIRE(bank.pluginUIDMatches(0, "Serum|Xfer Records"));
    REQUIRE_FALSE(bank.pluginUIDMatches(0, "Vital|Matt Tytel"));
    REQUIRE_FALSE(bank.pluginUIDMatches(0, ""));

    // Out-of-range slots
    REQUIRE_FALSE(bank.pluginUIDMatches(-1, "Serum|Xfer Records"));
    REQUIRE_FALSE(bank.pluginUIDMatches(SnapshotBank::NUM_SLOTS, "Serum|Xfer Records"));
}

TEST_CASE("SnapshotBank: clearSlot clears cached plugin UID", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));

    bank.setCachedPluginUID(0, "Serum|Xfer Records");
    REQUIRE(bank.getCachedPluginUID(0) == "Serum|Xfer Records");

    bank.clearSlot(0);
    REQUIRE(bank.getCachedPluginUID(0).isEmpty());
}

TEST_CASE("SnapshotBank: clearAll clears all cached plugin UIDs", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));
    bank.captureValues(1, std::vector<float>(values, values + 4));

    bank.setCachedPluginUID(0, "Serum|Xfer Records");
    bank.setCachedPluginUID(1, "Vital|Matt Tytel");

    bank.clearAll();
    REQUIRE(bank.getCachedPluginUID(0).isEmpty());
    REQUIRE(bank.getCachedPluginUID(1).isEmpty());
}

TEST_CASE("SnapshotBank: setCachedPluginUID out-of-range is no-op", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    bank.setCachedPluginUID(-1, "should-not-stick");
    REQUIRE(bank.getCachedPluginUID(-1).isEmpty());

    bank.setCachedPluginUID(SnapshotBank::NUM_SLOTS, "should-not-stick");
    REQUIRE(bank.getCachedPluginUID(SnapshotBank::NUM_SLOTS).isEmpty());
}

TEST_CASE("SnapshotBank: getCachedPluginUID out-of-range returns empty", "[smart-caching]")
{
    SnapshotBank bank;
    REQUIRE(bank.getCachedPluginUID(-1).isEmpty());
    REQUIRE(bank.getCachedPluginUID(SnapshotBank::NUM_SLOTS).isEmpty());
}

TEST_CASE("SnapshotBank: plugin UID survives toXml/fromXml round-trip", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));
    bank.setCachedPluginUID(0, "Serum|Xfer Records");

    // Also capture a state chunk so the slot is properly occupied for Full recall
    juce::MemoryBlock chunk("opaque-state", 12);
    bank.captureStateChunk(0, chunk);

    auto xml = bank.toXml();
    REQUIRE(xml != nullptr);

    SnapshotBank restored;
    restored.prepare(4);
    restored.fromXml(*xml);

    REQUIRE(restored.getCachedPluginUID(0) == "Serum|Xfer Records");
    REQUIRE(restored.hasStateChunk(0));
}

TEST_CASE("SnapshotBank: empty UID is not persisted to XML", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));

    // No UID set — should not appear in XML
    auto xml = bank.toXml();

    SnapshotBank restored;
    restored.prepare(4);
    restored.fromXml(*xml);

    REQUIRE(restored.getCachedPluginUID(0).isEmpty());
}

TEST_CASE("SnapshotBank: backward compat - XML without pluginUID defaults to empty", "[smart-caching]")
{
    SnapshotBank bank;
    bank.prepare(4);

    const float values[] = {0.1f, 0.2f, 0.3f, 0.4f};
    bank.captureValues(0, std::vector<float>(values, values + 4));

    // Manually create XML with stateChunk but WITHOUT pluginUID attribute (legacy format)
    auto xml = std::make_unique<juce::XmlElement>("SNAPSHOT_BANK");
    auto* slotXml = xml->createNewChildElement("SLOT");
    slotXml->setAttribute("id", 0);
    slotXml->setAttribute("paramCount", 4);
    slotXml->setAttribute("name", "");
    juce::MemoryBlock valueBlock(values, 4 * sizeof(float));
    slotXml->setAttribute("values", valueBlock.toBase64Encoding());
    juce::MemoryBlock chunk("legacy-state", 11);
    slotXml->setAttribute("stateChunk", chunk.toBase64Encoding());

    SnapshotBank restored;
    restored.prepare(4);
    restored.fromXml(*xml);

    // Legacy XML with no pluginUID — should be empty, not corrupted
    REQUIRE(restored.getCachedPluginUID(0).isEmpty());
    REQUIRE(restored.hasStateChunk(0));
}
