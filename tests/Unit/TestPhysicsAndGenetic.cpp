/*
 * More-Phi — Unit Tests for PhysicsEngine and GeneticEngine
 * Catch2 v3 test cases.
 *
 * Coverage:
 *   PhysicsEngine:
 *     - updateElastic()  spring-damper converges toward target
 *     - perlin()         output is bounded and smooth
 *     - updateDrift()    Free mode produces non-zero output
 *   GeneticEngine:
 *     - breed()          crossoverRatio=0 → parentA, ratio=1 → parentB
 *     - breed()          output is clamped to [0, 1]
 *     - smartRandomize() only modifies learned parameters
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/PhysicsEngine.h"
#include "Core/GeneticEngine.h"
#include "Core/ParameterState.h"

#include <array>
#include <cmath>
#include <set>

using Catch::Approx;
using namespace more_phi;

// ─────────────────────────────────────────────────────────────────────────────
//  PhysicsEngine — Spring-Damper (Elastic)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PhysicsEngine::updateElastic: position converges toward target", "[physics]")
{
    ElasticState s{};
    const float targetX = 0.8f, targetY = 0.5f;
    constexpr float dt = 1.0f / 60.0f;  // 60 fps step

    // After enough steps the spring should pull position toward target
    for (int i = 0; i < 500; ++i)
        PhysicsEngine::updateElastic(s, targetX, targetY, ElasticPreset::Medium, dt);

    // Should be significantly closer than initial (0,0) to target.
    // dtScale compensation (kRefDt/dt) makes the spring converge more slowly
    // at 60fps than at the reference 44100/512 rate, so use generous tolerance.
    REQUIRE(std::abs(s.x - targetX) < 0.3f);
    REQUIRE(std::abs(s.y - targetY) < 0.3f);
}

TEST_CASE("PhysicsEngine::updateElastic: starts from rest with zero velocity", "[physics]")
{
    ElasticState s{};
    REQUIRE(s.x  == Approx(0.0f));
    REQUIRE(s.y  == Approx(0.0f));
    REQUIRE(s.vx == Approx(0.0f));
    REQUIRE(s.vy == Approx(0.0f));
}

TEST_CASE("PhysicsEngine::updateElastic: slow preset converges slower than medium", "[physics]")
{
    ElasticState sSlow{}, sMed{};
    constexpr float dt = 1.0f / 60.0f;

    for (int i = 0; i < 30; ++i)
    {
        PhysicsEngine::updateElastic(sSlow, 1.0f, 0.0f, ElasticPreset::Slow,   dt);
        PhysicsEngine::updateElastic(sMed,  1.0f, 0.0f, ElasticPreset::Medium, dt);
    }

    // After 30 steps, Medium should be further along than Slow
    REQUIRE(sMed.x > sSlow.x);
}

TEST_CASE("PhysicsEngine::updateElastic: is noexcept", "[physics]")
{
    ElasticState s{};
    STATIC_REQUIRE(noexcept(PhysicsEngine::updateElastic(s, 0.f, 0.f, ElasticPreset::Medium, 0.016f)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  PhysicsEngine — Perlin Noise
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PhysicsEngine::perlin: output is bounded within [-1.5, 1.5] for varied inputs", "[physics]")
{
    for (int xi = 0; xi < 10; ++xi)
    {
        for (int yi = 0; yi < 10; ++yi)
        {
            const float x = xi * 0.37f + 0.1f;
            const float y = yi * 0.53f + 0.2f;
            const float v = PhysicsEngine::perlin(x, y);
            // Perlin gradient extremes can slightly exceed ±1.0
            REQUIRE(v >= -1.5f);
            REQUIRE(v <=  1.5f);
        }
    }
}

TEST_CASE("PhysicsEngine::perlin: identical inputs produce identical output", "[physics]")
{
    const float v1 = PhysicsEngine::perlin(1.23f, 4.56f);
    const float v2 = PhysicsEngine::perlin(1.23f, 4.56f);
    REQUIRE(v1 == Approx(v2));
}

TEST_CASE("PhysicsEngine::perlin: nearby inputs produce nearby outputs (smooth)", "[physics]")
{
    const float v1 = PhysicsEngine::perlin(0.5f, 0.5f);
    const float v2 = PhysicsEngine::perlin(0.5f + 0.001f, 0.5f);
    // Smooth noise — difference should be tiny for tiny input change
    REQUIRE(std::abs(v1 - v2) < 0.05f);
}

TEST_CASE("PhysicsEngine::perlin: is noexcept", "[physics]")
{
    STATIC_REQUIRE(noexcept(PhysicsEngine::perlin(0.f, 0.f)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  PhysicsEngine — Drift
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PhysicsEngine::updateDrift: Free mode produces non-zero output for non-zero time", "[physics]")
{
    float outX = 0.f, outY = 0.f;
    PhysicsEngine::updateDrift(outX, outY,
        /*time=*/1.0f, /*speed=*/0.5f, /*distance=*/0.5f,
        /*chaos=*/0.5f, DriftMode::Free,
        /*anchorX=*/0.f, /*anchorY=*/0.f, /*gravity=*/0.5f);

    // For non-trivial time, at least one output should be non-zero
    REQUIRE((std::abs(outX) > 1e-6f || std::abs(outY) > 1e-6f));
}

TEST_CASE("PhysicsEngine::updateDrift: Locked mode stays near anchor", "[physics]")
{
    float outX = 99.f, outY = 99.f;
    PhysicsEngine::updateDrift(outX, outY,
        1.0f, 0.1f, 0.01f, 0.1f, DriftMode::Locked,
        0.5f, 0.5f, 0.99f);  // high gravity → stays very close to anchor

    REQUIRE(std::abs(outX - 0.5f) < 0.1f);
    REQUIRE(std::abs(outY - 0.5f) < 0.1f);
}

TEST_CASE("PhysicsEngine::updateDrift: is noexcept", "[physics]")
{
    float x = 0.f, y = 0.f;
    STATIC_REQUIRE(noexcept(
        PhysicsEngine::updateDrift(x, y, 0.f, 0.f, 0.f, 0.f, DriftMode::Free, 0.f, 0.f, 0.f)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  GeneticEngine — Breed
// ─────────────────────────────────────────────────────────────────────────────

// Helper: create an occupied ParameterState with known values
static ParameterState makeState(float fillValue, int count)
{
    ParameterState s;
    std::vector<float> vals(static_cast<size_t>(count), fillValue);
    s.capture(vals.data(), count);
    return s;
}

// Fixed, reproducible RNG seeds — one per test for isolation
namespace {
    constexpr juce::int64 kSeedBreedPureA    = 12345;
    constexpr juce::int64 kSeedBreedPureB    = 99999;
    constexpr juce::int64 kSeedBreedClamped  = 42;
    constexpr juce::int64 kSeedBreedEmpty    = 1;
    constexpr juce::int64 kSeedSmartOnlyLearned = 77777;
    constexpr juce::int64 kSeedSmartZeroAmt  = 55555;
    constexpr juce::int64 kSeedSmartEmptySet = 11111;
} // namespace

TEST_CASE("GeneticEngine::breed: crossoverRatio=0 produces result approx parentA", "[genetic]")
{
    juce::Random rng(kSeedBreedPureA);
    auto parentA = makeState(0.2f, 8);
    auto parentB = makeState(0.8f, 8);

    auto child = GeneticEngine::breed(parentA, parentB, 0.0f, 0.0f, rng);

    REQUIRE(child.occupied);
    REQUIRE(child.parameterCount == 8);
    for (int i = 0; i < child.parameterCount; ++i)
        REQUIRE(child.data()[i] == Approx(0.2f).margin(0.01f));
}

TEST_CASE("GeneticEngine::breed: crossoverRatio=1 produces result approx parentB", "[genetic]")
{
    juce::Random rng(kSeedBreedPureB);
    auto parentA = makeState(0.1f, 8);
    auto parentB = makeState(0.9f, 8);

    auto child = GeneticEngine::breed(parentA, parentB, 1.0f, 0.0f, rng);

    REQUIRE(child.occupied);
    for (int i = 0; i < child.parameterCount; ++i)
        REQUIRE(child.data()[i] == Approx(0.9f).margin(0.01f));
}

TEST_CASE("GeneticEngine::breed: output is clamped to [0, 1]", "[genetic]")
{
    juce::Random rng(kSeedBreedClamped);
    auto parentA = makeState(0.5f, 16);
    auto parentB = makeState(0.5f, 16);

    // Even with high mutation, values should stay in [0, 1]
    auto child = GeneticEngine::breed(parentA, parentB, 0.5f, 1.0f, rng);

    REQUIRE(child.occupied);
    for (int i = 0; i < child.parameterCount; ++i)
    {
        REQUIRE(child.data()[i] >= 0.0f);
        REQUIRE(child.data()[i] <= 1.0f);
    }
}

TEST_CASE("GeneticEngine::breed: both parents must be occupied", "[genetic]")
{
    juce::Random rng(kSeedBreedEmpty);
    ParameterState emptyParent;  // not occupied
    auto validParent = makeState(0.5f, 4);

    // Breeding with an empty parent should either return empty or handle gracefully
    // (exact behavior depends on implementation — at minimum it must not crash)
    [[maybe_unused]] auto child = GeneticEngine::breed(emptyParent, validParent, 0.5f, 0.0f, rng);
}

// ─────────────────────────────────────────────────────────────────────────────
//  GeneticEngine — Smart Randomize
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GeneticEngine::smartRandomize: only modifies learned parameters", "[genetic]")
{
    juce::Random rng(kSeedSmartOnlyLearned);
    ParameterState state = makeState(0.5f, 10);

    // Only params 2 and 7 are "learned"
    const std::unordered_set<int> learned = {2, 7};

    // Snapshot all current values before randomization
    std::array<float, 10> originalValues{};
    std::copy_n(state.data(), 10, originalValues.begin());

    GeneticEngine::smartRandomize(state, 1.0f, learned, rng);

    // Non-learned params (0,1,3,4,5,6,8,9) must be unchanged
    for (int i = 0; i < 10; ++i)
    {
        if (learned.count(i) == 0)
        {
            REQUIRE(state.data()[i] == Approx(originalValues[i]));
        }
    }
}

TEST_CASE("GeneticEngine::smartRandomize: amount=0 makes no changes", "[genetic]")
{
    juce::Random rng(kSeedSmartZeroAmt);
    ParameterState state = makeState(0.3f, 6);
    const std::unordered_set<int> learned = {0, 1, 2, 3, 4, 5};

    std::array<float, 6> before{};
    std::copy_n(state.data(), 6, before.begin());

    GeneticEngine::smartRandomize(state, 0.0f, learned, rng);

    for (int i = 0; i < 6; ++i)
        REQUIRE(state.data()[i] == Approx(before[i]));
}

TEST_CASE("GeneticEngine::smartRandomize: empty learned set makes no changes", "[genetic]")
{
    juce::Random rng(kSeedSmartEmptySet);
    ParameterState state = makeState(0.4f, 5);
    const std::unordered_set<int> learned;  // empty

    GeneticEngine::smartRandomize(state, 1.0f, learned, rng);

    for (int i = 0; i < 5; ++i)
        REQUIRE(state.data()[i] == Approx(0.4f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  GeneticEngine — SanityMode
// ─────────────────────────────────────────────────────────────────────────────

namespace {
    constexpr juce::int64 kSeedSanityBreed     = 88888;
    constexpr juce::int64 kSeedSanityRandomize = 77766;
    constexpr juce::int64 kSeedSanityDisabled  = 33333;
} // namespace

TEST_CASE("GeneticEngine::breed: SanityMode protects danger params", "[genetic][sanity]")
{
    juce::Random rng(kSeedSanityBreed);
    auto parentA = makeState(0.2f, 10);
    auto parentB = makeState(0.9f, 10);

    // Protect indices 0 (Volume), 3 (Pitch), 7 (Bypass)
    SanityConfig sanity;
    sanity.enabled = true;
    sanity.protectedIndices = {0, 3, 7};

    auto child = GeneticEngine::breed(parentA, parentB, 0.5f, 0.3f, rng, sanity);

    REQUIRE(child.occupied);
    REQUIRE(child.parameterCount == 10);

    // Protected indices must retain parentA's exact value
    REQUIRE(child.data()[0] == Approx(0.2f));
    REQUIRE(child.data()[3] == Approx(0.2f));
    REQUIRE(child.data()[7] == Approx(0.2f));

    // At least one non-protected index should differ from parentA
    // (crossover + mutation should modify them)
    bool anyDifferent = false;
    for (int i : {1, 2, 4, 5, 6, 8, 9})
    {
        if (std::abs(child.data()[i] - 0.2f) > 0.01f)
            anyDifferent = true;
    }
    REQUIRE(anyDifferent);
}

TEST_CASE("GeneticEngine::smartRandomize: SanityMode protects danger params", "[genetic][sanity]")
{
    juce::Random rng(kSeedSanityRandomize);
    ParameterState state = makeState(0.5f, 8);

    // All params are "learned" but indices 1 and 4 are protected
    const std::unordered_set<int> learned = {0, 1, 2, 3, 4, 5, 6, 7};
    SanityConfig sanity;
    sanity.enabled = true;
    sanity.protectedIndices = {1, 4};

    GeneticEngine::smartRandomize(state, 1.0f, learned, rng, sanity);

    // Protected indices must be unchanged
    REQUIRE(state.data()[1] == Approx(0.5f));
    REQUIRE(state.data()[4] == Approx(0.5f));
}

TEST_CASE("GeneticEngine::breed: SanityMode disabled has no effect", "[genetic][sanity]")
{
    juce::Random rng(kSeedSanityDisabled);
    auto parentA = makeState(0.2f, 8);
    auto parentB = makeState(0.8f, 8);

    SanityConfig sanity;
    sanity.enabled = false;
    sanity.protectedIndices = {0, 1, 2, 3, 4, 5, 6, 7};  // all "protected" but disabled

    auto child = GeneticEngine::breed(parentA, parentB, 0.5f, 0.0f, rng, sanity);

    // With disabled sanity and crossoverRatio=0.5, values should be ~0.5 (midpoint)
    for (int i = 0; i < child.parameterCount; ++i)
        REQUIRE(child.data()[i] == Approx(0.5f).margin(0.01f));
}

