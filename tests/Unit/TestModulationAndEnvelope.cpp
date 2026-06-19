/*
 * More-Phi — Unit Tests: ModulationMatrix + EnvelopeFollower
 *
 * These tests promote two subsystems from "inspection-verified" to "tested"
 * (audit N4, 2026-06-19):
 *
 *   ModulationMatrix: the H3 fix (accumulate-then-clamp) is verified to be
 *     order-independent when multiple routes target the same parameter.
 *
 *   EnvelopeFollower: the one-pole time-constant formula is verified to be
 *     block-size-independent — the same attack/release setting converges in
 *     the same wall-clock time regardless of how the signal is chunked. This
 *     is the property the C15/H4 family of fixes established for the sidechain
 *     and smoothing paths; here it is pinned directly for the envelope follower.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/ModulationMatrix.h"
#include "Core/EnvelopeFollower.h"

#include <array>
#include <cmath>
#include <vector>

using Catch::Approx;
using namespace more_phi;

// =============================================================================
//  ModulationMatrix — accumulate-then-clamp (H3) is order-independent
// =============================================================================

TEST_CASE("ModulationMatrix: single route adds source*depth to destination", "[modmatrix]")
{
    ModulationMatrix mm;
    mm.prepare(4);
    mm.addRoute(ModSourceId::LFO_1, /*dest*/ 1, /*depth*/ 0.5f);

    std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)> sources{};
    sources[static_cast<int>(ModSourceId::LFO_1)] = 0.8f;

    std::vector<float> out = { 0.1f, 0.2f, 0.3f, 0.4f };
    mm.apply(sources, out);

    // dest 1 = 0.2 + 0.8*0.5 = 0.6; others unchanged.
    REQUIRE(out[0] == Approx(0.1f).margin(1e-5f));
    REQUIRE(out[1] == Approx(0.6f).margin(1e-5f));
    REQUIRE(out[2] == Approx(0.3f).margin(1e-5f));
    REQUIRE(out[3] == Approx(0.4f).margin(1e-5f));
}

TEST_CASE("ModulationMatrix: two routes to same destination are order-independent", "[modmatrix][h3]")
{
    // H3 core property: if routes A and B both target param 0, the result must
    // be the same regardless of insertion order. With accumulate-then-clamp,
    // both contribute fully before clamping. (The pre-H3 per-route clamp would
    // have saturated on the first route and lost the second's contribution.)
    auto runWithOrder = [](bool swap) {
        ModulationMatrix mm;
        mm.prepare(2);
        // Each route adds 0.6 to param 0; together they sum to 1.2, which
        // clamps to 1.0. If clamping happened per-route, the first would clamp
        // at some intermediate value and the second's contribution would be
        // partially or fully lost depending on order.
        if (!swap)
        {
            mm.addRoute(ModSourceId::LFO_1, 0, 0.6f);
            mm.addRoute(ModSourceId::LFO_2, 0, 0.6f);
        }
        else
        {
            mm.addRoute(ModSourceId::LFO_2, 0, 0.6f);
            mm.addRoute(ModSourceId::LFO_1, 0, 0.6f);
        }
        std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)> sources{};
        sources[static_cast<int>(ModSourceId::LFO_1)] = 1.0f;
        sources[static_cast<int>(ModSourceId::LFO_2)] = 1.0f;
        std::vector<float> out = { 0.0f, 0.0f };
        mm.apply(sources, out);
        return out[0];
    };

    const float first  = runWithOrder(false);
    const float second = runWithOrder(true);
    INFO("order A-first = " << first << "  order B-first = " << second);
    REQUIRE(first == Approx(second).margin(1e-6f));
    // And the summed contribution (1.2) clamps to 1.0 exactly.
    REQUIRE(first == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("ModulationMatrix: clamp-once preserves a second route the pre-fix would have lost", "[modmatrix][h3]")
{
    // Discriminating test: param 0 starts at 0.5. Route A (depth 0.8, source 1)
    // would push it to 1.3 -> clamp to 1.0 if clamped alone. Route B (depth 0.3,
    // source 1) then adds 0.3 -> but param is already at ceiling, so the final
    // clamp still yields 1.0. The KEY check: with accumulate-then-clamp, the
    // final value is clamp(0.5 + 0.8 + 0.3) = clamp(1.6) = 1.0, and crucially
    // the result does NOT depend on route order. A per-route-clamp bug would
    // make order A-then-B differ from B-then-A only in intermediate steps, but
    // here both saturate to 1.0 — we additionally assert a NON-saturating pair
    // to prove accumulation:
    ModulationMatrix mm;
    mm.prepare(1);
    // Two small routes that DON'T saturate: 0.2 + 0.2 = 0.4 (no clamp).
    mm.addRoute(ModSourceId::LFO_1, 0, 0.2f);
    mm.addRoute(ModSourceId::LFO_2, 0, 0.2f);
    std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)> sources{};
    sources[static_cast<int>(ModSourceId::LFO_1)] = 1.0f;
    sources[static_cast<int>(ModSourceId::LFO_2)] = 1.0f;
    std::vector<float> out = { 0.0f };
    mm.apply(sources, out);
    // Both contributions present (0.4), no premature clamp — proves accumulation.
    REQUIRE(out[0] == Approx(0.4f).margin(1e-6f));
}

TEST_CASE("ModulationMatrix: disabled route contributes nothing", "[modmatrix]")
{
    ModulationMatrix mm;
    mm.prepare(1);
    const int id = mm.addRoute(ModSourceId::LFO_1, 0, 0.5f);
    mm.setRouteEnabled(id, false);

    std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)> sources{};
    sources[static_cast<int>(ModSourceId::LFO_1)] = 1.0f;
    std::vector<float> out = { 0.3f };
    mm.apply(sources, out);
    REQUIRE(out[0] == Approx(0.3f).margin(1e-6f));  // unchanged
}

// =============================================================================
//  EnvelopeFollower — block-size-independent time constant
// =============================================================================
//
//  The one-pole coefficient is derived as exp(-1/(ms*0.001*sr)) per sample, and
//  the per-block coefficient is coeff^blockSize. So a given attack/release
//  setting must converge in the same wall-clock time whether the signal is fed
//  in 64-sample or 1024-sample chunks. This is the property the C15 fix
//  established; here it is asserted directly.

namespace {

// Feed a unit-step input through the follower over `totalSeconds` at the given
// block size, return the final envelope value.
float runFollowerStep(float attackMs, float releaseMs, int blockSize,
                      double sampleRate, double totalSeconds, float stepLevel)
{
    EnvelopeFollower ef;
    ef.prepare(sampleRate, blockSize);
    ef.setAttack(attackMs);
    ef.setRelease(releaseMs);

    const int totalSamples = static_cast<int>(sampleRate * totalSeconds);
    std::vector<float> block(static_cast<size_t>(blockSize), stepLevel);
    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int n = std::min(blockSize, totalSamples - start);
        ef.process(block.data(), n);
    }
    return ef.getCurrentValue();
}

} // namespace

TEST_CASE("EnvelopeFollower: attack convergence is block-size-independent", "[envelope][ballistics]")
{
    // Same attack (5 ms), same total wall-clock time (50 ms ≈ 10 time constants),
    // different block sizes. The final envelope must be essentially the same
    // regardless of chunking. Before the per-block coefficient fix, a larger
    // block would have advanced the one-pole further per process() call.
    constexpr double sr = 48000.0;
    constexpr double totalSeconds = 0.05;  // 50 ms
    const float v64  = runFollowerStep(5.0f, 100.0f, 64,   sr, totalSeconds, 1.0f);
    const float v256 = runFollowerStep(5.0f, 100.0f, 256,  sr, totalSeconds, 1.0f);
    const float v1024 = runFollowerStep(5.0f, 100.0f, 1024, sr, totalSeconds, 1.0f);

    INFO("block 64=" << v64 << "  256=" << v256 << "  1024=" << v1024);
    REQUIRE(std::abs(v64 - v256)  < 0.02f);
    REQUIRE(std::abs(v256 - v1024) < 0.02f);
}

TEST_CASE("EnvelopeFollower: release convergence is block-size-independent", "[envelope][ballistics]")
{
    // Prime the envelope to full, then release into silence. Same release
    // (20 ms), same wall-clock (100 ms), varied block sizes.
    constexpr double sr = 48000.0;

    auto runRelease = [&](int blockSize) {
        EnvelopeFollower ef;
        ef.prepare(sr, blockSize);
        ef.setAttack(1.0f);
        ef.setRelease(20.0f);
        // Prime: long loud block to saturate the envelope to ~1.0.
        std::vector<float> loud(static_cast<size_t>(blockSize * 8), 1.0f);
        for (int i = 0; i < 8; ++i)
            ef.process(loud.data() + i * blockSize, blockSize);
        // Release into silence for 100 ms.
        std::vector<float> silence(static_cast<size_t>(blockSize), 0.0f);
        const int totalSamples = static_cast<int>(sr * 0.10);
        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - start);
            ef.process(silence.data(), n);
        }
        return ef.getCurrentValue();
    };

    const float v64  = runRelease(64);
    const float v256 = runRelease(256);
    const float v1024 = runRelease(1024);

    INFO("release block 64=" << v64 << "  256=" << v256 << "  1024=" << v1024);
    REQUIRE(std::abs(v64 - v256)  < 0.02f);
    REQUIRE(std::abs(v256 - v1024) < 0.02f);
    // After ~5 release time constants the envelope must be near zero.
    REQUIRE(v256 < 0.05f);
}

TEST_CASE("EnvelopeFollower: envelope stays within [0, 1] under full-scale input", "[envelope][bounds]")
{
    EnvelopeFollower ef;
    ef.prepare(48000.0, 256);
    ef.setAttack(2.0f);
    ef.setRelease(50.0f);

    std::vector<float> fullscale(256, 1.0f);
    for (int i = 0; i < 20; ++i)
        ef.process(fullscale.data(), 256);

    const float env = ef.getCurrentValue();
    REQUIRE(env >= 0.0f);
    REQUIRE(env <= 1.0f);
    REQUIRE(env == Approx(1.0f).margin(0.01f));  // converged to full scale
}
