/*
 * MorphSnap — Unit Tests: V2 Integration Pipeline
 *
 * Catch2 v3 test suite for V2 audio domain pipeline integration.
 *
 * Coverage:
 *   - Audio domain bypass: disabled V2 path behaves like V1
 *   - No Plugin B loaded: skips audio-domain processing path
 *   - Modulation engine in pipeline: runs after morph processor
 *   - Modulation output clamping: always in [0, 1]
 *   - Latency reporting: total latency sums all components
 *   - Oversampling x1 adds zero latency
 *   - Spectral engine latency calculation
 *
 * These tests verify integration contracts between V2 subsystems,
 * using the LatencyManager (already existing) and self-contained
 * mocks for the V2 audio domain components.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Existing production headers (already in the build)
#include "Core/OversamplingWrapper.h"
#include "Core/LatencyManager.h"
#include "Core/MorphProcessor.h"
#include "Core/SnapshotBank.h"
#include "Core/ModulationTypes.h"

// V2 mock interfaces (for modulation and hybrid blend)
#include "../Mocks/MockV2Interfaces.h"

#include <juce_core/juce_core.h>

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using namespace morphsnap;
using namespace morphsnap::test;

// =============================================================================
//  Helpers
// =============================================================================
namespace {

/** Compute RMS of a float vector. */
float rms(const std::vector<float>& v)
{
    if (v.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : v) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / v.size()));
}

/**
 * V2PipelineConfig — models the V2 processing mode configuration.
 * In the production engine, this would be held in the processor.
 */
struct V2PipelineConfig
{
    bool audioDomainEnabled = false;  // true = V2 spectral/granular path active
    bool pluginBLoaded      = false;  // true = a second plugin is available
    int  fftSize            = 512;
    int  hopSize            = 128;
    OversamplingFactor oversamplingFactor = OversamplingFactor::x1;
};

/**
 * V2LatencyReport — collects latency from all V2 components.
 */
struct V2LatencyReport
{
    int oversamplingLatency = 0;
    int spectralLatency     = 0;
    int hostedPluginLatency = 0;
    int total               = 0;
};

V2LatencyReport computeLatency(const V2PipelineConfig& cfg, int hostedPluginLatency = 0)
{
    V2LatencyReport report;

    // Oversampling latency
    OversamplingWrapper os;
    os.setFactor(cfg.oversamplingFactor);
    os.prepare(512, 2, 48000.0);
    report.oversamplingLatency = os.getLatencyInSamples();

    // Spectral latency (only when audio domain is active)
    if (cfg.audioDomainEnabled && cfg.pluginBLoaded)
        report.spectralLatency = cfg.fftSize / 2 + cfg.hopSize;

    report.hostedPluginLatency = hostedPluginLatency;
    report.total = report.oversamplingLatency
                 + report.spectralLatency
                 + report.hostedPluginLatency;

    return report;
}

/**
 * Simulate the V2 modulation post-processing step.
 * After morphProcessor produces a parameter vector, the modulation
 * matrix is applied and results are clamped to [0, 1].
 */
std::vector<float> applyModulation(const std::vector<float>& morphOutput,
                                   const ModulationMatrixState& matrix,
                                   const std::array<float, NUM_MOD_SOURCES>& sources)
{
    std::vector<float> result = morphOutput;
    matrix.apply(result, sources);
    // Clamp to [0, 1] — production guarantee
    for (float& v : result)
        v = std::max(0.0f, std::min(1.0f, v));
    return result;
}

} // namespace

// =============================================================================
//  V2 Audio Domain Bypass
// =============================================================================

TEST_CASE("V2 audio domain bypass: audioDomainEnabled=false behaves like V1", "[integration][v2]")
{
    // When audioDomainEnabled = false, the pipeline must produce identical output
    // to a standard V1 MorphProcessor.

    V2PipelineConfig cfg;
    cfg.audioDomainEnabled = false;
    cfg.pluginBLoaded      = false;

    const int paramCount = 8;
    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vals = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    bank.captureValues(0, vals);

    MorphProcessor proc(bank);
    proc.prepare(paramCount);
    proc.setSmoothingRate(0.0f);

    std::vector<float> out(paramCount, 0.0f);

    // Process a few blocks
    for (int i = 0; i < 10; ++i)
        proc.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct,
                     1.0f / 60.0f, out);

    // V1 behavior: output should reflect the captured snapshot
    for (int p = 0; p < paramCount; ++p)
    {
        INFO("param " << p);
        REQUIRE(out[p] == Approx(vals[p]).margin(0.01f));
    }

    SECTION("no additional latency in bypass mode")
    {
        auto latency = computeLatency(cfg);
        // With x1 oversampling and no spectral engine, total latency = 0
        REQUIRE(latency.oversamplingLatency == 0);
        REQUIRE(latency.spectralLatency     == 0);
        REQUIRE(latency.total               == 0);
    }
}

TEST_CASE("V2 audio domain bypass: no Plugin B loaded skips audio-domain path", "[integration][v2]")
{
    // When pluginBLoaded = false, even with audioDomainEnabled = true,
    // the audio-domain morph path should not activate.

    V2PipelineConfig cfg;
    cfg.audioDomainEnabled = true;
    cfg.pluginBLoaded      = false;  // No Plugin B

    // With no Plugin B, spectral latency should be zero even in V2 mode
    auto latency = computeLatency(cfg);
    REQUIRE(latency.spectralLatency == 0);
    REQUIRE(latency.total           == latency.oversamplingLatency);
}

TEST_CASE("V2 audio domain bypass: audioDomainEnabled=true with Plugin B adds spectral latency", "[integration][v2]")
{
    V2PipelineConfig cfg;
    cfg.audioDomainEnabled = true;
    cfg.pluginBLoaded      = true;
    cfg.fftSize            = 512;
    cfg.hopSize            = 128;

    auto latency = computeLatency(cfg);

    const int expectedSpectralLatency = cfg.fftSize / 2 + cfg.hopSize;
    REQUIRE(latency.spectralLatency == expectedSpectralLatency);
    REQUIRE(latency.total > 0);
}

// =============================================================================
//  V2 Modulation in Pipeline
// =============================================================================

TEST_CASE("V2 modulation in pipeline: modulation engine runs after morph processor", "[integration][v2]")
{
    // Verify the sequencing contract: morph output is produced first,
    // then modulation is applied on top.

    const int paramCount = 4;
    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vals = {0.5f, 0.5f, 0.5f, 0.5f};
    bank.captureValues(0, vals);

    MorphProcessor proc(bank);
    proc.prepare(paramCount);
    proc.setSmoothingRate(0.0f);

    // Get morph output
    std::vector<float> morphOut(paramCount, 0.0f);
    proc.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct,
                 1.0f / 60.0f, morphOut);

    // Apply modulation: LFO_1 → param 0, depth = 0.1
    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 0.1f);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)] = 1.0f;

    std::vector<float> finalOut = applyModulation(morphOut, matrix, sources);

    // Modulation adds 0.1 to param 0 (0.5 + 0.1 = 0.6)
    REQUIRE(finalOut[0] == Approx(0.6f).margin(0.01f));

    // Other params unchanged by modulation
    for (int p = 1; p < paramCount; ++p)
        REQUIRE(finalOut[p] == Approx(morphOut[p]).margin(1e-5f));
}

TEST_CASE("V2 modulation in pipeline: modulation output is clamped to [0,1]", "[integration][v2]")
{
    // Even with extreme modulation depth, the clamping guarantee must hold.
    const int paramCount = 6;

    std::vector<float> morphOut = {0.9f, 0.1f, 0.5f, 0.0f, 1.0f, 0.5f};

    ModulationMatrixState matrix;
    // Extreme positive depth on param 0 (0.9 + 2.0 = 2.9 → clamped to 1.0)
    matrix.addRoute(ModSourceId::LFO_1, 0, 2.0f);
    // Extreme negative depth on param 1 (0.1 - 2.0 = -1.9 → clamped to 0.0)
    matrix.addRoute(ModSourceId::LFO_2, 1, -2.0f);
    // Normal modulation on param 2
    matrix.addRoute(ModSourceId::Macro_1, 2, 0.3f);

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)]  = 1.0f;
    sources[static_cast<size_t>(ModSourceId::LFO_2)]  = 1.0f;
    sources[static_cast<size_t>(ModSourceId::Macro_1)]= 1.0f;

    std::vector<float> finalOut = applyModulation(morphOut, matrix, sources);

    // All outputs must be in [0, 1]
    for (int p = 0; p < paramCount; ++p)
    {
        INFO("param " << p << " = " << finalOut[p]);
        REQUIRE(finalOut[p] >= 0.0f);
        REQUIRE(finalOut[p] <= 1.0f);
    }

    // Specific expected values after clamping
    REQUIRE(finalOut[0] == Approx(1.0f).margin(1e-4f));  // clamped from 2.9
    REQUIRE(finalOut[1] == Approx(0.0f).margin(1e-4f));  // clamped from -1.9
    REQUIRE(finalOut[2] == Approx(0.8f).margin(1e-4f));  // 0.5 + 0.3 = 0.8
    REQUIRE(finalOut[3] == Approx(0.0f).margin(1e-4f));  // unchanged, already 0.0
    REQUIRE(finalOut[4] == Approx(1.0f).margin(1e-4f));  // unchanged, already 1.0
}

TEST_CASE("V2 modulation in pipeline: modulation with zero sources has no effect", "[integration][v2]")
{
    std::vector<float> morphOut = {0.3f, 0.5f, 0.7f};

    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1, 0, 0.5f);
    matrix.addRoute(ModSourceId::Macro_1, 1, 1.0f);

    // All source values = 0
    std::array<float, NUM_MOD_SOURCES> sources{};
    sources.fill(0.0f);

    std::vector<float> finalOut = applyModulation(morphOut, matrix, sources);

    // Zero sources → no modulation applied
    REQUIRE(finalOut[0] == Approx(0.3f).margin(1e-5f));
    REQUIRE(finalOut[1] == Approx(0.5f).margin(1e-5f));
    REQUIRE(finalOut[2] == Approx(0.7f).margin(1e-5f));
}

TEST_CASE("V2 modulation in pipeline: multiple routes accumulate correctly", "[integration][v2]")
{
    // Two routes targeting the same parameter should accumulate
    std::vector<float> morphOut = {0.3f};

    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::LFO_1,  0, 0.1f);  // adds 0.1 * 1.0 = 0.1
    matrix.addRoute(ModSourceId::Macro_1, 0, 0.2f); // adds 0.2 * 1.0 = 0.2

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::LFO_1)]   = 1.0f;
    sources[static_cast<size_t>(ModSourceId::Macro_1)] = 1.0f;

    std::vector<float> finalOut = applyModulation(morphOut, matrix, sources);

    // 0.3 + 0.1 + 0.2 = 0.6
    REQUIRE(finalOut[0] == Approx(0.6f).margin(1e-4f));
}

// =============================================================================
//  V2 Latency Reporting
// =============================================================================

TEST_CASE("V2 latency reporting: total latency sums all components", "[integration][v2]")
{
    LatencyManager lm;

    const int oversamplingLatency  = 64;
    const int spectralLatency      = 320;   // fftSize/2 + hopSize = 256 + 64
    const int hostedPluginLatency  = 128;

    lm.setOversamplingLatency(oversamplingLatency);
    lm.setFFTWindowLatency(spectralLatency);
    lm.setHostedPluginLatency(hostedPluginLatency);

    REQUIRE(lm.getTotal() == oversamplingLatency + spectralLatency + hostedPluginLatency);
    REQUIRE(lm.getTotal() == 512);
}

TEST_CASE("V2 latency reporting: oversampling x1 adds zero latency", "[integration][v2]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1);
    os.prepare(512, 2, 48000.0);

    REQUIRE(os.getLatencyInSamples() == 0);

    LatencyManager lm;
    lm.setOversamplingLatency(os.getLatencyInSamples());
    lm.setFFTWindowLatency(0);
    lm.setHostedPluginLatency(0);

    REQUIRE(lm.getTotal() == 0);
}

TEST_CASE("V2 latency reporting: spectral engine latency is fftSize/2 + hopSize", "[integration][v2]")
{
    const int fftSize = 1024;
    const int hopSize = 256;
    const int expectedLatency = fftSize / 2 + hopSize;

    LatencyManager lm;
    lm.setFFTWindowLatency(expectedLatency);

    REQUIRE(lm.getFFTWindowLatency() == expectedLatency);
    REQUIRE(lm.getTotal()            == expectedLatency);
}

TEST_CASE("V2 latency reporting: latency increases with oversampling factor", "[integration][v2]")
{
    const OversamplingFactor factors[] = {
        OversamplingFactor::x1,
        OversamplingFactor::x2,
        OversamplingFactor::x4,
        OversamplingFactor::x8
    };

    std::array<int, 4> latencies{};
    for (int i = 0; i < 4; ++i)
    {
        OversamplingWrapper os;
        os.setFactor(factors[i]);
        os.prepare(512, 2, 48000.0);
        latencies[static_cast<size_t>(i)] = os.getLatencyInSamples();
    }

    // Latency must increase strictly with factor
    REQUIRE(latencies[0] == 0);
    REQUIRE(latencies[1] >  latencies[0]);
    REQUIRE(latencies[2] >  latencies[1]);
    REQUIRE(latencies[3] >  latencies[2]);
}

TEST_CASE("V2 latency reporting: LatencyManager recomputes on component update", "[integration][v2]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(32);
    REQUIRE(lm.getTotal() == 32);

    lm.setFFTWindowLatency(256);
    REQUIRE(lm.getTotal() == 288);

    lm.setHostedPluginLatency(64);
    REQUIRE(lm.getTotal() == 352);

    // Removing oversampling (switching to x1) should update total
    lm.setOversamplingLatency(0);
    REQUIRE(lm.getTotal() == 320);
}

TEST_CASE("V2 latency reporting: negative latency values are clamped to zero", "[integration][v2]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(-100);
    lm.setFFTWindowLatency(-50);
    lm.setHostedPluginLatency(-200);

    REQUIRE(lm.getTotal() == 0);
    REQUIRE(lm.getOversamplingLatency() == 0);
    REQUIRE(lm.getFFTWindowLatency()    == 0);
    REQUIRE(lm.getHostedPluginLatency() == 0);
}

// =============================================================================
//  V2 Pipeline: MorphProcessor + Modulation combined
// =============================================================================

TEST_CASE("V2 pipeline: MorphProcessor output is valid input for modulation", "[integration][v2]")
{
    // Verify that the morph processor output is always in [0, 1] before
    // modulation is applied (which is a prerequisite for correct clamping).

    const int paramCount = 16;
    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vA(paramCount, 0.0f);
    std::vector<float> vB(paramCount, 1.0f);
    bank.captureValues(0, vA);
    bank.captureValues(6, vB);

    MorphProcessor proc(bank);
    proc.prepare(paramCount);
    proc.setSmoothingRate(0.5f);

    std::vector<float> morphOut(paramCount, 0.5f);

    // Test at various morph positions
    const float positions[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float x : positions)
    {
        for (int i = 0; i < 5; ++i)
            proc.process(x, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct,
                         1.0f / 60.0f, morphOut);

        INFO("x = " << x);
        for (int p = 0; p < paramCount; ++p)
        {
            REQUIRE(morphOut[p] >= 0.0f);
            REQUIRE(morphOut[p] <= 1.0f);
        }
    }
}

TEST_CASE("V2 pipeline: modulation does not propagate changes between parameter slots", "[integration][v2]")
{
    // Route only targets param 2; params 0, 1, 3 must remain at morph values
    std::vector<float> morphOut = {0.2f, 0.4f, 0.6f, 0.8f};

    ModulationMatrixState matrix;
    matrix.addRoute(ModSourceId::Macro_3, 2, 0.1f);  // Only param 2 is modulated

    std::array<float, NUM_MOD_SOURCES> sources{};
    sources[static_cast<size_t>(ModSourceId::Macro_3)] = 1.0f;

    std::vector<float> finalOut = applyModulation(morphOut, matrix, sources);

    REQUIRE(finalOut[0] == Approx(0.2f).margin(1e-5f));
    REQUIRE(finalOut[1] == Approx(0.4f).margin(1e-5f));
    REQUIRE(finalOut[2] == Approx(0.7f).margin(1e-4f));  // 0.6 + 0.1
    REQUIRE(finalOut[3] == Approx(0.8f).margin(1e-5f));
}

TEST_CASE("V2 pipeline: process() is noexcept at V1 layer", "[integration][v2]")
{
    SnapshotBank bank;
    bank.prepare(4);
    MorphProcessor proc(bank);
    proc.prepare(4);

    std::vector<float> out(4, 0.0f);
    // process() is declared noexcept in MorphProcessor.h
    STATIC_REQUIRE(noexcept(proc.process(0.f, 0.f, 0.f,
                                         MorphSource::XYPad, MorphMode::Direct,
                                         1.f / 60.f, out)));
}

TEST_CASE("V2 pipeline: V2 config with all features disabled is equivalent to V1", "[integration][v2]")
{
    V2PipelineConfig cfg;
    cfg.audioDomainEnabled    = false;
    cfg.pluginBLoaded         = false;
    cfg.oversamplingFactor    = OversamplingFactor::x1;

    auto latency = computeLatency(cfg, 0);

    // Pure V1 config: zero latency from V2 components
    REQUIRE(latency.oversamplingLatency == 0);
    REQUIRE(latency.spectralLatency     == 0);
    REQUIRE(latency.hostedPluginLatency == 0);
    REQUIRE(latency.total               == 0);
}
