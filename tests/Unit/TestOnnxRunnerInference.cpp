/*
 * More-Phi — tests/Unit/TestOnnxRunnerInference.cpp
 *
 * End-to-end test of the OnnxNeuralMasteringRunner inference path against a
 * real trained ONNX artifact. Compiled ONLY when MORE_PHI_HAS_ONNX=1 (the
 * runner's real session path is active). When ORT is not linked, the file
 * compiles to a single skipped-test placeholder so the suite stays green on
 * default builds.
 *
 * The test loads the staged model artifact (model_scaled.onnx, produced by the
 * Python training pipeline), runs inference on a representative feature frame,
 * and asserts the contract the safety policy + DSP rely on: 72 finite deltas
 * in [-1, 1], deterministic, and that a successful load flips isAvailable() on.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/OnnxNeuralMasteringRunner.h"
#include "Core/NeuralMasteringTypes.h"

#include <cmath>
#include <filesystem>
#include <limits>

using Catch::Approx;

#if MORE_PHI_HAS_ONNX

namespace {

// Locate the staged model artifact. The build copies it next to the test exe
// (see tests/CMakeLists.txt ORT block); fall back to the source-tree path used
// during development.
std::filesystem::path resolve_model_path()
{
    for (const auto& candidate : {
             std::filesystem::current_path() / "model_scaled.onnx",
             std::filesystem::path("scripts/neural-mastering/control/model_scaled.onnx"),
         })
    {
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

more_phi::NeuralMasteringFeatureFrame representative_frame()
{
    more_phi::NeuralMasteringFeatureFrame frame {};
    frame.schemaVersion = more_phi::kNeuralMasteringFeatureSchemaVersion;
    frame.sampleRate = 48000.0;
    frame.channelCount = 2;
    frame.blockSize = 512;
    frame.frameIndex = 1000;
    frame.integratedLUFS = -14.0f;
    frame.shortTermLUFS = -12.0f;
    frame.momentaryLUFS = -10.0f;
    frame.loudnessRange = 7.0f;
    frame.truePeakDbTp = -1.0f;
    frame.spectralTilt = 1.5f;
    frame.monoFoldDownDeltaDb = 0.0f;   // runtime stub
    frame.transientDensity = 0.0f;       // runtime stub
    frame.harmonicRisk = 0.0f;           // runtime stub
    frame.sourceQualityScore = 1.0f;     // runtime constant
    for (auto& v : frame.spectralBands) v = -60.0f;
    frame.spectralBands[0] = -20.0f;
    for (auto& v : frame.stereoCorrelation) v = 0.8f;
    for (auto& v : frame.midSideRatio) v = 0.3f;
    return frame;
}

} // namespace

TEST_CASE("OnnxNeuralMasteringRunner loads and infers on a real artifact", "[OnnxNeuralMasteringRunner][inference][onnx]")
{
    const auto model_path = resolve_model_path();
    if (model_path.empty())
    {
        WARN("model_scaled.onnx not found — skipping live ONNX inference test. "
             "Stage the artifact via the Python training pipeline to exercise this path.");
        return;
    }

    more_phi::OnnxNeuralMasteringRunner runner;
    REQUIRE_FALSE(runner.isAvailable());

    // loadModel must succeed AND flip availability for a valid v1 artifact.
    REQUIRE(runner.loadModel(model_path.string(), "morephi-control-scaled", "sha256:unset"));
    REQUIRE(runner.isAvailable());
    REQUIRE(runner.usesExternalInference());

    const auto frame = representative_frame();
    const auto first = runner.proposePlan(frame);
    REQUIRE(first.producedCandidate);
    REQUIRE(first.usedModel);
    REQUIRE_FALSE(first.candidate.abstain);

    // Contract: 72 finite deltas, all in [-1, 1] (tanh head + sanitizePlanCandidate).
    const auto check_deltas = [](const more_phi::MasteringTargetVector& v) {
        auto finite_in_range = [](float x) {
            return std::isfinite(x) && x >= -1.0f - 1e-5f && x <= 1.0f + 1e-5f;
        };
        for (float x : v.eq) REQUIRE(finite_in_range(x));
        for (float x : v.dynamics) REQUIRE(finite_in_range(x));
        for (float x : v.stereo) REQUIRE(finite_in_range(x));
        for (float x : v.harmonic) REQUIRE(finite_in_range(x));
        for (float x : v.limiter) REQUIRE(finite_in_range(x));
        for (float x : v.loudness) REQUIRE(finite_in_range(x));
    };
    check_deltas(first.candidate.targets);
    check_deltas(first.candidate.deltas);

    // Determinism: same frame -> identical plan (the runner reuses buffers and
    // the safety policy treats the result as a stable plan).
    const auto second = runner.proposePlan(frame);
    REQUIRE(second.producedCandidate);
    for (std::size_t i = 0; i < first.candidate.deltas.eq.size(); ++i)
        REQUIRE(first.candidate.deltas.eq[i] == Approx(second.candidate.deltas.eq[i]));

    runner.unloadModel();
    REQUIRE_FALSE(runner.isAvailable());
}

TEST_CASE("OnnxNeuralMasteringRunner rejects a non-existent path", "[OnnxNeuralMasteringRunner][onnx]")
{
    more_phi::OnnxNeuralMasteringRunner runner;
    REQUIRE_FALSE(runner.loadModel("nonexistent/path.onnx", "x", "y"));
    REQUIRE_FALSE(runner.isAvailable());
}

#else // !MORE_PHI_HAS_ONNX — ORT not linked; suite stays green on default builds.

TEST_CASE("OnnxNeuralMasteringRunner inference path requires MORE_PHI_ENABLE_ONNX", "[OnnxNeuralMasteringRunner][onnx]")
{
    INFO("Built without MORE_PHI_ENABLE_ONNX — live ONNX inference path is dormant "
         "and OnnxNeuralMasteringRunner abstains. Reconfigure with -DMORE_PHI_ENABLE_ONNX=ON "
         "to exercise this test against a staged model artifact.");
    SUCCEED("ONNX inference test skipped (ORT not linked) — default build posture.");
}

#endif // MORE_PHI_HAS_ONNX
