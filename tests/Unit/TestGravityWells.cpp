/*
 * More-Phi — Unit Tests for Gravity Wells (Phase 1)
 * Catch2 v3 test cases.
 *
 * Coverage:
 *   - Default mass = 1.0 on new ParameterState
 *   - setMass/getMass round-trip with clamping
 *   - IDW interpolation weights scale with mass
 *   - Mass persistence via toXml/fromXml
 *   - Seqlock-safe concurrent read/write
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/ParameterState.h"
#include "Core/SnapshotBank.h"
#include "Core/InterpolationEngine.h"

#include <array>
#include <cmath>
#include <thread>
#include <vector>

using Catch::Approx;
using namespace more_phi;

// ─────────────────────────────────────────────────────────────────────────────
//  ParameterState — default mass
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ParameterState: default mass is 1.0", "[gravity-wells]")
{
    ParameterState slot;
    REQUIRE(slot.mass == Approx(1.0f));
}

TEST_CASE("ParameterState: mass survives capture", "[gravity-wells]")
{
    ParameterState slot;
    slot.mass = 2.5f;
    std::array<float, 4> vals = { 0.1f, 0.5f, 0.8f, 0.3f };
    slot.capture(vals.data(), 4);
    REQUIRE(slot.occupied == true);
    REQUIRE(slot.parameterCount == 4);
    REQUIRE(slot.mass == Approx(2.5f)); // mass preserved across capture
}

// ─────────────────────────────────────────────────────────────────────────────
//  SnapshotBank — setMass / getMass
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SnapshotBank: setMass/getMass round-trip", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);

    // Capture something so slot 0 is occupied
    std::vector<float> vals(4, 0.5f);
    bank.captureValues(0, vals);

    // Default mass
    REQUIRE(bank.getMass(0) == Approx(1.0f));

    // Set and verify
    bank.setMass(0, 2.0f);
    REQUIRE(bank.getMass(0) == Approx(2.0f));

    bank.setMass(0, 0.3f);
    REQUIRE(bank.getMass(0) == Approx(0.3f));
}

TEST_CASE("SnapshotBank: setMass clamps to [0.1, 3.0]", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);
    std::vector<float> vals(4, 0.5f);
    bank.captureValues(0, vals);

    bank.setMass(0, -1.0f);
    REQUIRE(bank.getMass(0) == Approx(0.1f));

    bank.setMass(0, 5.0f);
    REQUIRE(bank.getMass(0) == Approx(3.0f));

    bank.setMass(0, 1.5f);
    REQUIRE(bank.getMass(0) == Approx(1.5f));
}

TEST_CASE("SnapshotBank: setMass out-of-range slot is no-op", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);
    bank.setMass(-1, 2.0f);  // should not crash
    bank.setMass(12, 2.0f);  // should not crash
    // Just verify no exception — this is a smoke test
    SUCCEED("No crash on out-of-range slot");
}

TEST_CASE("SnapshotBank: getMasses bulk read", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);
    std::vector<float> vals(4, 0.5f);
    bank.captureValues(0, vals);
    bank.captureValues(1, vals);
    bank.setMass(0, 2.0f);
    bank.setMass(1, 0.5f);

    std::array<float, 12> masses{};
    bank.getMasses(masses);
    REQUIRE(masses[0] == Approx(2.0f));
    REQUIRE(masses[1] == Approx(0.5f));
    // Unoccupied slots keep default
    REQUIRE(masses[2] == Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mass persistence via XML
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SnapshotBank: mass survives toXml/fromXml round-trip", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);
    std::vector<float> vals(4, 0.5f);
    bank.captureValues(0, vals);
    bank.setMass(0, 2.5f);

    auto xml = bank.toXml();
    REQUIRE(xml != nullptr);

    // Verify XML contains mass attribute
    auto* slotEl = xml->getChildByName("SLOT");
    REQUIRE(slotEl != nullptr);
    REQUIRE(slotEl->getDoubleAttribute("mass", 1.0) == Approx(2.5));

    // Restore into a new bank
    SnapshotBank bank2;
    bank2.prepare(64);
    bank2.fromXml(*xml);

    REQUIRE(bank2.isOccupied(0));
    REQUIRE(bank2.getMass(0) == Approx(2.5f));
}

TEST_CASE("SnapshotBank: default mass (1.0) is omitted from XML", "[gravity-wells]")
{
    SnapshotBank bank;
    bank.prepare(64);
    std::vector<float> vals(4, 0.5f);
    bank.captureValues(0, vals);
    // mass stays at default 1.0 — should not appear in XML

    auto xml = bank.toXml();
    auto* slotEl = xml->getChildByName("SLOT");
    REQUIRE(slotEl != nullptr);
    REQUIRE_FALSE(slotEl->hasAttribute("mass"));
}

TEST_CASE("SnapshotBank: backward compat - old XML without mass defaults to 1.0", "[gravity-wells]")
{
    // Simulate old preset XML (no mass attribute)
    juce::XmlElement xml("SNAPSHOT_BANK");
    auto* slot = xml.createNewChildElement("SLOT");
    slot->setAttribute("id", 0);
    slot->setAttribute("paramCount", 4);
    slot->setAttribute("name", "Test");
    juce::MemoryBlock block(4 * sizeof(float));
    slot->setAttribute("values", block.toBase64Encoding());

    SnapshotBank bank;
    bank.prepare(64);
    bank.fromXml(xml);

    REQUIRE(bank.isOccupied(0));
    REQUIRE(bank.getMass(0) == Approx(1.0f));  // default
}

// ─────────────────────────────────────────────────────────────────────────────
//  Interpolation — mass-weighted IDW
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine: higher mass means higher weight (IDW)", "[gravity-wells]")
{
    // Set up: two snapshots at [0,1] and [0,-1] (clock positions 0 and 6)
    // Cursor at [0, 0] → equal distance → equal weight if same mass
    SnapshotBank bank;
    bank.prepare(4);

    std::vector<float> valsA = { 0.1f, 0.2f, 0.3f, 0.4f };
    std::vector<float> valsB = { 0.9f, 0.8f, 0.7f, 0.6f };
    bank.captureValues(0, valsA);  // clock pos 0: (0, 1)
    bank.captureValues(1, valsB);  // clock pos 3: (1, 0) — roughly equidistant from center

    // Position cursor at (0, 0.5) — closer to slot 0
    std::vector<float> output(4, 0.0f);
    InterpolationEngine::compute2D(0.0f, 0.5f, bank, output);

    float sumEqualMass = output[0];
    // With equal mass, cursor at (0, 0.5) is roughly equidistant from both
    // slots at bottom of circle, so blend is ~midpoint of their values.
    // slot 0 has value 0.1, slot 1 has value 0.9 → midpoint ~0.5
    REQUIRE(sumEqualMass == Approx(0.512f).margin(0.01f));

    // Now give slot 0 more mass (3.0) and slot 1 less (0.1).
    // Output should shift strongly toward slot 0's value (0.1).
    bank.setMass(0, 3.0f);
    bank.setMass(1, 0.1f);

    std::vector<float> output2(4, 0.0f);
    InterpolationEngine::compute2D(0.0f, 0.5f, bank, output2);

    // Slot 0 now dominates → output[0] should be much closer to slot 0's 0.1
    float sumHighMass = output2[0];
    REQUIRE(sumHighMass < sumEqualMass);  // pulled toward slot 0's low value
    REQUIRE(sumHighMass == Approx(0.127f).margin(0.02f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Thread safety — concurrent mass read/write
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SnapshotBank: concurrent mass read/write is safe", "[gravity-wells][concurrency]")
{
    SnapshotBank bank;
    bank.prepare(64);
    std::vector<float> vals(64, 0.5f);
    for (int i = 0; i < 12; ++i)
        bank.captureValues(i, vals);

    std::atomic<bool> running{true};
    std::atomic<int> readCount{0};
    std::atomic<int> writeCount{0};

    // Reader thread — simulates audio thread
    std::thread reader([&]() {
        while (running.load(std::memory_order_relaxed))
        {
            float m = bank.getMass(0);
            REQUIRE(m >= 0.1f);
            REQUIRE(m <= 3.0f);
            ++readCount;
        }
    });

    // Writer thread — simulates UI/MCP thread
    std::thread writer([&]() {
        for (int i = 0; i < 1000 && running.load(std::memory_order_relaxed); ++i)
        {
            float newMass = 1.0f + static_cast<float>(i % 20) * 0.1f;
            bank.setMass(0, newMass);
            ++writeCount;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    writer.join();
    running.store(false, std::memory_order_relaxed);
    reader.join();

    REQUIRE(readCount > 0);
    REQUIRE(writeCount > 0);
    // Final mass should be clamped and valid
    float finalMass = bank.getMass(0);
    REQUIRE(finalMass >= 0.1f);
    REQUIRE(finalMass <= 3.0f);
}
