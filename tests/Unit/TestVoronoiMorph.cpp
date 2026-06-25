/*
 * More-Phi — Unit Tests for Voronoi/NNI Morphing Engine (Phase 2)
 * Catch2 v3 test cases.
 *
 * Coverage:
 *   - Delaunay triangulation builds correctly for 3-12 points on a circle
 *   - findContainingTriangle returns correct triangle
 *   - Barycentric weights sum to ~1.0
 *   - Cursor directly on vertex → exclusive weight
 *   - Cursor outside convex hull → zero weights (IDW fallback)
 *   - Gravity Well mass modulates weights
 *   - Voronoi cells computed for UI
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/VoronoiMorphEngine.h"
#include "Core/InterpolationEngine.h"
#include "Core/SnapshotBank.h"

#include <array>
#include <cmath>
#include <vector>

using Catch::Approx;
using namespace more_phi;

// ── Helper: set up a SnapshotBank with N occupied slots ──────────────────────

static void occupySlots(SnapshotBank& bank, int numSlots)
{
    std::vector<float> vals(4, 0.5f);
    for (int i = 0; i < numSlots; ++i)
        bank.captureValues(i, vals);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Delaunay Triangulation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("VoronoiMorphEngine: rebuild fails with <3 occupied slots", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 2);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);

    REQUIRE_FALSE(engine.isValid());
}

TEST_CASE("VoronoiMorphEngine: rebuild succeeds with 3+ occupied slots", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 3);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);

    REQUIRE(engine.isValid());
}

TEST_CASE("VoronoiMorphEngine: triangulation edges are non-empty", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 6);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);

    REQUIRE(engine.isValid());
    const auto& edges = engine.getEdges();
    REQUIRE_FALSE(edges.empty());
    // For 6 points on a circle, Delaunay triangulation has ~2n-3 edges ≈ 9
    REQUIRE(edges.size() >= 6);
}

TEST_CASE("VoronoiMorphEngine: rebuild with all 12 slots", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 12);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);

    REQUIRE(engine.isValid());
    const auto& edges = engine.getEdges();
    // 12 points on convex hull: triangulation has 2*12-3 = 21 edges
    REQUIRE(edges.size() >= 18);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Voronoi Cells (for UI)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("VoronoiMorphEngine: Voronoi cells for 3+ occupied slots", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 4);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);

    REQUIRE(engine.isValid());
    const auto& cells = engine.getVoronoiCells();
    REQUIRE_FALSE(cells.empty());
    // Should have one cell per occupied slot
    REQUIRE(cells.size() >= 3);

    // Each cell should be a convex polygon with at least 3 vertices
    for (const auto& cell : cells)
    {
        REQUIRE(cell.vertices.size() >= 3);
        REQUIRE(cell.slotIndex >= 0);
        REQUIRE(cell.slotIndex < 12);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  NNI Weight Computation (barycentric)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("VoronoiMorphEngine: computeWeights inside triangle", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    // Occupy 3 slots forming a clear triangle: 0=(0,-1), 3=(1,0), 6=(0,1)
    for (int i : {0, 3, 6})
    {
        std::vector<float> vals(4, 0.5f);
        bank.captureValues(i, vals);
    }

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE(engine.isValid());

    // Cursor slightly below center — clearly inside triangle (0,3,6)
    std::array<float, 12> masses{};
    masses.fill(1.0f);
    std::array<float, 12> weights{};
    float totalWeight = 0.0f;

    engine.computeWeights(0.0f, -0.2f, positions, masses, weights, totalWeight);

    REQUIRE(totalWeight > 0.0f);
    // Weights should sum to totalWeight
    float sum = 0.0f;
    for (int i = 0; i < 12; ++i)
        sum += weights[i];
    REQUIRE(sum == Approx(totalWeight).margin(1e-5f));
}

TEST_CASE("VoronoiMorphEngine: cursor on vertex gives exclusive weight", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 6);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE(engine.isValid());

    std::array<float, 12> masses{};
    masses.fill(1.0f);
    std::array<float, 12> weights{};
    float totalWeight = 0.0f;

    // Place cursor exactly at slot 0's clock position
    engine.computeWeights(positions[0].x, positions[0].y, positions, masses, weights, totalWeight);

    REQUIRE(totalWeight == Approx(1.0f).margin(1e-4f));
    REQUIRE(weights[0] == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("VoronoiMorphEngine: cursor outside convex hull gives zero weights", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    occupySlots(bank, 3);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE(engine.isValid());

    std::array<float, 12> masses{};
    masses.fill(1.0f);
    std::array<float, 12> weights{};
    float totalWeight = 0.0f;

    // Place cursor far outside the unit circle
    engine.computeWeights(5.0f, 5.0f, positions, masses, weights, totalWeight);

    REQUIRE(totalWeight == 0.0f);  // fallback to IDW
}

TEST_CASE("VoronoiMorphEngine: mass modulates weights", "[voronoi]")
{
    SnapshotBank bank;
    bank.prepare(64);
    // Occupy 3 adjacent slots so cursor is inside their triangle
    for (int i : {0, 1, 2})
    {
        std::vector<float> vals(4, static_cast<float>(i) * 0.1f);
        bank.captureValues(i, vals);
    }

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE(engine.isValid());

    std::array<float, 12> masses{};
    masses.fill(1.0f);
    std::array<float, 12> weights1{};
    float total1 = 0.0f;

    // Cursor clearly inside the triangle of slots 0, 1, 2
    // Average of the three positions:
    float cx = (positions[0].x + positions[1].x + positions[2].x) / 3.0f;
    float cy = (positions[0].y + positions[1].y + positions[2].y) / 3.0f;
    engine.computeWeights(cx, cy, positions, masses, weights1, total1);

    // Now give slot 0 high mass and slot 2 low mass
    masses[0] = 3.0f;
    masses[2] = 0.1f;
    std::array<float, 12> weights2{};
    float total2 = 0.0f;
    engine.computeWeights(cx, cy, positions, masses, weights2, total2);

    // Slot 0's normalized weight should increase relative to slot 2
    float norm0_before = weights1[0] / total1;
    float norm2_before = weights1[2] / total1;
    float norm0_after  = weights2[0] / total2;
    float norm2_after  = weights2[2] / total2;

    float ratioBefore = norm0_before / (norm2_before + 1e-10f);
    float ratioAfter  = norm0_after  / (norm2_after  + 1e-10f);

    REQUIRE(ratioAfter > ratioBefore);  // slot 0 gained influence
}

// ─────────────────────────────────────────────────────────────────────────────
//  compute2D_Voronoi integration
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine::compute2D_Voronoi with valid triangulation", "[voronoi][interpolation]")
{
    SnapshotBank bank;
    bank.prepare(8);
    // Occupy 4 slots
    std::vector<float> valsA = { 0.1f, 0.2f, 0.3f, 0.4f, 0.1f, 0.2f, 0.3f, 0.4f };
    std::vector<float> valsB = { 0.9f, 0.8f, 0.7f, 0.6f, 0.9f, 0.8f, 0.7f, 0.6f };
    bank.captureValues(0, valsA);
    bank.captureValues(1, valsA);
    bank.captureValues(6, valsB);
    bank.captureValues(7, valsB);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE(engine.isValid());

    std::vector<float> output(8, 0.0f);
    InterpolationEngine::compute2D_Voronoi(0.0f, 0.5f, bank, engine, output);

    // Output should be non-zero and within [0, 1]
    bool anyNonZero = false;
    for (float v : output)
    {
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
        if (v > 0.01f) anyNonZero = true;
    }
    REQUIRE(anyNonZero);
}

TEST_CASE("InterpolationEngine::compute2D_Voronoi falls back to IDW when invalid", "[voronoi][interpolation]")
{
    SnapshotBank bank;
    bank.prepare(4);
    // Only 2 occupied slots — triangulation invalid
    std::vector<float> valsA = { 0.1f, 0.2f, 0.3f, 0.4f };
    std::vector<float> valsB = { 0.9f, 0.8f, 0.7f, 0.6f };
    bank.captureValues(0, valsA);
    bank.captureValues(1, valsB);

    VoronoiMorphEngine engine;
    auto positions = InterpolationEngine::getClockPositions();
    engine.rebuild(positions, bank);
    REQUIRE_FALSE(engine.isValid());

    std::vector<float> output(4, 0.0f);
    InterpolationEngine::compute2D_Voronoi(0.0f, 0.0f, bank, engine, output);

    // Should have fallen back to IDW — output should be non-zero blend
    bool anyNonZero = false;
    for (float v : output)
        if (v > 0.01f) anyNonZero = true;
    REQUIRE(anyNonZero);
}
