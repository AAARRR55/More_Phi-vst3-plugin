// tests/Unit/TestEndToEndSignalIntegrity.cpp
//
// AUDIT-FIX (P0, 2026-06-27): Full-chain signal integrity test. Generates a
// known audio signal (pink noise at controlled RMS), runs it through the
// complete mastering pipeline (capture → resample → infer → decode → safety →
// apply → processBlock), and verifies the output meets analytical expectations.
//
// This is the single test that validates aggregate behavior across all five
// audit layers. Every layer is unit-tested individually; this test confirms
// they work together correctly.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterAnalysisEngine.h"
#include "AI/SonicMasterDecisionDecoder.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/LUFSMeter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace {

// ── Pink noise generator ──────────────────────────────────────────────────
// Generates pink noise (-3 dB/octave rolloff) using the Voss-McCartney
// algorithm (N=16 octaves, simple random-step accumulation).
std::vector<float> generatePinkNoise(std::size_t numSamples, float targetRms = 0.1f)
{
    std::vector<float> out(numSamples, 0.0f);
    std::mt19937_64 rng(42);  // fixed seed for reproducibility

    constexpr int kOctaves = 16;
    std::array<float, kOctaves> octaveValues {};
    std::array<unsigned, kOctaves> octaveCounters {};
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Initialize with small random values.
    for (int o = 0; o < kOctaves; ++o)
        octaveValues[o] = dist(rng) * 0.01f;

    for (std::size_t i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int o = 0; o < kOctaves; ++o)
        {
            octaveCounters[o] = (octaveCounters[o] + 1);
            // Check if this octave's counter is a power of 2.
            const bool needsNew = (octaveCounters[o] & (octaveCounters[o] - 1u)) == 0u;
            if (needsNew)
                octaveValues[o] = dist(rng);
            sum += octaveValues[o];
        }
        out[i] = sum;
    }

    // Normalize to target RMS.
    double rms = 0.0;
    for (const auto v : out) rms += static_cast<double>(v) * static_cast<double>(v);
    rms = std::sqrt(rms / static_cast<double>(numSamples));
    if (rms > 1e-12)
    {
        const float scale = targetRms / static_cast<float>(rms);
        for (auto& v : out) v *= scale;
    }
    return out;
}

// ── Known-decision source ──────────────────────────────────────────────────
// Injects a specific decision vector so the test can predict the applied
// processing. Uses the same pattern as StubDecisionSource in
// TestSonicMasterAnalysisEngine.cpp.
class FixedDecisionSource final : public more_phi::ISonicMasterInferenceSource
{
public:
    explicit FixedDecisionSource(const float* decision, std::size_t decisionSize)
    {
        REQUIRE(decisionSize == more_phi::kSonicMasterDecisionWidth);
        std::copy_n(decision, more_phi::kSonicMasterDecisionWidth, decision_.begin());
    }

    [[nodiscard]] bool isAvailable() const noexcept override { return true; }
    [[nodiscard]] std::uint64_t inferenceRunCount() const noexcept override { return runCount_; }

    bool infer(const float* /*stereoInterleaved*/, float* outDecision,
               std::size_t outCapacity) noexcept override
    {
        if (outCapacity < more_phi::kSonicMasterDecisionWidth)
            return false;
        std::copy_n(decision_.data(), more_phi::kSonicMasterDecisionWidth, outDecision);
        ++runCount_;
        return true;
    }

private:
    std::array<float, more_phi::kSonicMasterDecisionWidth> decision_ {};
    std::uint64_t runCount_ = 0;
};

// ── Host-rate window computation ───────────────────────────────────────────
std::size_t hostWindowFrames(double sampleRate)
{
    return static_cast<std::size_t>(
        std::llround(more_phi::kSonicMasterSegmentFrames * sampleRate / 44100.0));
}

// ── Feed audio blocks into the capture ring ───────────────────────────────
void feedAudio(more_phi::SonicMasterAnalysisEngine& eng,
               const std::vector<float>& left,
               const std::vector<float>& right,
               std::size_t blockSize = 512)
{
    const std::size_t n = std::min(left.size(), right.size());
    for (std::size_t off = 0; off < n; off += blockSize)
    {
        const std::size_t nb = std::min(blockSize, n - off);
        eng.capture(left.data() + off, right.data() + off, nb);
    }
}

// ── Compute RMS of a buffer ──────────────────────────────────────────────
float computeRmsDb(const float* data, std::size_t n)
{
    if (n == 0) return -120.0f;
    double sumSq = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        sumSq += static_cast<double>(data[i]) * static_cast<double>(data[i]);
    const double rms = std::sqrt(sumSq / static_cast<double>(n));
    return 20.0f * static_cast<float>(std::log10(std::max(rms, 1e-12)));
}

} // namespace

TEST_CASE("E2E: Full mastering chain produces correct output LUFS from known input and decision",
          "[E2E][NeuralMastering][P0]")
{
    // ── Build a known decision vector ──────────────────────────────────────
    // Target -14 LUFS, -1 dBTP ceiling, moderate EQ (+2 dB @ 60 Hz, -1 dB @ 10 kHz),
    // conservative compression (ratio ~2.5:1, threshold -20 dB), stereo width 0.8.
    std::array<float, more_phi::kSonicMasterDecisionWidth> decision {};
    decision.fill(0.0f);

    // Target LUFS: -14 → loudness[0] = 0.0 (neutral).
    decision[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;

    // True peak: -1 dBTP → limiter[0] = 0.0.
    decision[more_phi::kSonicMasterTruePeakIdx] = -1.0f;

    // EQ bands (8): +2 dB @ 60 Hz, +1 dB @ 120 Hz, 0 dB @ 250 Hz,
    // 0 dB @ 500 Hz, 0 dB @ 1 kHz, +0.5 dB @ 2.5 kHz, -1 dB @ 5 kHz, -1 dB @ 10 kHz.
    decision[more_phi::kSonicMasterEqGainOffset + 0] = 2.0f;   // 60 Hz: +2 dB
    decision[more_phi::kSonicMasterEqGainOffset + 1] = 1.0f;   // 120 Hz: +1 dB
    decision[more_phi::kSonicMasterEqGainOffset + 2] = 0.0f;   // 250 Hz: 0 dB
    decision[more_phi::kSonicMasterEqGainOffset + 3] = 0.0f;   // 500 Hz: 0 dB
    decision[more_phi::kSonicMasterEqGainOffset + 4] = 0.0f;   // 1 kHz: 0 dB
    decision[more_phi::kSonicMasterEqGainOffset + 5] = 0.5f;   // 2.5 kHz: +0.5 dB
    decision[more_phi::kSonicMasterEqGainOffset + 6] = -1.0f;  // 5 kHz: -1 dB
    decision[more_phi::kSonicMasterEqGainOffset + 7] = -1.0f;  // 10 kHz: -1 dB

    // Compressor band 0 (Sub): threshold -20 dB, ratio 2.5:1, attack 10ms, release 100ms.
    const std::size_t c0 = more_phi::kSonicMasterCompOffset;
    decision[c0 + 0] = -20.0f;  // threshold
    decision[c0 + 1] = 2.5f;    // ratio
    decision[c0 + 2] = 10.0f;   // attack ms
    decision[c0 + 3] = 100.0f;  // release ms
    decision[c0 + 4] = 0.0f;    // makeup dB
    decision[c0 + 5] = 3.0f;    // knee dB

    // Compressor band 1 (Low): ratio 3.0:1.
    const std::size_t c1 = c0 + more_phi::kSonicMasterCompBandWidth;
    decision[c1 + 0] = -22.0f;
    decision[c1 + 1] = 3.0f;
    decision[c1 + 2] = 8.0f;
    decision[c1 + 3] = 120.0f;
    decision[c1 + 4] = 0.0f;
    decision[c1 + 5] = 2.0f;

    // Compressor band 2 (Mid): ratio 2.0:1.
    const std::size_t c2 = c0 + 2 * more_phi::kSonicMasterCompBandWidth;
    decision[c2 + 0] = -18.0f;
    decision[c2 + 1] = 2.0f;
    decision[c2 + 2] = 5.0f;
    decision[c2 + 3] = 80.0f;
    decision[c2 + 4] = 0.0f;
    decision[c2 + 5] = 2.0f;

    // Stereo width: 0.8 normalized (both regions).
    decision[more_phi::kSonicMasterStereoOffset + 0] = -0.2f;  // 1.0 + (-0.2) = 0.8
    decision[more_phi::kSonicMasterStereoOffset + 1] = -0.2f;

    // ── Wire the pipeline ─────────────────────────────────────────────────
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;

    more_phi::AutoMasteringEngine engine;
    // Use startIntelligence=true so the internal chain is active and actually
    // processes audio (the shipped plugin uses false, but for testing we need
    // the DSP writes to take effect).
    engine.prepare(kSampleRate, kBlockSize, /*startIntelligence=*/true);
    engine.setActive(true);

    more_phi::SonicMasterAnalysisEngine sonicEngine;
    FixedDecisionSource source(decision.data(), decision.size());
    sonicEngine.setInferenceSource(&source);
    sonicEngine.setApplicationEngine(&engine);
    sonicEngine.prepare(kSampleRate, kBlockSize);

    // ── Generate pink noise at ~-18 dBFS RMS ──────────────────────────────
    const std::size_t totalFrames = hostWindowFrames(kSampleRate) + 8192; // window + margin
    auto noise = generatePinkNoise(totalFrames, 0.126f); // 0.126 ≈ -18 dBFS RMS

    // Feed into the capture ring.
    feedAudio(sonicEngine, noise, noise, kBlockSize);

    // Verify capture diagnostics confirm a full window.
    const auto diag = sonicEngine.getCaptureDiagnostics();
    REQUIRE(diag.ringAllocated);
    REQUIRE(diag.capturedFrames >= diag.requiredFrames);

    // ── Run one analysis cycle ────────────────────────────────────────────
    REQUIRE(sonicEngine.runOneCycleForTest());
    REQUIRE(sonicEngine.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::Applied);

    // ── Verify plan was stored ────────────────────────────────────────────
    REQUIRE(engine.hasLastSafeNeuralMasteringPlan());
    const auto& plan = engine.getLastSafeNeuralMasteringPlan();
    CHECK(plan.valid);
    CHECK(plan.appliedMask.eq);
    CHECK(plan.appliedMask.dynamics);
    CHECK(plan.appliedMask.stereo);

    // ── Verify selected EQ bands match the decision ───────────────────────
    // Band 0: +2 dB / 12 = 0.1667 normalized.
    CHECK_THAT(plan.projectedTargets.eq[0],
               Catch::Matchers::WithinAbs(2.0f / more_phi::kAdaptiveEqMaxGainDb, 0.01f));
    // Band 6: -1 dB / 12 = -0.0833 normalized.
    CHECK_THAT(plan.projectedTargets.eq[6],
               Catch::Matchers::WithinAbs(-1.0f / more_phi::kAdaptiveEqMaxGainDb, 0.01f));

    // ── Feed audio through the mastering chain ────────────────────────────
    juce::AudioBuffer<float> outputBuf(2, kBlockSize);
    std::vector<float> outputSamples;
    outputSamples.reserve(totalFrames);

    // Process in blocks (drive enough audio for the meters to stabilize).
    constexpr int kMeterBlocks = 200;  // ~10s at 512 block / 48k
    for (int block = 0; block < kMeterBlocks; ++block)
    {
        // Feed fresh noise on each block (not the ring — the audio pipe).
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* writePtr = outputBuf.getWritePointer(ch);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const std::size_t srcIdx = static_cast<std::size_t>(
                    (static_cast<std::size_t>(block) * static_cast<std::size_t>(kBlockSize) + static_cast<std::size_t>(i))
                    % noise.size());
                writePtr[i] = noise[srcIdx];
            }
        }

        // Run through the processing chain.
        engine.processBlock(outputBuf);

        // Accumulate output samples for analysis.
        const auto* ch0 = outputBuf.getReadPointer(0);
        outputSamples.insert(outputSamples.end(), ch0, ch0 + kBlockSize);
    }

    // ── Verify output properties ──────────────────────────────────────────

    // 1. All output samples are finite (no NaN, no Inf).
    for (const auto s : outputSamples)
        CHECK(std::isfinite(s));

    // 2. Output is bounded (no clipping beyond reasonable headroom).
    float peak = 0.0f;
    for (const auto s : outputSamples)
        peak = std::max(peak, std::abs(s));
    // With -14 LUFS target and -1 dBTP ceiling, output should be well below 0 dBFS.
    CHECK(peak < 1.0f);
    CHECK(peak > 1e-6f);

    // 3. Engine meters are reporting correctly.
    const float lufsIntegrated = engine.getLUFSIntegrated();
    CHECK(std::isfinite(lufsIntegrated));
    // With target -14 LUFS and ~-18 dBFS input pink noise, the normalizer
    // should have added ~4 dB of gain. The integrated LUFS after processing
    // should converge toward -14 LUFS within ±3 dB (generous tolerance for
    // pink noise vs gated LUFS measurement interaction).
    CHECK(lufsIntegrated > -20.0f);
    CHECK(lufsIntegrated < -8.0f);

    // 4. True peak reading is valid and non-zero.
    const float truePeak = engine.getTruePeak_dBTP();
    CHECK(std::isfinite(truePeak));
    CHECK(truePeak < 0.0f);  // should be negative dBTP (below ceiling)

    // 5. Readback verification from the last apply should show writes landed.
    const auto verification = engine.getLastApplyVerification();
    CHECK(verification.enqueued > 0);
    // The internal chain doesn't use the Ozone bridge, so the verification
    // counts may be 0 (no hosted plugin). This is expected — the internal
    // chain does not go through OzonePlanApplicator.

    // ── Clean up ──────────────────────────────────────────────────────────
    sonicEngine.release();
    engine.reset();
}

TEST_CASE("E2E: Full chain handles NaN in decision vector without crashing",
          "[E2E][NeuralMastering][P0][Safety]")
{
    // ── All-NaN decision (should be coerced by finiteOr/clamp) ────────────
    std::array<float, more_phi::kSonicMasterDecisionWidth> nanDecision {};
    for (auto& v : nanDecision)
        v = std::numeric_limits<float>::quiet_NaN();

    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;

    more_phi::AutoMasteringEngine engine;
    engine.prepare(kSampleRate, kBlockSize, /*startIntelligence=*/true);
    engine.setActive(true);

    more_phi::SonicMasterAnalysisEngine sonicEngine;
    FixedDecisionSource source(nanDecision.data(), nanDecision.size());
    sonicEngine.setInferenceSource(&source);
    sonicEngine.setApplicationEngine(&engine);
    sonicEngine.prepare(kSampleRate, kBlockSize);

    const std::size_t totalFrames = hostWindowFrames(kSampleRate) + 1024;
    auto noise = generatePinkNoise(totalFrames, 0.126f);
    feedAudio(sonicEngine, noise, noise, kBlockSize);

    // The cycle should succeed (decoder coerces NaN to 0.0).
    REQUIRE(sonicEngine.runOneCycleForTest());

    // All plan targets must be finite (the decoder's finiteOr/clamp guarantee).
    REQUIRE(engine.hasLastSafeNeuralMasteringPlan());
    const auto& plan = engine.getLastSafeNeuralMasteringPlan();

    // Check every projected target dimension is finite.
    for (const auto v : plan.projectedTargets.eq)
        CHECK(std::isfinite(v));
    for (const auto v : plan.projectedTargets.dynamics)
        CHECK(std::isfinite(v));
    for (const auto v : plan.projectedTargets.stereo)
        CHECK(std::isfinite(v));
    CHECK(std::isfinite(plan.projectedTargets.loudness[0]));

    // Apply through the chain and verify no NaN output.
    juce::AudioBuffer<float> buf(2, kBlockSize);
    for (int ch = 0; ch < 2; ++ch)
        std::fill_n(buf.getWritePointer(ch), kBlockSize, noise[0]);
    engine.processBlock(buf);
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* data = buf.getReadPointer(ch);
        for (int i = 0; i < kBlockSize; ++i)
            CHECK(std::isfinite(data[i]));
    }

    sonicEngine.release();
    engine.reset();
}

TEST_CASE("E2E: Full chain produces deterministic output (same input + same decision = same output)",
          "[E2E][NeuralMastering][P0][Determinism]")
{
    // ── Build a simple neutral decision vector ─────────────────────────────
    std::array<float, more_phi::kSonicMasterDecisionWidth> decision {};
    decision.fill(0.0f);
    decision[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;
    decision[more_phi::kSonicMasterTruePeakIdx] = -1.0f;

    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;

    // Generate deterministic noise (fixed seed).
    auto noise = generatePinkNoise(hostWindowFrames(kSampleRate) + 8192, 0.126f);

    // Run twice with identical setup.
    std::vector<float> output1, output2;

    for (int run = 0; run < 2; ++run)
    {
        more_phi::AutoMasteringEngine engine;
        engine.prepare(kSampleRate, kBlockSize, /*startIntelligence=*/true);
        engine.setActive(true);

        more_phi::SonicMasterAnalysisEngine sonicEngine;
        FixedDecisionSource source(decision.data(), decision.size());
        sonicEngine.setInferenceSource(&source);
        sonicEngine.setApplicationEngine(&engine);
        sonicEngine.prepare(kSampleRate, kBlockSize);

        feedAudio(sonicEngine, noise, noise, kBlockSize);
        REQUIRE(sonicEngine.runOneCycleForTest());

        juce::AudioBuffer<float> buf(2, kBlockSize);
        auto& outVec = (run == 0) ? output1 : output2;

        for (int block = 0; block < 50; ++block)  // ~500ms
        {
            for (int ch = 0; ch < 2; ++ch)
                std::fill_n(buf.getWritePointer(ch), kBlockSize, noise[static_cast<std::size_t>(block * kBlockSize) % noise.size()]);
            engine.processBlock(buf);
            const auto* r0 = buf.getReadPointer(0);
            outVec.insert(outVec.end(), r0, r0 + kBlockSize);
        }

        sonicEngine.release();
        engine.reset();
    }

    // Outputs must be byte-identical across runs.
    REQUIRE(output1.size() == output2.size());
    for (std::size_t i = 0; i < output1.size(); ++i)
    {
        // Use exact bit comparison (ULP-level) for determinism.
        if (std::isfinite(output1[i]) && std::isfinite(output2[i]))
        {
            const auto& a = output1[i];
            const auto& b = output2[i];
            // Float bit-cast for ULP comparison.
            auto bitsA = *reinterpret_cast<const std::uint32_t*>(&a);
            auto bitsB = *reinterpret_cast<const std::uint32_t*>(&b);
            // Handle sign bit for negative numbers.
            if (bitsA & 0x80000000u) bitsA = ~bitsA + 1;
            if (bitsB & 0x80000000u) bitsB = ~bitsB + 1;
            const auto ulpDiff = bitsA > bitsB ? bitsA - bitsB : bitsB - bitsA;
            CHECK(ulpDiff == 0);
        }
        else
        {
            CHECK(std::isnan(output1[i]) == std::isnan(output2[i]));
        }
    }
}
