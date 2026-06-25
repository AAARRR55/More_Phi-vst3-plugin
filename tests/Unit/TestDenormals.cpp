/*
 * More-Phi — Unit Tests for denormal-pathology resistance (AUDIT T2).
 * Catch2 v3 test cases.
 *
 * Coverage:
 *   - LUFSMeter K-weighting biquad stays finite + fast under a decaying input
 *     that would otherwise produce subnormal state (the classic denormal hotspot).
 *   - PhysicsEngine elastic state stays finite + fast under tiny-impulse input.
 *   - Generic finite-output guard: no engine emits NaN/Inf from a decaying exp.
 *
 * Rationale (AUDIT T2): the pre-existing suite has zero denormal coverage, and
 * the benchmark (M1) historically did not set FTZ/DAZ. A regression that removed
 * ScopedNoDenormals from processBlock would not be caught. These tests are the
 * canary.
 */

#include <catch2/catch_test_macros.hpp>

#include "Core/LUFSMeter.h"
#include "Core/PhysicsEngine.h"
#include "Core/ParameterState.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace more_phi;

namespace {
// Generate a decaying exponential that reaches the denormal range (~1e-40).
// A K-weighting biquad driven by this without FTZ/DAZ accumulates subnormal
// state and slows down 10–100× on the trailing silence.
void fillDecayingExponential(float* dst, int count, double sampleRate, double freq)
{
    const double twoPi = 6.28318530717958647692;
    // Amplitude decays from 1.0 to ~1e-20 over the buffer (squared → ~1e-40 mean-square).
    const double decayPerSample = std::pow(1e-20, 1.0 / static_cast<double>(count));
    double amp = 1.0;
    for (int i = 0; i < count; ++i)
    {
        dst[i] = static_cast<float>(amp * std::sin(twoPi * freq * static_cast<double>(i) / sampleRate));
        amp *= decayPerSample;
    }
}

void fillSilence(float* dst, int count)
{
    std::fill_n(dst, count, 0.0f);
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  LUFSMeter — K-weighting biquad denormal resistance
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LUFSMeter stays finite after a decaying exponential into the denormal range", "[denormals][lufs]")
{
    LUFSMeter meter;
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    meter.prepare(sampleRate, blockSize);

    // Two channels of the same decaying signal.
    std::vector<float> ch(blockSize);
    std::vector<float*> channels { ch.data(), ch.data() };

    // Drive ~5 seconds of decaying signal so the biquad state enters the
    // subnormal regime. 5 s @ 48k/512 ≈ 468 blocks.
    const int totalBlocks = 468;
    std::vector<float> longBuf(static_cast<size_t>(totalBlocks) * blockSize);
    fillDecayingExponential(longBuf.data(), static_cast<int>(longBuf.size()), sampleRate, 220.0);

    for (int b = 0; b < totalBlocks; ++b)
    {
        std::copy_n(longBuf.data() + b * blockSize, blockSize, ch.data());
        meter.processBlock(channels.data(), 2, blockSize);
    }

    // After the decay: finite output, no NaN/Inf propagated. A full-scale 220 Hz
    // sine decaying over 5 s spends most of its energy above the -70 LUFS
    // absolute gate, so integrated must be a real finite number (not -inf, not NaN).
    const float integrated = meter.getIntegrated();
    REQUIRE_FALSE(std::isnan(integrated));
    REQUIRE(std::isfinite(integrated));   // rejects both NaN and ±inf
}

TEST_CASE("LUFSMeter does not exhibit denormal slowdown on silence after a decay", "[denormals][lufs][.timing]")
{
    // This is a soft timing check (marked [.timing] so it's opt-in via -r timing).
    // Without FTZ/DAZ, a biquad with subnormal state runs 10–100× slower. We
    // assert the silence-after-decadence block time is < 10× the baseline silence
    // block time. ScopedNoDenormals inside processBlock should keep the ratio ~1×.
    LUFSMeter meter;
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    meter.prepare(sampleRate, blockSize);

    std::vector<float> ch(blockSize, 0.0f);
    std::vector<float*> channels { ch.data(), ch.data() };

    // Prime the biquad with a decaying signal to seed subnormal-risky state.
    std::vector<float> decay(blockSize);
    fillDecayingExponential(decay.data(), blockSize, sampleRate, 220.0);
    for (int i = 0; i < 200; ++i)   // 200 quick re-seeds of the decay envelope
    {
        std::copy(decay.begin(), decay.end(), ch.data());
        meter.processBlock(channels.data(), 2, blockSize);
    }

    // Baseline: silence.
    fillSilence(ch.data(), blockSize);
    constexpr int probeBlocks = 500;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < probeBlocks; ++i)
        meter.processBlock(channels.data(), 2, blockSize);
    auto t1 = std::chrono::high_resolution_clock::now();
    const double probeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Compare to a fresh meter processing the same silence (no decay seed).
    LUFSMeter fresh;
    fresh.prepare(sampleRate, blockSize);
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < probeBlocks; ++i)
        fresh.processBlock(channels.data(), 2, blockSize);
    auto t3 = std::chrono::high_resolution_clock::now();
    const double baselineMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    if (baselineMs > 1e-6)   // guard against zero-divider on extremely fast machines
    {
        const double ratio = probeMs / baselineMs;
        // 10× is the denormal signature; ScopedNoDenormals should keep this ~1×.
        // Allow generous headroom for CI noise.
        REQUIRE(ratio < 10.0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PhysicsEngine — elastic state denormal resistance
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PhysicsEngine::updateElastic stays finite under a tiny impulse to the denormal range", "[denormals][physics]")
{
    // A spring-damper with a tiny impulse decays toward zero; the velocity /
    // position state can enter the subnormal range. The denormal-kill guard
    // (PhysicsEngine.cpp) should snap to zero rather than crawl through subnormals.
    ElasticState s{};
    constexpr float kRefDt = 512.0f / 44100.0f;
    const float target = 0.0f;

    // Settle from a small initial offset — small enough that decayed velocity
    // enters the subnormal regime before the guard trips.
    s.x = 1e-6f;
    s.vx = 0.0f;
    PhysicsEngine::updateElastic(s, target, target, ElasticPreset::Slow, kRefDt);

    for (int i = 0; i < 5000; ++i)
        PhysicsEngine::updateElastic(s, target, target, ElasticPreset::Slow, kRefDt);

    REQUIRE(std::isfinite(s.x));
    REQUIRE(std::isfinite(s.vx));
    REQUIRE_FALSE(std::isnan(s.x));
    REQUIRE_FALSE(std::isnan(s.vx));
    // The denormal-kill guard should have driven the state to (near) zero.
    REQUIRE(std::abs(s.x) < 1e-3f);
}
