/*
 * More-Phi — Unit Tests: Modulation Engine (V2)
 *
 * Catch2 v3 test suite for the V2 modulation subsystem.
 *
 * Coverage:
 *   - LFO waveform generation: sine, triangle, saw, square, sample-and-hold
 *   - LFO phase and rate accuracy
 *   - EnvelopeFollower: tracking, attack/release times, sensitivity
 *   - StepSequencer: direction modes, step count, smoothing
 *   - ModulationMatrix: routing, depth, polarity, enable/disable, limits
 *   - ModRoute and ModulationState type contracts
 *
 * These tests are self-contained: the mock implementations live in
 * tests/Mocks/MockV2Interfaces.h. They define and validate the V2
 * modulation API contract that the production implementation must satisfy.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Core/ModulationTypes.h"
#include "../Mocks/MockV2Interfaces.h"

#include <vector>
#include <array>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <string>

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using namespace more_phi;
using namespace more_phi::test;

// =============================================================================
//  LFO Waveform Generation
// =============================================================================

TEST_CASE("LFO waveform generation: sine outputs [-1, 1] range", "[modulation][lfo]")
{
    LFOState lfo;
    lfo.prepare(44100.0);
    lfo.setShape(LFOState::Shape::Sine);

    float minVal =  2.0f;
    float maxVal = -2.0f;

    // Generate one full cycle at 1 Hz (44100 samples)
    for (int i = 0; i < 44100; ++i)
    {
        float v = lfo.tick(1.0f);
        minVal = std::min(minVal, v);
        maxVal = std::max(maxVal, v);
    }

    REQUIRE(minVal >= -1.0f - 1e-4f);
    REQUIRE(maxVal <=  1.0f + 1e-4f);
    // The range must actually reach near ±1 for a full cycle
    REQUIRE(maxVal >=  0.99f);
    REQUIRE(minVal <= -0.99f);
}

TEST_CASE("LFO waveform generation: triangle is symmetric", "[modulation][lfo]")
{
    LFOState lfo;
    lfo.prepare(10000.0);
    lfo.setShape(LFOState::Shape::Triangle);

    // Triangle at 1 Hz: quarter-cycle peak at 0.25 seconds = 2500 samples
    // It should be symmetric about 0 and reach ±1
    float maxVal = -2.0f, minVal = 2.0f;
    std::vector<float> samples(10000);
    for (int i = 0; i < 10000; ++i)
    {
        samples[static_cast<size_t>(i)] = lfo.tick(1.0f);
        maxVal = std::max(maxVal, samples[static_cast<size_t>(i)]);
        minVal = std::min(minVal, samples[static_cast<size_t>(i)]);
    }

    // Triangle must be within [-1, 1]
    REQUIRE(minVal >= -1.0f - 1e-4f);
    REQUIRE(maxVal <=  1.0f + 1e-4f);

    // Triangle must reach both extremes
    REQUIRE(maxVal >=  0.99f);
    REQUIRE(minVal <= -0.99f);

    SECTION("positive peak roughly mirrors negative peak")
    {
        // The mean of a triangle wave over a full cycle is 0
        float mean = 0.0f;
        for (float s : samples) mean += s;
        mean /= static_cast<float>(samples.size());
        REQUIRE(mean == Approx(0.0f).margin(0.01f));
    }
}

TEST_CASE("LFO waveform generation: saw ramps from -1 to 1", "[modulation][lfo]")
{
    LFOState lfo;
    lfo.prepare(10000.0);
    lfo.setShape(LFOState::Shape::Saw);

    // Generate one cycle, sample at start and just-before-wrap
    // Saw: phase 0 → -1, phase 0.999... → +1
    float atStart = lfo.tick(1.0f);  // phase ≈ 0 → value ≈ -1

    REQUIRE(atStart == Approx(-1.0f).margin(0.01f));

    // Advance to just before wrap (9999 samples into a 10000-sample cycle)
    for (int i = 1; i < 9999; ++i)
        lfo.tick(1.0f);

    float nearEnd = lfo.tick(1.0f);
    REQUIRE(nearEnd > 0.9f); // should be close to +1 just before reset

    SECTION("saw output is monotonically increasing within a cycle")
    {
        LFOState lfo2;
        lfo2.prepare(1000.0);
        lfo2.setShape(LFOState::Shape::Saw);

        float prev = lfo2.tick(1.0f);
        bool allIncreasing = true;
        for (int i = 1; i < 999; ++i)
        {
            float curr = lfo2.tick(1.0f);
            if (curr < prev - 1e-4f)
            {
                allIncreasing = false;
                break;
            }
            prev = curr;
        }
        REQUIRE(allIncreasing);
    }
}

TEST_CASE("LFO waveform generation: square is binary plus-or-minus 1", "[modulation][lfo]")
{
    LFOState lfo;
    lfo.prepare(10000.0);
    lfo.setShape(LFOState::Shape::Square);

    int positiveCount = 0;
    int negativeCount = 0;

    for (int i = 0; i < 10000; ++i)
    {
        float v = lfo.tick(1.0f);
        REQUIRE((v == Approx(1.0f) || v == Approx(-1.0f)));
        if (v > 0.0f) ++positiveCount;
        else          ++negativeCount;
    }

    // Square wave should spend roughly equal time in each state
    // (first and last sample may be asymmetric by one, so allow margin)
    REQUIRE(positiveCount > 4000);
    REQUIRE(negativeCount > 4000);
}

TEST_CASE("LFO waveform generation: sample-and-hold latches on phase wrap", "[modulation][lfo]")
{
    LFOState lfo;
    lfo.prepare(1000.0);
    lfo.setShape(LFOState::Shape::SampleAndHold);

    // Collect one full cycle
    float firstCycleValue = lfo.tick(1.0f);

    // All samples in the first cycle should have the same held value
    bool allSame = true;
    for (int i = 1; i < 999; ++i)
    {
        float v = lfo.tick(1.0f);
        if (std::abs(v - firstCycleValue) > 1e-4f)
        {
            allSame = false;
            break;
        }
    }
    REQUIRE(allSame);

    // The 1000th sample triggers a phase wrap and should produce a new value
    float newCycleValue = lfo.tick(1.0f);
    // It may or may not be different, but the held value within the new cycle
    // must remain consistent
    float secondCycleHeld = newCycleValue;
    for (int i = 1; i < 999; ++i)
    {
        float v = lfo.tick(1.0f);
        REQUIRE(v == Approx(secondCycleHeld).margin(1e-4f));
    }
}

TEST_CASE("LFO waveform generation: rate of 1 Hz completes cycle in 1 second", "[modulation][lfo]")
{
    const double sampleRate = 48000.0;
    LFOState lfo;
    lfo.prepare(sampleRate);
    lfo.setShape(LFOState::Shape::Sine);

    // At exactly 1 Hz, phase should wrap after 48000 samples
    // Check that phase starts at 0 and is near 0 again after 48000 ticks
    double phase0 = lfo.getPhase();
    REQUIRE(phase0 == Approx(0.0).margin(1e-6));

    // Advance 47999 samples
    for (int i = 0; i < 47999; ++i)
        lfo.tick(1.0f);

    // Phase should be very close to (but less than) 1.0
    double phaseBeforeWrap = lfo.getPhase();
    REQUIRE(phaseBeforeWrap > 0.99);

    // One more tick triggers the wrap
    lfo.tick(1.0f);
    double phaseAfterWrap = lfo.getPhase();
    REQUIRE(phaseAfterWrap < 0.01); // Reset to near 0
}

TEST_CASE("LFO waveform generation: tempo sync calculates correct rate from BPM", "[modulation][lfo]")
{
    // At 120 BPM, a quarter-note LFO rate = 2 Hz (120 beats/min = 2 beats/sec)
    // At 60 BPM, a quarter-note LFO rate = 1 Hz
    const float bpm120 = 120.0f;
    const float bpm60  = 60.0f;

    // Rate (Hz) = BPM / 60 * noteMultiplier
    auto bpmToHz = [](float bpm, float noteMultiplier = 1.0f) -> float
    {
        return bpm / 60.0f * noteMultiplier;
    };

    REQUIRE(bpmToHz(120.0f) == Approx(2.0f));
    REQUIRE(bpmToHz(60.0f)  == Approx(1.0f));
    REQUIRE(bpmToHz(120.0f, 0.5f) == Approx(1.0f));  // Half-note at 120 BPM
    REQUIRE(bpmToHz(120.0f, 2.0f) == Approx(4.0f));  // Eighth-note at 120 BPM

    // Verify the LFO completes correct cycles at these rates
    const double sampleRate = 48000.0;
    LFOState lfo;
    lfo.prepare(sampleRate);
    lfo.setShape(LFOState::Shape::Sine);

    // At 2 Hz, the cycle period is 24000 samples
    float rate = bpmToHz(bpm120);
    REQUIRE(rate == Approx(2.0f));

    for (int i = 0; i < 23999; ++i) lfo.tick(rate);
    double phaseBeforeWrap = lfo.getPhase();
    REQUIRE(phaseBeforeWrap > 0.99);

    (void)bpm60; // suppress unused
}

// =============================================================================
//  EnvelopeFollower
// =============================================================================

TEST_CASE("EnvelopeFollower tracking: silent input produces zero output", "[modulation][envelope]")
{
    EnvelopeFollowerState ef;
    ef.prepare(44100.0);
    ef.setAttackMs(5.0f);
    ef.setReleaseMs(50.0f);

    // Process 1000 samples of silence
    float maxOutput = 0.0f;
    for (int i = 0; i < 1000; ++i)
    {
        float v = ef.process(0.0f);
        maxOutput = std::max(maxOutput, v);
    }

    REQUIRE(maxOutput == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("EnvelopeFollower tracking: loud input produces high output", "[modulation][envelope]")
{
    EnvelopeFollowerState ef;
    ef.prepare(44100.0);
    ef.setAttackMs(1.0f);
    ef.setReleaseMs(100.0f);
    ef.setSensitivity(1.0f);

    // Process 10000 samples of full-amplitude signal — envelope must converge high
    for (int i = 0; i < 10000; ++i)
        ef.process(1.0f);

    float envelope = ef.getEnvelope();
    REQUIRE(envelope > 0.9f);
}

TEST_CASE("EnvelopeFollower tracking: attack time controls rise speed", "[modulation][envelope]")
{
    // Shorter attack should reach a higher envelope value in the same number of samples
    const int sampleCount = 200;
    const double sr = 44100.0;

    EnvelopeFollowerState efFast, efSlow;
    efFast.prepare(sr);
    efSlow.prepare(sr);
    efFast.setAttackMs(1.0f);
    efSlow.setAttackMs(50.0f);
    efFast.setReleaseMs(1000.0f);
    efSlow.setReleaseMs(1000.0f);

    for (int i = 0; i < sampleCount; ++i)
    {
        efFast.process(1.0f);
        efSlow.process(1.0f);
    }

    REQUIRE(efFast.getEnvelope() > efSlow.getEnvelope());
}

TEST_CASE("EnvelopeFollower tracking: release time controls decay speed", "[modulation][envelope]")
{
    const double sr = 44100.0;

    // Prime both followers with loud signal
    EnvelopeFollowerState efFast, efSlow;
    efFast.prepare(sr);
    efSlow.prepare(sr);
    efFast.setAttackMs(1.0f);
    efSlow.setAttackMs(1.0f);
    efFast.setReleaseMs(10.0f);
    efSlow.setReleaseMs(500.0f);

    // Prime to high level
    for (int i = 0; i < 5000; ++i)
    {
        efFast.process(1.0f);
        efSlow.process(1.0f);
    }

    // Now release — process silence
    const int releaseCount = 1000;
    for (int i = 0; i < releaseCount; ++i)
    {
        efFast.process(0.0f);
        efSlow.process(0.0f);
    }

    // Fast release should have decayed more
    REQUIRE(efFast.getEnvelope() < efSlow.getEnvelope());
}

TEST_CASE("EnvelopeFollower tracking: sensitivity scales output", "[modulation][envelope]")
{
    const double sr = 44100.0;

    EnvelopeFollowerState efFull, efHalf;
    efFull.prepare(sr);
    efHalf.prepare(sr);
    efFull.setAttackMs(1.0f);
    efHalf.setAttackMs(1.0f);
    efFull.setSensitivity(1.0f);
    efHalf.setSensitivity(0.5f);

    for (int i = 0; i < 5000; ++i)
    {
        efFull.process(1.0f);
        efHalf.process(1.0f);
    }

    // Full sensitivity should produce roughly double the output of half sensitivity
    REQUIRE(efFull.getEnvelope() > efHalf.getEnvelope() * 1.5f);
}

// =============================================================================
//  StepSequencer
// =============================================================================

TEST_CASE("StepSequencer sequencing: forward direction cycles through steps", "[modulation][stepseq]")
{
    StepSequencerState seq;
    seq.setStepCount(4);
    seq.setDirection(StepSequencerState::Direction::Forward);
    for (int i = 0; i < 4; ++i)
        seq.setStep(i, static_cast<float>(i) * 0.25f);  // 0, 0.25, 0.5, 0.75

    // First full cycle
    REQUIRE(seq.advance() == Approx(0.00f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(0.25f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(0.50f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(0.75f).margin(1e-4f));

    // Should wrap back to step 0
    REQUIRE(seq.advance() == Approx(0.00f).margin(1e-4f));
}

TEST_CASE("StepSequencer sequencing: backward direction reverses", "[modulation][stepseq]")
{
    StepSequencerState seq;
    seq.setStepCount(4);
    seq.setDirection(StepSequencerState::Direction::Backward);
    for (int i = 0; i < 4; ++i)
        seq.setStep(i, static_cast<float>(i) * 0.25f);

    // Backward starts from step 0 and goes down (wraps to 3, then 2...)
    float first  = seq.advance();  // step 0 = 0.0
    float second = seq.advance();  // step 3 = 0.75
    float third  = seq.advance();  // step 2 = 0.5

    REQUIRE(first  == Approx(0.00f).margin(1e-4f));
    REQUIRE(second == Approx(0.75f).margin(1e-4f));
    REQUIRE(third  == Approx(0.50f).margin(1e-4f));
}

TEST_CASE("StepSequencer sequencing: ping-pong reverses at boundaries", "[modulation][stepseq]")
{
    StepSequencerState seq;
    seq.setStepCount(4);
    seq.setDirection(StepSequencerState::Direction::PingPong);
    for (int i = 0; i < 4; ++i)
        seq.setStep(i, static_cast<float>(i) * 0.1f);  // 0, 0.1, 0.2, 0.3

    // Forward: 0, 1, 2, 3 then reversal
    std::vector<float> collected;
    for (int i = 0; i < 7; ++i)
        collected.push_back(seq.advance());

    // Pattern: 0, 0.1, 0.2, 0.3, 0.2, 0.1, 0.0
    REQUIRE(collected[0] == Approx(0.0f).margin(1e-4f));
    REQUIRE(collected[1] == Approx(0.1f).margin(1e-4f));
    REQUIRE(collected[2] == Approx(0.2f).margin(1e-4f));
    REQUIRE(collected[3] == Approx(0.3f).margin(1e-4f));
    // After hitting the top, it reverses
    REQUIRE(collected[4] == Approx(0.2f).margin(1e-4f));
    REQUIRE(collected[5] == Approx(0.1f).margin(1e-4f));
}

TEST_CASE("StepSequencer sequencing: step count limits range", "[modulation][stepseq]")
{
    StepSequencerState seq;
    seq.setStepCount(3);
    seq.setDirection(StepSequencerState::Direction::Forward);
    for (int i = 0; i < StepSequencerState::MAX_STEPS; ++i)
        seq.setStep(i, static_cast<float>(i));

    // With step count = 3, only steps 0, 1, 2 should cycle
    REQUIRE(seq.advance() == Approx(0.0f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(1.0f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(2.0f).margin(1e-4f));
    REQUIRE(seq.advance() == Approx(0.0f).margin(1e-4f));  // wraps at 3, not higher
}

TEST_CASE("StepSequencer sequencing: smoothing interpolates between steps", "[modulation][stepseq]")
{
    StepSequencerState seq;
    seq.setStepCount(2);
    seq.setDirection(StepSequencerState::Direction::Forward);
    seq.setStep(0, 0.0f);
    seq.setStep(1, 1.0f);
    seq.setSmoothing(0.9f);  // strong smoothing

    // With smoothing, the output should transition gradually between steps
    std::vector<float> outputs;
    for (int i = 0; i < 20; ++i)
        outputs.push_back(seq.advance());

    // First output should be near 0 (start of smoothing toward 0.0)
    REQUIRE(outputs.front() < 0.5f);

    // With strong smoothing the values should not jump abruptly between 0 and 1
    for (size_t i = 1; i < outputs.size(); ++i)
    {
        float delta = std::abs(outputs[i] - outputs[i - 1]);
        REQUIRE(delta < 0.5f);  // no instantaneous jump >= 0.5
    }
}

// =============================================================================
//  Modulation Matrix Routing
// =============================================================================

TEST_CASE("ModulationMatrix routing: empty matrix passes output unchanged", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    std::vector<float> params = {0.3f, 0.5f, 0.7f, 0.9f};
    std::vector<float> original = params;

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources.fill(1.0f);

    matrix.apply(params, sources);

    for (size_t i = 0; i < params.size(); ++i)
        REQUIRE(params[i] == Approx(original[i]));
}

TEST_CASE("ModulationMatrix routing: single route modulates target param", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    // LFO_1 → param index 2, depth = 0.1
    REQUIRE(matrix.addRoute(ModSourceId::LFO_1, 2, 0.1f));
    REQUIRE(matrix.getRouteCount() == 1);

    std::vector<float> params = {0.5f, 0.5f, 0.5f, 0.5f};
    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;  // source at full

    matrix.apply(params, sources);

    // Param 2 should have increased by depth * sourceValue = 0.1
    REQUIRE(params[2] == Approx(0.6f).margin(1e-4f));
    // Other params unchanged
    REQUIRE(params[0] == Approx(0.5f));
    REQUIRE(params[1] == Approx(0.5f));
    REQUIRE(params[3] == Approx(0.5f));
}

TEST_CASE("ModulationMatrix routing: depth controls modulation amount", "[modulation][matrix]")
{
    ModulationMatrixState matrixLow, matrixHigh;
    matrixLow.addRoute(ModSourceId::Macro_1, 0, 0.1f);
    matrixHigh.addRoute(ModSourceId::Macro_1, 0, 0.5f);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::Macro_1)] = 1.0f;

    std::vector<float> paramsLow  = {0.4f};
    std::vector<float> paramsHigh = {0.4f};

    matrixLow.apply(paramsLow, sources);
    matrixHigh.apply(paramsHigh, sources);

    REQUIRE(paramsHigh[0] > paramsLow[0]);
    REQUIRE(paramsLow[0]  == Approx(0.5f).margin(1e-4f));
    REQUIRE(paramsHigh[0] == Approx(0.9f).margin(1e-4f));
}

TEST_CASE("ModulationMatrix routing: negative depth inverts modulation", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_2, 0, -0.2f);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_2)] = 1.0f;

    std::vector<float> params = {0.5f};
    matrix.apply(params, sources);

    // Negative depth: 0.5 + (1.0 * -0.2) = 0.3
    REQUIRE(params[0] == Approx(0.3f).margin(1e-4f));
}

TEST_CASE("ModulationMatrix routing: disabled route has no effect", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 0.5f);
    matrix.setRouteEnabled(0, false);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;

    std::vector<float> params = {0.5f};
    matrix.apply(params, sources);

    REQUIRE(params[0] == Approx(0.5f).margin(1e-4f));  // unchanged
}

TEST_CASE("ModulationMatrix routing: remove route stops modulation", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 0.3f);
    REQUIRE(matrix.getRouteCount() == 1);

    matrix.removeRoute(0);
    REQUIRE(matrix.getRouteCount() == 0);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;

    std::vector<float> params = {0.5f};
    matrix.apply(params, sources);

    REQUIRE(params[0] == Approx(0.5f).margin(1e-4f));  // no modulation applied
}

TEST_CASE("ModulationMatrix routing: max 128 routes enforced", "[modulation][matrix]")
{
    ModulationMatrixState matrix;

    // Fill to capacity
    int successCount = 0;
    for (int i = 0; i < 200; ++i)
    {
        if (matrix.addRoute(ModSourceId::LFO_1, 0, 0.01f))
            ++successCount;
    }

    // Exactly 128 routes should have succeeded
    REQUIRE(successCount == ModulationState::MAX_ROUTES);
    REQUIRE(matrix.getRouteCount() == ModulationState::MAX_ROUTES);
}

TEST_CASE("ModulationMatrix routing: out-of-range dest param ignored safely", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 9999, 0.5f);  // destParamIndex way out of range

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;

    std::vector<float> params = {0.5f, 0.5f};

    // Must not crash; params must be unchanged
    REQUIRE_NOTHROW(matrix.apply(params, sources));
    REQUIRE(params[0] == Approx(0.5f));
    REQUIRE(params[1] == Approx(0.5f));
}

// =============================================================================
//  ModulationEngine Integration
// =============================================================================

TEST_CASE("ModulationEngine integration: processBlock with no routes is identity", "[modulation][integration]")
{
    // With zero routes, applying the matrix must leave params at their initial values
    ModulationMatrixState matrix;
    REQUIRE(matrix.getRouteCount() == 0);

    std::vector<float> params = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    std::vector<float> original = params;

    std::array<float, NUM_MOD_SOURCES> sources{};
    for (auto& s : sources) s = 0.5f;  // sources all at 0.5

    matrix.apply(params, sources);

    for (size_t i = 0; i < params.size(); ++i)
        REQUIRE(params[i] == Approx(original[i]));
}

TEST_CASE("ModulationEngine integration: LFO routed to param oscillates output", "[modulation][integration]")
{
    // Simulate: LFO sine at 1 Hz modulating param 0 with depth 0.2
    const double sampleRate = 1000.0;  // Low rate for fast simulation
    LFOState lfo;
    lfo.prepare(sampleRate);
    lfo.setShape(LFOState::Shape::Sine);

    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 0.2f);

    const float baseValue = 0.5f;
    float minOutput =  2.0f;
    float maxOutput = -2.0f;

    std::array<float, NUM_MOD_SOURCES> sources{};

    // Process one full 1 Hz cycle = 1000 samples
    for (int i = 0; i < 1000; ++i)
    {
        float lfoOut = lfo.tick(1.0f);  // 1 Hz sine
        sources[static_cast<size_t>(ModSourceId::LFO_1)] = lfoOut;

        std::vector<float> params = {baseValue};
        matrix.apply(params, sources);

        minOutput = std::min(minOutput, params[0]);
        maxOutput = std::max(maxOutput, params[0]);
    }

    // Output should oscillate between ~0.3 and ~0.7 (baseValue ± depth)
    REQUIRE(maxOutput > 0.65f);
    REQUIRE(minOutput < 0.35f);
    REQUIRE(maxOutput <= 1.0f);
    REQUIRE(minOutput >= 0.0f);
}

TEST_CASE("ModulationEngine integration: macro knob scales modulation", "[modulation][integration]")
{
    // Macro source at 0.0 → no modulation effect; at 1.0 → full depth applied
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::Macro_1, 0, 0.4f);

    std::array<float, NUM_MOD_SOURCES> sourcesFull{}, sourcesZero{};
    sourcesFull[static_cast<size_t>(ModSourceId::Macro_1)] = 1.0f;
    sourcesZero[static_cast<size_t>(ModSourceId::Macro_1)] = 0.0f;

    std::vector<float> paramsFull = {0.5f};
    std::vector<float> paramsZero = {0.5f};

    matrix.apply(paramsFull, sourcesFull);
    matrix.apply(paramsZero, sourcesZero);

    REQUIRE(paramsFull[0] == Approx(0.9f).margin(1e-4f));
    REQUIRE(paramsZero[0] == Approx(0.5f).margin(1e-4f));
}

TEST_CASE("ModulationEngine integration: MIDI velocity modulates target", "[modulation][integration]")
{
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::MIDIVelocity, 1, 0.5f);

    std::array<float, NUM_MOD_SOURCES> sourcesSoft{}, sourcesHard{};
    sourcesSoft[static_cast<size_t>(ModSourceId::MIDIVelocity)] = 0.2f;  // soft note
    sourcesHard[static_cast<size_t>(ModSourceId::MIDIVelocity)] = 1.0f;  // hard note

    std::vector<float> paramsSoft  = {0.5f, 0.3f};
    std::vector<float> paramsHard  = {0.5f, 0.3f};

    matrix.apply(paramsSoft, sourcesSoft);
    matrix.apply(paramsHard, sourcesHard);

    // Hard velocity should produce larger modulation on param 1
    REQUIRE(paramsHard[1] > paramsSoft[1]);
    REQUIRE(paramsHard[1] == Approx(0.3f + 1.0f * 0.5f).margin(1e-4f));
    REQUIRE(paramsSoft[1] == Approx(0.3f + 0.2f * 0.5f).margin(1e-4f));
}

TEST_CASE("ModulationEngine integration: serialization round-trip preserves routes", "[modulation][integration]")
{
    // Verify that ModulationState can be captured and restored correctly.
    // This simulates the serialization contract without file I/O.
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1,    0, 0.3f);
    matrix.addRoute(ModSourceId::Macro_2,  4, -0.5f);
    matrix.addRoute(ModSourceId::StepSeq_1, 7, 0.1f);
    REQUIRE(matrix.getRouteCount() == 3);

    // "Serialize" by reading out all routes
    struct SerializedRoute
    {
        ModSourceId source;
        int         destParamIndex;
        float       depth;
        bool        enabled;
    };

    std::vector<SerializedRoute> serialized;
    for (int i = 0; i < matrix.getRouteCount(); ++i)
    {
        const auto& r = matrix.getRoute(i);
        serialized.push_back({r.source, r.destParamIndex, r.depth, r.enabled});
    }

    // "Deserialize" into a fresh matrix
    ModulationMatrixState restored;
    for (const auto& sr : serialized)
        restored.addRoute(sr.source, sr.destParamIndex, sr.depth);

    REQUIRE(restored.getRouteCount() == 3);
    REQUIRE(restored.getRoute(0).source         == ModSourceId::LFO_1);
    REQUIRE(restored.getRoute(0).destParamIndex == 0);
    REQUIRE(restored.getRoute(0).depth          == Approx(0.3f));
    REQUIRE(restored.getRoute(1).source         == ModSourceId::Macro_2);
    REQUIRE(restored.getRoute(1).destParamIndex == 4);
    REQUIRE(restored.getRoute(1).depth          == Approx(-0.5f));
    REQUIRE(restored.getRoute(2).source         == ModSourceId::StepSeq_1);
    REQUIRE(restored.getRoute(2).destParamIndex == 7);
    REQUIRE(restored.getRoute(2).depth          == Approx(0.1f));
}

// =============================================================================
//  ModulationState type contracts
// =============================================================================

TEST_CASE("ModulationState: MAX_ROUTES is 128", "[modulation][types]")
{
    STATIC_REQUIRE(ModulationState::MAX_ROUTES == 128);
}

TEST_CASE("ModulationState: default construction has zero active routes", "[modulation][types]")
{
    ModulationState state;
    REQUIRE(state.activeRouteCount == 0);
}

TEST_CASE("ModRoute: default construction is unassigned and disabled", "[modulation][types]")
{
    ModRoute route;
    REQUIRE(route.destParamIndex == -1);
    REQUIRE(route.depth          == Approx(0.0f));
    REQUIRE_FALSE(route.enabled);
}

TEST_CASE("ModSourceId: NUM_SOURCES equals number of enum members", "[modulation][types]")
{
    // Check that NUM_MOD_SOURCES is consistent with the enum count
    REQUIRE(NUM_MOD_SOURCES == static_cast<int>(ModSourceId::NUM_SOURCES));
    REQUIRE(NUM_MOD_SOURCES > 0);

    // Verify specific enum values are stable (for serialization)
    REQUIRE(static_cast<int>(ModSourceId::LFO_1)       == 0);
    REQUIRE(static_cast<int>(ModSourceId::Envelope_1)  == 4);
    REQUIRE(static_cast<int>(ModSourceId::Macro_1)     == 6);
    REQUIRE(static_cast<int>(ModSourceId::MIDIVelocity)== static_cast<int>(ModSourceId::MIDIVelocity));
}

TEST_CASE("ModulationMatrix: clearAllRoutes resets to zero routes", "[modulation][matrix]")
{
    ModulationMatrixState matrix;
    for (int i = 0; i < 10; ++i)
        matrix.addRoute(ModSourceId::LFO_1, i, 0.1f);

    REQUIRE(matrix.getRouteCount() == 10);
    matrix.clearAllRoutes();
    REQUIRE(matrix.getRouteCount() == 0);
}

TEST_CASE("ModulationMatrix: output is always clamped to [0, 1]", "[modulation][matrix]")
{
    // Route with extreme depth that would push param beyond 1.0
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 2.0f);   // depth > 1 would exceed [0,1] without clamp

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;

    std::vector<float> params = {0.9f};
    matrix.apply(params, sources);

    REQUIRE(params[0] <= 1.0f);
    REQUIRE(params[0] >= 0.0f);

    // Also test negative clamp
    matrix.clearAllRoutes();
    matrix.addRoute(ModSourceId::LFO_1, 0, -2.0f);
    params = {0.1f};
    matrix.apply(params, sources);

    REQUIRE(params[0] <= 1.0f);
    REQUIRE(params[0] >= 0.0f);
}
