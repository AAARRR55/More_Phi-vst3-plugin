/*
 * More-Phi — tests/Unit/TestNeuralMasteringFeatureExtractor.cpp
 *
 * Pins both the unchanged summary extractor and the new full-frame
 * extractFromAnalysis path. The summary extractor behaviour is asserted to be
 * byte-identical to v3.3.0 (regression guard); the analysis extractor fills
 * every field and is robust to null / non-finite inputs.
 */
#include <catch2/catch_test_macros.hpp>

#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"

#include <cmath>
#include <limits>

namespace {

more_phi::NeuralMasteringAnalysisSnapshot nominalSnapshot()
{
    more_phi::NeuralMasteringAnalysisSnapshot snap;
    snap.integratedLUFS  = -14.0f;
    snap.shortTermLUFS   = -12.0f;
    snap.momentaryLUFS   = -10.0f;
    snap.loudnessRange   = 7.0f;
    snap.truePeakDbTp    = -1.0f;
    snap.crestFactorDb   = 11.0f;
    snap.spectralTilt    = 1.5f;
    snap.monoFoldDownDeltaDb = 0.25f;
    snap.transientDensity = 0.4f;
    snap.harmonicRisk    = 0.1f;
    snap.sourceQualityScore = 0.9f;
    return snap;
}

} // namespace

// ── Regression guard: extractFromSummary is unchanged ────────────────────────

TEST_CASE("extractFromSummary populates only the summary fields (v3.3.0 behaviour)", "[NeuralMasteringFeatureExtractor][regression]")
{
    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto result = extractor.extractFromSummary(48000.0, 2, 512, 1000, -14.0f, -1.0f, 1.5f);

    REQUIRE(result.succeeded());
    CHECK(result.frame.sampleRate == 48000.0);
    CHECK(result.frame.channelCount == 2);
    CHECK(result.frame.blockSize == 512);
    CHECK(result.frame.frameIndex == 1000);
    CHECK(result.frame.integratedLUFS == -14.0f);
    CHECK(result.frame.shortTermLUFS == -14.0f);   // mirrors integrated (unchanged)
    CHECK(result.frame.momentaryLUFS == -14.0f);   // mirrors integrated (unchanged)
    CHECK(result.frame.truePeakDbTp == -1.0f);
    CHECK(result.frame.spectralTilt == 1.5f);
    // Unchanged: fields outside the summary remain zeroed by the summary path.
    CHECK(result.frame.crestFactorDb == 0.0f);
    CHECK(result.frame.loudnessRange == 0.0f);
    CHECK(result.frame.spectralBands[0] == 0.0f);
}

// ── extractFromAnalysis: happy path fills everything ─────────────────────────

TEST_CASE("extractFromAnalysis fills every scalar and band field", "[NeuralMasteringFeatureExtractor][analysis]")
{
    float spec[more_phi::kNeuralMasteringSpectralBandCount];
    float stereo[more_phi::kNeuralMasteringStereoBandCount];
    float ms[more_phi::kNeuralMasteringStereoBandCount];
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringSpectralBandCount; ++i) spec[i] = 0.1f * static_cast<float>(i);
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringStereoBandCount; ++i) { stereo[i] = 0.7f; ms[i] = 0.3f; }

    auto snap = nominalSnapshot();
    snap.spectralBands = spec;
    snap.stereoCorrelation = stereo;
    snap.midSideRatio = ms;

    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto result = extractor.extractFromAnalysis(44100.0, 2, 256, 42, snap);

    REQUIRE(result.succeeded());
    const auto& f = result.frame;
    CHECK(f.sampleRate == 44100.0);
    CHECK(f.channelCount == 2);
    CHECK(f.blockSize == 256);
    CHECK(f.frameIndex == 42);
    CHECK(f.integratedLUFS == -14.0f);
    CHECK(f.shortTermLUFS == -12.0f);
    CHECK(f.momentaryLUFS == -10.0f);
    CHECK(f.loudnessRange == 7.0f);
    CHECK(f.truePeakDbTp == -1.0f);
    CHECK(f.crestFactorDb == 11.0f);
    CHECK(f.spectralTilt == 1.5f);
    CHECK(f.monoFoldDownDeltaDb == 0.25f);
    CHECK(f.transientDensity == 0.4f);
    CHECK(f.harmonicRisk == 0.1f);
    CHECK(f.sourceQualityScore == 0.9f);
    CHECK(f.spectralBands[5] == 0.5f);
    // Last spectral band = 0.1f * (count-1). Compare with a small tolerance
    // rather than exact equality (0.1f * 31 isn't bit-exact in float).
    {
        const float expected = 0.1f * static_cast<float>(more_phi::kNeuralMasteringSpectralBandCount - 1);
        CHECK(std::fabs(f.spectralBands[more_phi::kNeuralMasteringSpectralBandCount - 1] - expected) < 1e-5f);
    }
    CHECK(f.stereoCorrelation[0] == 0.7f);
    CHECK(f.midSideRatio[0] == 0.3f);

    // The resulting frame must always be finite — that is the contract the
    // ONNX input head relies on.
    CHECK(more_phi::NeuralMasteringFeatureExtractor::isFrameFinite(f));
}

// ── extractFromAnalysis: null band pointers are zero-filled, frame stays finite ─

TEST_CASE("extractFromAnalysis zero-fills null band arrays and keeps the frame finite", "[NeuralMasteringFeatureExtractor][analysis]")
{
    auto snap = nominalSnapshot(); // spectralBands/stereoCorrelation/midSideRatio all null

    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto result = extractor.extractFromAnalysis(48000.0, 2, 512, 0, snap);

    REQUIRE(result.succeeded());
    const auto& f = result.frame;
    CHECK(f.spectralBands[0] == 0.0f);
    CHECK(f.spectralBands[more_phi::kNeuralMasteringSpectralBandCount - 1] == 0.0f);
    CHECK(f.stereoCorrelation[0] == 0.0f);
    CHECK(f.midSideRatio[0] == 0.0f);
    CHECK(more_phi::NeuralMasteringFeatureExtractor::isFrameFinite(f));
}

// ── extractFromAnalysis: non-finite scalars are coerced ──────────────────────

TEST_CASE("extractFromAnalysis coerces non-finite scalars to zero", "[NeuralMasteringFeatureExtractor][analysis]")
{
    auto snap = nominalSnapshot();
    snap.integratedLUFS = std::numeric_limits<float>::quiet_NaN();
    snap.truePeakDbTp   = std::numeric_limits<float>::infinity();

    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto result = extractor.extractFromAnalysis(48000.0, 2, 512, 0, snap);

    REQUIRE(result.succeeded());
    CHECK(result.frame.integratedLUFS == 0.0f);
    CHECK(result.frame.truePeakDbTp == 0.0f);
    CHECK(more_phi::NeuralMasteringFeatureExtractor::isFrameFinite(result.frame));
}

// ── extractFromAnalysis: error paths ─────────────────────────────────────────

TEST_CASE("extractFromAnalysis rejects unsupported layouts and invalid params", "[NeuralMasteringFeatureExtractor][analysis]")
{
    more_phi::NeuralMasteringFeatureExtractor extractor;
    auto snap = nominalSnapshot();

    CHECK(extractor.extractFromAnalysis(48000.0, 6, 512, 0, snap).status
          == more_phi::NeuralMasteringFeatureExtractionStatus::UnsupportedLayout);
    CHECK(extractor.extractFromAnalysis(0.0, 2, 512, 0, snap).status
          == more_phi::NeuralMasteringFeatureExtractionStatus::InvalidInput);
    CHECK(extractor.extractFromAnalysis(48000.0, 2, 0, 0, snap).status
          == more_phi::NeuralMasteringFeatureExtractionStatus::InvalidInput);

    // Mono is supported.
    CHECK(extractor.extractFromAnalysis(48000.0, 1, 512, 0, snap).succeeded());
}
