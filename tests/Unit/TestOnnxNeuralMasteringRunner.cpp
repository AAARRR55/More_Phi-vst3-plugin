/*
 * More-Phi — tests/Unit/TestOnnxNeuralMasteringRunner.cpp
 *
 * Exercises the ONNX mastering runner SEAM (no ONNX Runtime linked) and the
 * pure I/O transforms it exposes. These tests pin the feature/plan I/O
 * contract so that when real ONNX inference is wired in, only the
 * session/run path changes — the transforms stay byte-stable.
 */
#include <catch2/catch_test_macros.hpp>

#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringController.h"
#include "AI/OnnxNeuralMasteringRunner.h"
#include "Core/AutoMasteringEngine.h"

#include <cmath>
#include <limits>
#include <vector>

namespace {

// Build a feature frame with every field populated (incl. band arrays) so the
// round-trip through serializeFeatureFrame is fully exercised.
more_phi::NeuralMasteringFeatureFrame fullFeatureFrame(std::uint64_t frameIndex)
{
    more_phi::NeuralMasteringAnalysisSnapshot snap;
    snap.integratedLUFS  = -14.0f;
    snap.shortTermLUFS   = -12.5f;
    snap.momentaryLUFS   = -10.0f;
    snap.loudnessRange   = 7.0f;
    snap.truePeakDbTp    = -1.0f;
    snap.crestFactorDb   = 12.0f;
    snap.spectralTilt    = 1.5f;
    snap.monoFoldDownDeltaDb = 0.3f;
    snap.transientDensity = 0.45f;
    snap.harmonicRisk    = 0.1f;
    snap.sourceQualityScore = 0.92f;

    static float spec[more_phi::kNeuralMasteringSpectralBandCount];
    static float stereo[more_phi::kNeuralMasteringStereoBandCount];
    static float ms[more_phi::kNeuralMasteringStereoBandCount];
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringSpectralBandCount; ++i) spec[i] = 0.01f * static_cast<float>(i);
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringStereoBandCount; ++i)  { stereo[i] = 0.8f - 0.05f * static_cast<float>(i); ms[i] = 0.5f + 0.02f * static_cast<float>(i); }
    snap.spectralBands = spec;
    snap.stereoCorrelation = stereo;
    snap.midSideRatio = ms;

    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto extracted = extractor.extractFromAnalysis(48000.0, 2, 512, frameIndex, snap);
    REQUIRE(extracted.succeeded());
    return extracted.frame;
}

} // namespace

// ── Pure transform: serializeFeatureFrame ────────────────────────────────────

TEST_CASE("serializeFeatureFrame writes exactly kOnnxInputFeatureCount values", "[OnnxNeuralMasteringRunner][io]")
{
    const auto frame = fullFeatureFrame(12345);

    std::vector<float> tensor(more_phi::kOnnxInputFeatureCount + 8, -99.0f);
    more_phi::serializeFeatureFrame(frame, tensor.data(), tensor.size());

    // The 11 scalar block must match the frame's scalar fields exactly.
    CHECK(tensor[0]  == frame.integratedLUFS);
    CHECK(tensor[1]  == frame.shortTermLUFS);
    CHECK(tensor[2]  == frame.momentaryLUFS);
    CHECK(tensor[3]  == frame.loudnessRange);
    CHECK(tensor[4]  == frame.truePeakDbTp);
    CHECK(tensor[5]  == frame.crestFactorDb);
    CHECK(tensor[6]  == frame.spectralTilt);
    CHECK(tensor[7]  == frame.monoFoldDownDeltaDb);
    CHECK(tensor[8]  == frame.transientDensity);
    CHECK(tensor[9]  == frame.harmonicRisk);
    CHECK(tensor[10] == frame.sourceQualityScore);

    // Band arrays land contiguously right after the scalar block.
    CHECK(tensor[11] == frame.spectralBands[0]);
    CHECK(tensor[11 + more_phi::kNeuralMasteringSpectralBandCount - 1] == frame.spectralBands[more_phi::kNeuralMasteringSpectralBandCount - 1]);

    // Guard slots beyond the written region must be untouched.
    CHECK(tensor[more_phi::kOnnxInputFeatureCount]     == -99.0f);
    CHECK(tensor[more_phi::kOnnxInputFeatureCount + 7] == -99.0f);
}

TEST_CASE("serializeFeatureFrame coerces non-finite scalars to zero", "[OnnxNeuralMasteringRunner][io]")
{
    auto frame = fullFeatureFrame(0);
    frame.integratedLUFS = std::numeric_limits<float>::quiet_NaN();
    frame.spectralTilt   = std::numeric_limits<float>::infinity();
    frame.spectralBands[0] = std::numeric_limits<float>::quiet_NaN();

    float tensor[more_phi::kOnnxInputFeatureCount] {};
    more_phi::serializeFeatureFrame(frame, tensor, more_phi::kOnnxInputFeatureCount);

    CHECK(tensor[0] == 0.0f); // integratedLUFS NaN -> 0
    CHECK(tensor[6] == 0.0f); // spectralTilt inf -> 0
    CHECK(tensor[11] == 0.0f); // spectralBands[0] NaN -> 0
}

TEST_CASE("serializeFeatureFrame is a no-op when the buffer is too small", "[OnnxNeuralMasteringRunner][io]")
{
    const auto frame = fullFeatureFrame(0);
    float tensor[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    more_phi::serializeFeatureFrame(frame, tensor, 4); // capacity < required
    CHECK(tensor[0] == 1.0f); // untouched
}

// ── Pure transform: buildPlanCandidate ───────────────────────────────────────

TEST_CASE("buildPlanCandidate maps the 72-float output into deltas in order", "[OnnxNeuralMasteringRunner][io]")
{
    const auto frame = fullFeatureFrame(5000);
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.0f);
    // Stamp each control group with a distinct sentinel so we can verify ordering.
    for (auto& v : deltas) v = 0.0f;
    std::size_t o = 0;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringEqTargetCount; ++i)        deltas[o++] = 0.10f;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringDynamicsTargetCount; ++i)  deltas[o++] = 0.05f;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringStereoTargetCount; ++i)    deltas[o++] = -0.03f;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringHarmonicTargetCount; ++i)  deltas[o++] = 0.20f;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringLimiterTargetCount; ++i)   deltas[o++] = -0.25f;
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringLoudnessTargetCount; ++i)  deltas[o++] = 0.07f;

    more_phi::MasteringControlMask all;
    all.eq = all.dynamics = all.stereo = all.harmonic = all.limiter = all.loudness = true;

    const auto candidate = more_phi::buildPlanCandidate(deltas.data(), deltas.size(), frame,
                                                        0.9f,
                                                        more_phi::NeuralMasteringEvidenceLevel::PrototypeMeasured,
                                                        all);

    CHECK(candidate.schemaVersion == more_phi::kNeuralMasteringPlanSchemaVersion);
    CHECK(candidate.producedAtFrame == frame.frameIndex);
    CHECK(candidate.expiresAfterFrame == frame.frameIndex + 96000);
    CHECK(candidate.confidence == 0.9f);
    CHECK(candidate.evidenceLevel == more_phi::NeuralMasteringEvidenceLevel::PrototypeMeasured);
    CHECK(candidate.deltas.eq[0] == 0.10f);
    CHECK(candidate.deltas.dynamics[0] == 0.05f);
    CHECK(candidate.deltas.stereo[0] == -0.03f);
    CHECK(candidate.deltas.harmonic[0] == 0.20f);
    CHECK(candidate.deltas.limiter[0] == -0.25f);
    CHECK(candidate.deltas.loudness[0] == 0.07f);
    // targets mirror deltas by convention.
    CHECK(candidate.targets.eq[0] == candidate.deltas.eq[0]);
}

TEST_CASE("buildPlanCandidate zeroes deltas for controls outside the editable mask", "[OnnxNeuralMasteringRunner][io]")
{
    const auto frame = fullFeatureFrame(0);
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.5f);

    more_phi::MasteringControlMask safe; // default mask: eq/dynamics/stereo/loudness only
    safe.eq = safe.dynamics = safe.stereo = safe.loudness = true;

    const auto candidate = more_phi::buildPlanCandidate(deltas.data(), deltas.size(), frame,
                                                        0.8f,
                                                        more_phi::NeuralMasteringEvidenceLevel::PrototypeMeasured,
                                                        safe);

    CHECK(candidate.deltas.eq[0]       == 0.5f);
    CHECK(candidate.deltas.dynamics[0] == 0.5f);
    CHECK(candidate.deltas.stereo[0]   == 0.5f);
    CHECK(candidate.deltas.loudness[0] == 0.5f);
    // High-risk controls are masked off → must be zeroed even though the model emitted 0.5.
    CHECK(candidate.deltas.harmonic[0] == 0.0f);
    CHECK(candidate.deltas.limiter[0]  == 0.0f);
}

// ── Pure transform: sanitizePlanCandidate ────────────────────────────────────

TEST_CASE("sanitizePlanCandidate clamps deltas to [-1,1] and zeroes non-finite values", "[OnnxNeuralMasteringRunner][io]")
{
    more_phi::NeuralMasteringPlanCandidate candidate;
    candidate.confidence = 1.5f; // out of [0,1]
    candidate.deltas.eq[0] = 1.7f;        // > 1
    candidate.deltas.dynamics[0] = -2.3f; // < -1
    candidate.deltas.stereo[0] = std::numeric_limits<float>::quiet_NaN();
    candidate.targets.eq[0] = std::numeric_limits<float>::infinity();

    const bool changed = more_phi::sanitizePlanCandidate(candidate);

    CHECK(changed);
    CHECK(candidate.confidence == 1.0f);
    CHECK(candidate.deltas.eq[0] == 1.0f);
    CHECK(candidate.deltas.dynamics[0] == -1.0f);
    CHECK(candidate.deltas.stereo[0] == 0.0f);
    CHECK(candidate.targets.eq[0] == 0.0f);
}

// ── Proposal disposition: confidence / abstention brain layer ───────────────

TEST_CASE("evaluateNeuralMasteringProposal treats near-zero output as an intentional no-op", "[OnnxNeuralMasteringRunner][brain]")
{
    const auto frame = fullFeatureFrame(1000);
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.0f);

    const auto disposition = more_phi::evaluateNeuralMasteringProposal(deltas.data(), deltas.size(), frame);

    CHECK(disposition.confidence > 0.95f);
    CHECK_FALSE(disposition.abstain);
    CHECK_FALSE(disposition.reviewOnly);
    CHECK(disposition.requestedFallbackMode == more_phi::NeuralMasteringFallbackMode::None);
}

TEST_CASE("evaluateNeuralMasteringProposal abstains to transparent bypass on impossible feature frames", "[OnnxNeuralMasteringRunner][brain]")
{
    more_phi::NeuralMasteringFeatureFrame frame; // invalid: no sample rate/channel/block analysis context
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.25f);

    const auto disposition = more_phi::evaluateNeuralMasteringProposal(deltas.data(), deltas.size(), frame);

    CHECK(disposition.confidence == 0.0f);
    CHECK(disposition.abstain);
    CHECK_FALSE(disposition.reviewOnly);
    CHECK(disposition.requestedFallbackMode == more_phi::NeuralMasteringFallbackMode::TransparentBypass);
}

TEST_CASE("evaluateNeuralMasteringProposal accepts moderate finite moves", "[OnnxNeuralMasteringRunner][brain]")
{
    const auto frame = fullFeatureFrame(2000);
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.20f);

    const auto disposition = more_phi::evaluateNeuralMasteringProposal(deltas.data(), deltas.size(), frame);

    CHECK(disposition.confidence >= 0.75f);
    CHECK_FALSE(disposition.abstain);
    CHECK_FALSE(disposition.reviewOnly);
    CHECK(disposition.requestedFallbackMode == more_phi::NeuralMasteringFallbackMode::None);
}

TEST_CASE("evaluateNeuralMasteringProposal marks saturated neural output review-only", "[OnnxNeuralMasteringRunner][brain]")
{
    const auto frame = fullFeatureFrame(3000);
    std::vector<float> deltas(more_phi::kOnnxOutputDeltaCount, 0.95f);

    const auto disposition = more_phi::evaluateNeuralMasteringProposal(deltas.data(), deltas.size(), frame);

    CHECK(disposition.confidence < 0.75f);
    CHECK_FALSE(disposition.abstain);
    CHECK(disposition.reviewOnly);
    CHECK(disposition.requestedFallbackMode == more_phi::NeuralMasteringFallbackMode::ReviewOnly);
}

// ── Runner seam behaviour (no ONNX linked) ────────────────────────────────────

TEST_CASE("OnnxNeuralMasteringRunner abstains when no model is loaded", "[OnnxNeuralMasteringRunner][seam]")
{
    more_phi::OnnxNeuralMasteringRunner runner;

    CHECK_FALSE(runner.isAvailable());
    CHECK_FALSE(runner.usesExternalInference());

    const auto frame = fullFeatureFrame(1000);
    const auto result = runner.proposePlan(frame);

    CHECK_FALSE(result.producedCandidate);
    CHECK_FALSE(result.usedModel);
    CHECK(result.candidate.abstain);
    CHECK(result.fallbackMode == more_phi::NeuralMasteringFallbackMode::DeterministicBaseline);
}

TEST_CASE("OnnxNeuralMasteringRunner loadModel returns false for a non-existent/unloadable path", "[OnnxNeuralMasteringRunner][lifecycle]")
{
    more_phi::OnnxNeuralMasteringRunner runner;

    // loadModel must fail for a path that does not resolve to a loadable model.
    // In the seam build (no ORT) it abstains before any file access; in the ONNX
    // build it attempts session creation and rolls back. Either way: not available.
    REQUIRE_FALSE(runner.loadModel("C:/models/mastering.onnx", "morephi-mst-v1", "sha256:abcd"));
    CHECK_FALSE(runner.isAvailable());

    const auto meta = runner.metadata();
    CHECK_FALSE(meta.audioCallbackInference);  // never runs on the audio thread, regardless of build
    CHECK_FALSE(meta.enabled);                 // not enabled until a real session exists

#if MORE_PHI_HAS_ONNX
    // ONNX linked: a failed load rolls the runner back to a clean state, so no
    // stale model id is left dangling in the metadata handle. This is the
    // desirable runtime contract — a botched reload never advertises the old id.
    CHECK(meta.modelId[0] == '\0');
#else
    // Seam (no ORT): loadModel abstains *before* attempting to open the file, so
    // the id/checksum that were recorded up-front remain visible in metadata.
    // This is purely a seam artefact and goes away once ONNX is linked.
    CHECK(meta.modelId[0] != '\0');
#endif

    runner.unloadModel();
    CHECK_FALSE(runner.isAvailable());
    CHECK(runner.metadata().modelId[0] == '\0');
}

// ── Controller integration: wiring the ONNX runner must be a no-op today ─────

TEST_CASE("Wiring OnnxNeuralMasteringRunner into the controller falls back to deterministic baseline", "[OnnxNeuralMasteringRunner][integration]")
{
    more_phi::OnnxNeuralMasteringRunner onnxRunner;
    more_phi::NeuralMasteringController controller;
    controller.setModelRunner(&onnxRunner);

    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto frame = extractor.extractFromSummary(48000.0, 2, 512, 1000, -14.0f, -1.0f, 1.5f).frame;

    more_phi::NeuralMasteringRuntimeState runtime;
    runtime.currentFrame = 1000;
    runtime.sampleRate = 48000.0;
    runtime.channelCount = 2;
    runtime.layout = more_phi::NeuralMasteringLayout::Stereo;

    const auto status = controller.processFeatureFrame(frame, runtime, false);

    CHECK(status.featureFrameValid);
    CHECK(status.plannerInvoked);
    // Because the runner abstained, the controller substituted the deterministic
    // baseline → modelInvoked stays false, exactly like the no-runner case.
    CHECK_FALSE(status.modelInvoked);
    CHECK(status.validationAccepted);
    CHECK(status.lastValidation.plan.valid);
}
