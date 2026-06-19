/*
 * More-Phi — Unit Tests: Granular Morph Engine (V2)
 *
 * Catch2 v3 test suite for V2 granular processing subsystems.
 *
 * Coverage:
 *   - GrainPool: activation, deactivation, capacity, enumeration
 *   - GrainPool: Hann envelope shape at boundary positions
 *   - GranularMorphEngine: inactive bypass, alpha=0/1 grain selection
 *   - GranularMorphEngine: density and grain size controls
 *   - GranularMorphEngine: amplitude bounds
 *   - HybridBlend: weight-based blending, equal-power crossfade
 *
 * These tests use the self-contained mock implementations from
 * tests/Mocks/MockV2Interfaces.h. They validate the V2 granular API
 * contract before any production implementation exists.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../Mocks/MockV2Interfaces.h"
#include "Core/GranularMorphEngine.h"

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <random>
#include <cassert>

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using namespace more_phi::test;

// =============================================================================
//  DSP Helpers
// =============================================================================
namespace {

constexpr float kPi = 3.14159265358979f;

float rms(const std::vector<float>& buf)
{
    if (buf.empty()) return 0.0f;
    double acc = 0.0;
    for (float v : buf) acc += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(acc / buf.size()));
}

void fillSine(std::vector<float>& buf, float freqHz, float sampleRate)
{
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = std::sin(2.0f * kPi * freqHz * static_cast<float>(i) / sampleRate);
}

// ---------------------------------------------------------------------------
// GranularMorphEngine — self-contained mock for testing
// ---------------------------------------------------------------------------

/**
 * Models the expected GranularMorphEngine production interface.
 *
 * Algorithm:
 *   - Maintains a GrainPool of up to 128 concurrent grains.
 *   - Each grain has a sourceMix [0=A, 1=B], amplitude, and lifetime.
 *   - New grains are spawned according to grainDensity (grains/sec).
 *   - Grains are shaped with a Hann window envelope over grainSize samples.
 *   - Output = sum of active grain contributions.
 *
 * Thread safety: Not thread-safe — call from a single thread in tests.
 */
class GranularMorphEngine
{
public:
    static constexpr int DEFAULT_GRAIN_SIZE = 512;

    void prepare(int maxBlockSize, double sampleRate)
    {
        sampleRate_   = sampleRate;
        maxBlockSize_ = maxBlockSize;
        pool_.reset();
        samplesSinceLastGrain_ = 0;
        prepared_     = true;
    }

    void setActive(bool active)      { active_   = active; }
    void setAlpha(float alpha)       { alpha_    = std::max(0.0f, std::min(1.0f, alpha)); }
    void setGrainSize(int samples)   { grainSize_= std::max(1, samples); }
    void setGrainDensity(float d)    { density_  = std::max(0.0f, d); }

    bool  isActive()     const { return active_; }
    float getAlpha()     const { return alpha_;  }
    int   getGrainSize() const { return grainSize_; }

    /**
     * Process a block of samples.
     * When inactive: copies srcA directly to dst.
     * When active: spawns grains from srcA/srcB mix and accumulates them.
     *
     * @param srcA      Source A buffer (numSamples)
     * @param srcB      Source B buffer (numSamples)
     * @param dst       Output buffer (numSamples, zeroed)
     * @param numSamples Number of samples to process
     */
    void process(const std::vector<float>& srcA,
                 const std::vector<float>& srcB,
                 std::vector<float>& dst,
                 int numSamples)
    {
        assert(prepared_);
        dst.assign(static_cast<size_t>(numSamples), 0.0f);

        if (!active_)
        {
            // Bypass: copy source A
            const int copyCount = std::min(numSamples, static_cast<int>(srcA.size()));
            std::copy_n(srcA.begin(), static_cast<size_t>(copyCount), dst.begin());
            return;
        }

        // Determine inter-grain interval in samples
        const float grainIntervalSamples = (density_ > 0.0f)
            ? static_cast<float>(sampleRate_) / density_
            : static_cast<float>(numSamples + 1); // no spawning

        // Process sample by sample for the test mock
        for (int i = 0; i < numSamples; ++i)
        {
            // Spawn a new grain if the interval has elapsed
            ++samplesSinceLastGrain_;
            if (static_cast<float>(samplesSinceLastGrain_) >= grainIntervalSamples)
            {
                samplesSinceLastGrain_ = 0;
                int idx = pool_.activate();
                if (idx >= 0)
                {
                    GrainDescriptor& g = pool_.get(idx);
                    g.grainSize  = grainSize_;
                    g.currentAge = 0;
                    g.sourceMix  = alpha_;  // 0=A, 1=B
                    g.amplitude  = 0.7f;    // fixed for mock
                }
            }

            // Accumulate active grains
            std::vector<int> toDeactivate;
            pool_.forEachActive([&](int gIdx, GrainDescriptor& g) {
                if (g.currentAge >= g.grainSize)
                {
                    toDeactivate.push_back(gIdx);
                    return;
                }

                float normPos = static_cast<float>(g.currentAge) /
                                static_cast<float>(g.grainSize);
                float envelope = GrainPool::hannEnvelope(normPos);

                // Read from source A or B proportionally
                float srcASample = (i < static_cast<int>(srcA.size())) ? srcA[static_cast<size_t>(i)] : 0.0f;
                float srcBSample = (i < static_cast<int>(srcB.size())) ? srcB[static_cast<size_t>(i)] : 0.0f;
                float grainSample = srcASample * (1.0f - g.sourceMix) + srcBSample * g.sourceMix;

                dst[static_cast<size_t>(i)] += g.amplitude * envelope * grainSample;
                ++g.currentAge;
            });

            for (int gIdx : toDeactivate)
                pool_.deactivate(gIdx);
        }
    }

    GrainPool& getPool() { return pool_; }

private:
    GrainPool pool_;
    double    sampleRate_             = 44100.0;
    int       maxBlockSize_           = 512;
    float     alpha_                  = 0.0f;
    float     density_                = 10.0f;  // grains/sec
    int       grainSize_              = DEFAULT_GRAIN_SIZE;
    bool      active_                 = false;
    bool      prepared_               = false;
    int       samplesSinceLastGrain_  = 0;
};

// ---------------------------------------------------------------------------
// HybridBlend — blends parameter, spectral, and granular output buffers
// ---------------------------------------------------------------------------

/**
 * Blends three output buffers with configurable weights.
 * Also provides a two-source linear and equal-power crossfade.
 */
class HybridBlend
{
public:
    /**
     * Blend three source buffers with explicit weights.
     * Weights need not sum to 1; the result is a weighted sum.
     *
     * @param paramBuf    Parameter (direct interpolation) buffer
     * @param spectralBuf Spectral morph output buffer
     * @param granularBuf Granular morph output buffer
     * @param dst         Output buffer (same size as inputs)
     * @param wParam      Weight for parameter buffer
     * @param wSpectral   Weight for spectral buffer
     * @param wGranular   Weight for granular buffer
     */
    static void blend(const std::vector<float>& paramBuf,
                      const std::vector<float>& spectralBuf,
                      const std::vector<float>& granularBuf,
                      std::vector<float>& dst,
                      float wParam, float wSpectral, float wGranular)
    {
        const size_t N = paramBuf.size();
        dst.resize(N);
        for (size_t i = 0; i < N; ++i)
        {
            float p = (i < paramBuf.size())    ? paramBuf[i]    : 0.0f;
            float s = (i < spectralBuf.size()) ? spectralBuf[i] : 0.0f;
            float g = (i < granularBuf.size()) ? granularBuf[i] : 0.0f;
            dst[i] = p * wParam + s * wSpectral + g * wGranular;
        }
    }

    /**
     * Linear two-source blend: dst = bufA * (1-alpha) + bufB * alpha.
     */
    static void blendTwo(const std::vector<float>& bufA,
                         const std::vector<float>& bufB,
                         std::vector<float>& dst,
                         float alpha)
    {
        const size_t N = bufA.size();
        dst.resize(N);
        for (size_t i = 0; i < N; ++i)
        {
            float a = (i < bufA.size()) ? bufA[i] : 0.0f;
            float b = (i < bufB.size()) ? bufB[i] : 0.0f;
            dst[i] = a * (1.0f - alpha) + b * alpha;
        }
    }

    /**
     * Equal-power two-source crossfade.
     * At alpha=0.5, both inputs are weighted by sin(pi/4) = sqrt(2)/2 ≈ 0.707.
     */
    static void blendEqualPower(const std::vector<float>& bufA,
                                const std::vector<float>& bufB,
                                std::vector<float>& dst,
                                float alpha)
    {
        const size_t N = bufA.size();
        dst.resize(N);
        const float angle = alpha * (kPi / 2.0f);
        const float wA    = std::cos(angle);
        const float wB    = std::sin(angle);

        for (size_t i = 0; i < N; ++i)
        {
            float a = (i < bufA.size()) ? bufA[i] : 0.0f;
            float b = (i < bufB.size()) ? bufB[i] : 0.0f;
            dst[i] = a * wA + b * wB;
        }
    }
};

} // namespace

// =============================================================================
//  GrainPool Tests
// =============================================================================

TEST_CASE("GrainPool management: all grains initially inactive", "[granular][pool]")
{
    GrainPool pool;
    REQUIRE(pool.countActive() == 0);
    for (int i = 0; i < GrainPool::MAX_GRAINS; ++i)
        REQUIRE_FALSE(pool.isActive(i));
}

TEST_CASE("GrainPool management: activate returns valid index", "[granular][pool]")
{
    GrainPool pool;
    int idx = pool.activate();
    REQUIRE(idx >= 0);
    REQUIRE(idx < GrainPool::MAX_GRAINS);
    REQUIRE(pool.isActive(idx));
    REQUIRE(pool.countActive() == 1);
}

TEST_CASE("GrainPool management: pool returns -1 when exhausted (128 grains)", "[granular][pool]")
{
    GrainPool pool;

    // Activate all 128 grains
    int successCount = 0;
    for (int i = 0; i < GrainPool::MAX_GRAINS; ++i)
    {
        int idx = pool.activate();
        if (idx >= 0) ++successCount;
    }
    REQUIRE(successCount == GrainPool::MAX_GRAINS);
    REQUIRE(pool.countActive() == GrainPool::MAX_GRAINS);

    // Next activation should fail
    int overflowIdx = pool.activate();
    REQUIRE(overflowIdx == -1);
}

TEST_CASE("GrainPool management: deactivate frees slot for reuse", "[granular][pool]")
{
    GrainPool pool;

    // Fill the pool
    for (int i = 0; i < GrainPool::MAX_GRAINS; ++i)
        pool.activate();

    REQUIRE(pool.activate() == -1);  // Should be full

    // Deactivate one grain
    pool.deactivate(0);
    REQUIRE_FALSE(pool.isActive(0));

    // Now there should be a free slot
    int newIdx = pool.activate();
    REQUIRE(newIdx >= 0);
}

TEST_CASE("GrainPool management: forEachActive visits only active grains", "[granular][pool]")
{
    GrainPool pool;

    // Activate grains 3, 7, 15
    pool.activate();  // index 0 or first free
    pool.activate();
    pool.activate();

    // Mark specific grains active by activating then configuring them
    pool.reset();

    // Activate 5 grains
    for (int i = 0; i < 5; ++i)
        pool.activate();

    REQUIRE(pool.countActive() == 5);

    int visitCount = 0;
    pool.forEachActive([&](int /*idx*/, GrainDescriptor& /*g*/) {
        ++visitCount;
    });

    REQUIRE(visitCount == 5);
}

TEST_CASE("GrainPool management: deactivate out-of-range index is safe", "[granular][pool]")
{
    GrainPool pool;
    REQUIRE_NOTHROW(pool.deactivate(-1));
    REQUIRE_NOTHROW(pool.deactivate(GrainPool::MAX_GRAINS));
    REQUIRE_NOTHROW(pool.deactivate(9999));
    REQUIRE(pool.countActive() == 0);  // unchanged
}

TEST_CASE("GrainPool management: envelope is Hann-shaped at boundaries", "[granular][pool]")
{
    // Hann window: at position 0 and 1, value = 0; at position 0.5, value = 1
    REQUIRE(GrainPool::hannEnvelope(0.0f) == Approx(0.0f).margin(1e-5f));
    REQUIRE(GrainPool::hannEnvelope(1.0f) == Approx(0.0f).margin(1e-5f));
    REQUIRE(GrainPool::hannEnvelope(0.5f) == Approx(1.0f).margin(1e-5f));

    SECTION("Hann window is symmetric about 0.5")
    {
        for (float t = 0.0f; t <= 0.5f; t += 0.05f)
        {
            float left  = GrainPool::hannEnvelope(t);
            float right = GrainPool::hannEnvelope(1.0f - t);
            REQUIRE(left == Approx(right).margin(1e-5f));
        }
    }

    SECTION("Hann window is non-negative everywhere")
    {
        for (float t = 0.0f; t <= 1.0f; t += 0.01f)
            REQUIRE(GrainPool::hannEnvelope(t) >= -1e-6f);
    }
}

TEST_CASE("GrainPool management: reset clears all active grains", "[granular][pool]")
{
    GrainPool pool;
    for (int i = 0; i < 64; ++i)
        pool.activate();

    REQUIRE(pool.countActive() == 64);
    pool.reset();
    REQUIRE(pool.countActive() == 0);
}

// =============================================================================
//  GranularMorphEngine Tests
// =============================================================================

TEST_CASE("GranularMorphEngine processing: inactive engine passes audio unchanged", "[granular][dsp]")
{
    GranularMorphEngine engine;
    engine.prepare(512, 44100.0);
    engine.setActive(false);

    std::vector<float> srcA(128, 0.6f);
    std::vector<float> srcB(128, 0.2f);
    std::vector<float> dst;

    engine.process(srcA, srcB, dst, 128);

    REQUIRE(static_cast<int>(dst.size()) == 128);
    for (float s : dst)
        REQUIRE(s == Approx(0.6f).margin(1e-4f));
}

TEST_CASE("GranularMorphEngine processing: alpha=0 produces only source A grains", "[granular][dsp]")
{
    GranularMorphEngine engine;
    engine.prepare(512, 44100.0);
    engine.setActive(true);
    engine.setAlpha(0.0f);
    engine.setGrainSize(64);
    engine.setGrainDensity(500.0f);

    // Source A = 1.0, Source B = 0.0
    std::vector<float> srcA(512, 1.0f);
    std::vector<float> srcB(512, 0.0f);
    std::vector<float> dst;

    // Process several blocks to let grains stabilize
    for (int block = 0; block < 5; ++block)
        engine.process(srcA, srcB, dst, 256);

    // With alpha=0, only source A grains contribute — output must be non-zero
    float outputRMS = rms(dst);
    REQUIRE(outputRMS > 0.0f);

    // Since srcB = 0, and alpha=0 means only source A is used,
    // the output should carry source A energy
    REQUIRE(outputRMS > 0.01f);
}

TEST_CASE("GranularMorphEngine processing: alpha=1 produces only source B grains", "[granular][dsp]")
{
    GranularMorphEngine engine;
    engine.prepare(512, 44100.0);
    engine.setActive(true);
    engine.setAlpha(1.0f);
    engine.setGrainSize(64);
    engine.setGrainDensity(500.0f);

    // Source A = 0.0, Source B = 1.0
    std::vector<float> srcA(512, 0.0f);
    std::vector<float> srcB(512, 1.0f);
    std::vector<float> dst;

    for (int block = 0; block < 5; ++block)
        engine.process(srcA, srcB, dst, 256);

    // With alpha=1, only source B grains contribute
    float outputRMS = rms(dst);
    REQUIRE(outputRMS > 0.01f);
}

TEST_CASE("GranularMorphEngine processing: grain density controls output activity", "[granular][dsp]")
{
    // Higher density should produce more simultaneous grains, leading to higher output energy

    GranularMorphEngine engineLow, engineHigh;
    engineLow.prepare(1024, 44100.0);
    engineHigh.prepare(1024, 44100.0);
    engineLow.setActive(true);
    engineHigh.setActive(true);
    engineLow.setAlpha(0.5f);
    engineHigh.setAlpha(0.5f);
    engineLow.setGrainSize(32);
    engineHigh.setGrainSize(32);
    engineLow.setGrainDensity(5.0f);    // sparse
    engineHigh.setGrainDensity(100.0f); // dense

    std::vector<float> srcA(1024, 0.5f);
    std::vector<float> srcB(1024, 0.5f);
    std::vector<float> dstLow, dstHigh;

    for (int block = 0; block < 5; ++block)
    {
        engineLow.process(srcA, srcB, dstLow, 1024);
        engineHigh.process(srcA, srcB, dstHigh, 1024);
    }

    float rmsLow  = rms(dstLow);
    float rmsHigh = rms(dstHigh);

    // Dense engine should produce higher RMS output
    REQUIRE(rmsHigh > rmsLow);
}

TEST_CASE("GranularMorphEngine processing: grain size affects temporal resolution", "[granular][dsp]")
{
    // Smaller grains = higher temporal resolution = more zero-crossings
    // We just verify the engine accepts different grain sizes without crashing

    GranularMorphEngine engine;
    engine.prepare(512, 44100.0);
    engine.setActive(true);
    engine.setAlpha(0.5f);

    std::vector<float> srcA(512, 0.3f);
    std::vector<float> srcB(512, 0.3f);
    std::vector<float> dst;

    for (int grainSize : {16, 64, 256, 512})
    {
        INFO("grain size = " << grainSize);
        engine.setGrainSize(grainSize);
        REQUIRE_NOTHROW(engine.process(srcA, srcB, dst, 256));

        // Output must be finite
        for (float s : dst)
            REQUIRE(std::isfinite(s));
    }
}

TEST_CASE("GranularMorphEngine processing: output stays within reasonable amplitude bounds", "[granular][dsp]")
{
    GranularMorphEngine engine;
    engine.prepare(512, 44100.0);
    engine.setActive(true);
    engine.setAlpha(0.5f);
    engine.setGrainSize(64);
    engine.setGrainDensity(50.0f);

    // Unit amplitude inputs
    std::vector<float> srcA(512, 1.0f);
    std::vector<float> srcB(512, 1.0f);
    std::vector<float> dst;

    float peakOutput = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        engine.process(srcA, srcB, dst, 256);
        for (float s : dst)
            peakOutput = std::max(peakOutput, std::abs(s));
    }

    // Even with many concurrent grains, the Hann window limits individual grain amplitude.
    // A reasonable bound is 20x the input amplitude (very conservative for many grains).
    REQUIRE(peakOutput < 20.0f);
}

// =============================================================================
//  HybridBlend Tests
// =============================================================================

TEST_CASE("HybridBlend: weight (1,0,0) outputs parameter buffer only", "[granular][blend]")
{
    std::vector<float> paramBuf    = {0.1f, 0.2f, 0.3f};
    std::vector<float> spectralBuf = {0.5f, 0.5f, 0.5f};
    std::vector<float> granularBuf = {0.9f, 0.9f, 0.9f};
    std::vector<float> dst;

    HybridBlend::blend(paramBuf, spectralBuf, granularBuf, dst, 1.0f, 0.0f, 0.0f);

    REQUIRE(dst.size() == 3);
    REQUIRE(dst[0] == Approx(0.1f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(0.2f).margin(1e-5f));
    REQUIRE(dst[2] == Approx(0.3f).margin(1e-5f));
}

TEST_CASE("HybridBlend: weight (0,1,0) outputs spectral buffer only", "[granular][blend]")
{
    std::vector<float> paramBuf    = {0.1f, 0.2f, 0.3f};
    std::vector<float> spectralBuf = {0.5f, 0.6f, 0.7f};
    std::vector<float> granularBuf = {0.9f, 0.9f, 0.9f};
    std::vector<float> dst;

    HybridBlend::blend(paramBuf, spectralBuf, granularBuf, dst, 0.0f, 1.0f, 0.0f);

    REQUIRE(dst[0] == Approx(0.5f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(0.6f).margin(1e-5f));
    REQUIRE(dst[2] == Approx(0.7f).margin(1e-5f));
}

TEST_CASE("HybridBlend: weight (0,0,1) outputs granular buffer only", "[granular][blend]")
{
    std::vector<float> paramBuf    = {0.1f, 0.2f, 0.3f};
    std::vector<float> spectralBuf = {0.5f, 0.5f, 0.5f};
    std::vector<float> granularBuf = {0.8f, 0.7f, 0.6f};
    std::vector<float> dst;

    HybridBlend::blend(paramBuf, spectralBuf, granularBuf, dst, 0.0f, 0.0f, 1.0f);

    REQUIRE(dst[0] == Approx(0.8f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(0.7f).margin(1e-5f));
    REQUIRE(dst[2] == Approx(0.6f).margin(1e-5f));
}

TEST_CASE("HybridBlend: equal weights produces weighted average", "[granular][blend]")
{
    std::vector<float> paramBuf    = {0.3f, 0.3f};
    std::vector<float> spectralBuf = {0.6f, 0.6f};
    std::vector<float> granularBuf = {0.9f, 0.9f};
    std::vector<float> dst;

    const float w = 1.0f / 3.0f;
    HybridBlend::blend(paramBuf, spectralBuf, granularBuf, dst, w, w, w);

    // Expected = (0.3 + 0.6 + 0.9) / 3 = 0.6
    REQUIRE(dst[0] == Approx(0.6f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(0.6f).margin(1e-5f));
}

TEST_CASE("HybridBlend: blendTwo with alpha=0 outputs bufA", "[granular][blend]")
{
    std::vector<float> bufA = {1.0f, 2.0f, 3.0f};
    std::vector<float> bufB = {7.0f, 8.0f, 9.0f};
    std::vector<float> dst;

    HybridBlend::blendTwo(bufA, bufB, dst, 0.0f);

    REQUIRE(dst[0] == Approx(1.0f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(2.0f).margin(1e-5f));
    REQUIRE(dst[2] == Approx(3.0f).margin(1e-5f));
}

TEST_CASE("HybridBlend: blendTwo with alpha=1 outputs bufB", "[granular][blend]")
{
    std::vector<float> bufA = {1.0f, 2.0f, 3.0f};
    std::vector<float> bufB = {7.0f, 8.0f, 9.0f};
    std::vector<float> dst;

    HybridBlend::blendTwo(bufA, bufB, dst, 1.0f);

    REQUIRE(dst[0] == Approx(7.0f).margin(1e-5f));
    REQUIRE(dst[1] == Approx(8.0f).margin(1e-5f));
    REQUIRE(dst[2] == Approx(9.0f).margin(1e-5f));
}

TEST_CASE("HybridBlend: blendTwo with alpha=0.5 produces linear midpoint", "[granular][blend]")
{
    std::vector<float> bufA = {0.0f, 0.0f, 0.0f};
    std::vector<float> bufB = {1.0f, 1.0f, 1.0f};
    std::vector<float> dst;

    HybridBlend::blendTwo(bufA, bufB, dst, 0.5f);

    for (float v : dst)
        REQUIRE(v == Approx(0.5f).margin(1e-5f));
}

TEST_CASE("HybridBlend: equal-power: sqrt weighting at crossfade midpoint", "[granular][blend]")
{
    // At alpha=0.5, equal-power blend uses weights cos(pi/4) = sin(pi/4) = sqrt(2)/2
    const float expectedWeight = std::sqrt(2.0f) / 2.0f;

    std::vector<float> bufA = {1.0f};
    std::vector<float> bufB = {0.0f};
    std::vector<float> dst;

    HybridBlend::blendEqualPower(bufA, bufB, dst, 0.5f);
    REQUIRE(dst[0] == Approx(expectedWeight).margin(1e-4f));

    std::vector<float> bufA2 = {0.0f};
    std::vector<float> bufB2 = {1.0f};
    HybridBlend::blendEqualPower(bufA2, bufB2, dst, 0.5f);
    REQUIRE(dst[0] == Approx(expectedWeight).margin(1e-4f));
}

TEST_CASE("HybridBlend: equal-power preserves total energy at crossfade midpoint", "[granular][blend]")
{
    // For equal-amplitude inputs, equal-power blend should preserve total power:
    // |wA|^2 + |wB|^2 = cos^2(pi/4) + sin^2(pi/4) = 1.0
    const float wA = std::cos(kPi / 4.0f);
    const float wB = std::sin(kPi / 4.0f);
    REQUIRE(wA * wA + wB * wB == Approx(1.0f).margin(1e-5f));

    // With unit inputs, output should have total power = 1.0
    std::vector<float> bufA = {1.0f};
    std::vector<float> bufB = {1.0f};
    std::vector<float> dst;
    HybridBlend::blendEqualPower(bufA, bufB, dst, 0.5f);

    // dst[0] = wA * 1 + wB * 1 = sqrt(2)
    REQUIRE(dst[0] == Approx(std::sqrt(2.0f)).margin(1e-4f));
}

TEST_CASE("HybridBlend: equal-power alpha=0 is all bufA, alpha=1 is all bufB", "[granular][blend]")
{
    std::vector<float> bufA = {0.7f, 0.3f};
    std::vector<float> bufB = {0.2f, 0.8f};
    std::vector<float> dst;

    HybridBlend::blendEqualPower(bufA, bufB, dst, 0.0f);
    REQUIRE(dst[0] == Approx(0.7f).margin(1e-4f));
    REQUIRE(dst[1] == Approx(0.3f).margin(1e-4f));

    HybridBlend::blendEqualPower(bufA, bufB, dst, 1.0f);
    REQUIRE(dst[0] == Approx(0.2f).margin(1e-4f));
    REQUIRE(dst[1] == Approx(0.8f).margin(1e-4f));
}

TEST_CASE("HybridBlend: blendTwo is monotonic between alpha=0 and alpha=1", "[granular][blend]")
{
    // For bufA=0, bufB=1, as alpha increases from 0 to 1,
    // the output should monotonically increase from 0 to 1.
    std::vector<float> bufA = {0.0f};
    std::vector<float> bufB = {1.0f};
    std::vector<float> dst;

    float prevValue = -1.0f;
    for (int step = 0; step <= 20; ++step)
    {
        float alpha = static_cast<float>(step) / 20.0f;
        HybridBlend::blendTwo(bufA, bufB, dst, alpha);
        REQUIRE(dst[0] >= prevValue - 1e-5f);
        prevValue = dst[0];
    }
}

TEST_CASE("GrainPool: isActive returns false for out-of-range indices", "[granular][pool]")
{
    GrainPool pool;
    REQUIRE_FALSE(pool.isActive(-1));
    REQUIRE_FALSE(pool.isActive(GrainPool::MAX_GRAINS));
    REQUIRE_FALSE(pool.isActive(9999));
}

// =============================================================================
//  Production GranularMorphEngine Tests (H14 fix)
// =============================================================================

TEST_CASE("GranularMorphEngine (production): prepare and processBlock with sine wave", "[granular][production]")
{
    more_phi::GranularMorphEngine engine;
    engine.prepare(44100.0, 512);
    engine.setActive(true);
    engine.setGrainSize(50.0f);
    engine.setGrainDensity(20.0f);

    juce::AudioBuffer<float> bufA(2, 512);
    juce::AudioBuffer<float> bufB(2, 512);
    bufA.clear();
    bufB.clear();

    for (int ch = 0; ch < 2; ++ch)
    {
        float* dataA = bufA.getWritePointer(ch);
        float* dataB = bufB.getWritePointer(ch);
        for (int i = 0; i < 512; ++i)
        {
            dataA[i] = std::sin(2.0f * 3.14159265358979f * 440.0f * static_cast<float>(i) / 44100.0f);
            dataB[i] = std::sin(2.0f * 3.14159265358979f * 880.0f * static_cast<float>(i) / 44100.0f);
        }
    }

    engine.processBlock(bufA, bufB, 0.5f);

    for (int ch = 0; ch < bufA.getNumChannels(); ++ch)
    {
        const float* data = bufA.getReadPointer(ch);
        bool hasNonZero = false;
        for (int i = 0; i < bufA.getNumSamples(); ++i)
        {
            REQUIRE(std::isfinite(data[i]));
            if (std::abs(data[i]) > 1e-6f) hasNonZero = true;
        }
        REQUIRE(hasNonZero);
    }
}

TEST_CASE("GranularMorphEngine (production): inactive engine leaves buffer unchanged", "[granular][production]")
{
    more_phi::GranularMorphEngine engine;
    engine.prepare(44100.0, 512);
    engine.setActive(false);

    juce::AudioBuffer<float> bufA(1, 256);
    juce::AudioBuffer<float> bufB(1, 256);
    bufA.clear();
    bufB.clear();

    float* dataA = bufA.getWritePointer(0);
    for (int i = 0; i < 256; ++i)
        dataA[i] = 0.5f;

    engine.processBlock(bufA, bufB, 0.3f);

    const float* out = bufA.getReadPointer(0);
    for (int i = 0; i < 256; ++i)
    {
        REQUIRE(out[i] == Catch::Approx(0.5f).margin(1e-6f));
    }
}

// =============================================================================
//  H5 normalization characterization — peak gain vs active grain count
//
//  The H5 fix normalizes the grain cloud by 1/sqrt(0.375 * N) (the Hann²-power
//  correct normalization). This test characterizes the actual peak-gain curve
//  so the normalization can be validated against its design intent:
//    - Output RMS must stay bounded (not grow with N) for uncorrelated grains.
//    - Output peak must never exceed a sane ceiling even at max density.
//
//  We deliberately avoid asserting an exact scaling law (peak gain is
//  path-dependent on grain scheduling, positions, and pitch), and instead
//  bound the behaviour the normalization is meant to guarantee.
// =============================================================================

TEST_CASE("GranularMorphEngine (production): output RMS stays bounded as density grows", "[granular][production][h5]")
{
    // Drive both sources with uncorrelated noise-like content (different sines)
    // so the cloud's grains are genuinely decorrelated — the regime the H5
    // RMS-normalization targets.
    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    auto runCloud = [&](float density, float positionRandom) -> float
    {
        more_phi::GranularMorphEngine engine;
        engine.prepare(sr, blockSize);
        engine.setActive(true);
        engine.setGrainSize(50.0f);
        engine.setGrainDensity(density);
        engine.setPositionRandomization(positionRandom);

        juce::AudioBuffer<float> bufA(1, blockSize);
        juce::AudioBuffer<float> bufB(1, blockSize);

        // Uncorrelated sources: A = 440 Hz, B = 660 Hz at unit amplitude.
        for (int ch = 0; ch < 1; ++ch)
        {
            float* a = bufA.getWritePointer(ch);
            float* b = bufB.getWritePointer(ch);
            for (int i = 0; i < blockSize; ++i)
            {
                a[i] = std::sin(2.0f * 3.14159265358979f * 440.0f * i / static_cast<float>(sr));
                b[i] = std::sin(2.0f * 3.14159265358979f * 660.0f * i / static_cast<float>(sr));
            }
        }

        // Warm up + measure over many blocks for a stable RMS.
        double sumSq = 0.0;
        int count = 0;
        int globalSample = 0;
        for (int blk = 0; blk < 100; ++blk)
        {
            // Re-fill sources each block so the cloud keeps reading new samples.
            for (int i = 0; i < blockSize; ++i)
            {
                bufA.getWritePointer(0)[i] = std::sin(2.0f * 3.14159265358979f * 440.0f * globalSample / static_cast<float>(sr));
                bufB.getWritePointer(0)[i] = std::sin(2.0f * 3.14159265358979f * 660.0f * globalSample / static_cast<float>(sr));
                ++globalSample;
            }
            engine.processBlock(bufA, bufB, 0.5f);
            if (blk < 10) continue;  // skip warmup
            const float* out = bufA.getReadPointer(0);
            for (int i = 0; i < blockSize; ++i)
            {
                sumSq += static_cast<double>(out[i]) * out[i];
                ++count;
            }
        }
        return static_cast<float>(std::sqrt(sumSq / static_cast<double>(count)));
    };

    const float rmsLow  = runCloud(10.0f, 1.0f);   // sparse, decorrelated
    const float rmsHigh = runCloud(100.0f, 1.0f);  // max density, decorrelated

    INFO("RMS low-density = " << rmsLow << "  RMS high-density = " << rmsHigh);
    // RMS must not explode with density — the whole point of H5.
    // Allow generous headroom; the requirement is "bounded", not "flat".
    REQUIRE(rmsHigh < 4.0f * (rmsLow + 1e-6f));
}

TEST_CASE("GranularMorphEngine (production): peak output stays within sane ceiling at max density", "[granular][production][h5]")
{
    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    more_phi::GranularMorphEngine engine;
    engine.prepare(sr, blockSize);
    engine.setActive(true);
    engine.setGrainSize(200.0f);   // max grain size -> longest overlap
    engine.setGrainDensity(100.0f); // max density
    engine.setPositionRandomization(0.0f); // worst case: coherent reads

    juce::AudioBuffer<float> bufA(1, blockSize);
    juce::AudioBuffer<float> bufB(1, blockSize);

    float globalPeak = 0.0f;
    for (int blk = 0; blk < 50; ++blk)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const int gi = blk * blockSize + i;
            bufA.getWritePointer(0)[i] = 1.0f;  // full-scale DC: maximizes coherent sum
            bufB.getWritePointer(0)[i] = 1.0f;
        }
        engine.processBlock(bufA, bufB, 0.5f);
        const float* out = bufA.getReadPointer(0);
        for (int i = 0; i < blockSize; ++i)
            globalPeak = std::max(globalPeak, std::abs(out[i]));
    }

    INFO("peak at max density / coherent = " << globalPeak);
    // Even in the worst coherent case, the cloud must not run away. The prior
    // test asserted < 20.0; the H5 normalization should keep this substantially
    // tighter. We assert a conservative ceiling that still catches a
    // normalization regression (e.g. normalization removed entirely).
    REQUIRE(globalPeak < 10.0f);
    REQUIRE(std::isfinite(globalPeak));
}
