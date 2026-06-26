// tests/Unit/TestSonicMasterRunnerLive.cpp
//
// Opt-in live-inference smoke test for SonicMasterDecisionRunner. Only compiled
// when MORE_PHI_HAS_ONNX is defined (see tests/CMakeLists.txt). When the
// exported masteringbrain_v2_decision.onnx + contract are staged next to the
// test exe, this loads them, runs inference on a 6s synthetic sweep, and
// asserts the output is 44 finite floats in the expected ranges under 500ms.
//
// If the model is not staged, the test WARNs and returns (does not fail), so
// the standard CI build (no model) stays green.
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterDecisionRunner.h"
#include "AI/SonicMasterDecisionDecoder.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#if MORE_PHI_HAS_ONNX

namespace {

// Probe a few candidate locations (relative to CWD) for the staged model +
// contract. The CMake staging step copies both next to the test exe; the
// build/sonicmaster fallback covers running from the repo root.
bool findFile(const char* const* candidates, int count, std::string& out)
{
    for (int i = 0; i < count; ++i)
    {
        if (juce::File::isAbsolutePath(juce::String(candidates[i]))
                ? juce::File(candidates[i]).existsAsFile()
                : juce::File::getCurrentWorkingDirectory().getChildFile(candidates[i]).existsAsFile())
        {
            out = candidates[i];
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("SonicMasterDecisionRunner loads and infers the real ONNX model",
          "[SonicMaster][Live]")
{
    const char* modelCandidates[] = {
        "masteringbrain_v2_decision.onnx",
        "build/sonicmaster/masteringbrain_v2_decision.onnx",
        "build/tests/Release/masteringbrain_v2_decision.onnx",
    };
    std::string modelPath;
    if (!findFile(modelCandidates, 3, modelPath))
    {
        WARN("masteringbrain_v2_decision.onnx not staged — skipping live test");
        return;
    }

    more_phi::SonicMasterDecisionRunner runner;
    REQUIRE(runner.loadModel(modelPath));
    REQUIRE(runner.isAvailable());

    // Synthetic 6 s stereo: 220 Hz sine + 2.5 kHz harmonic at -14 dBFS-ish.
    std::vector<float> interleaved(2 * more_phi::kSonicMasterSegmentFrames, 0.0f);
    for (std::size_t i = 0; i < more_phi::kSonicMasterSegmentFrames; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        const float v = 0.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * t));
        interleaved[2 * i + 0] = v;
        interleaved[2 * i + 1] = v;
    }

    float decision[more_phi::kSonicMasterDecisionWidth] {};
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(runner.runDecision(interleaved.data(), decision, more_phi::kSonicMasterDecisionWidth));
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Every output must be finite; the target-LUFS slot must sit in the schema
    // band the decoder clamps to.
    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        REQUIRE(std::isfinite(decision[i]));
    CHECK(decision[more_phi::kSonicMasterTargetLufsIdx] >= -30.0f);
    CHECK(decision[more_phi::kSonicMasterTargetLufsIdx] <= -6.0f);
    // CPU inference budget guard (catches a pathological export).
    CHECK(dt < 500);
}

// AUDIT (C1 verify, 2026-06-25): the production load path
// (PluginProcessor.cpp:269-273) calls loadModel(modelPath, contractPath) WITH the
// contract, unconditionally. The sibling test above calls loadModel(modelPath)
// with NO contract — which skipped the contract block and hid the C1 defect
// (the shipped contract's schema didn't match parseSonicMasterContract, so the
// parser fail-closed and loadModel returned false, silently forcing HTTP-only
// in every production build). This test reproduces the EXACT production path:
// it stages both files and asserts the model loads WITH the contract. If the
// contract schema ever drifts from the parser again, this fails where the
// sibling test would still pass.
TEST_CASE("SonicMasterDecisionRunner loads with the contract (production path, C1 guard)",
          "[SonicMaster][Live]")
{
    const char* modelCandidates[] = {
        "masteringbrain_v2_decision.onnx",
        "build/sonicmaster/masteringbrain_v2_decision.onnx",
    };
    const char* contractCandidates[] = {
        "masteringbrain_v2_decision.contract.json",
        "build/sonicmaster/masteringbrain_v2_contract.json",
    };
    std::string modelPath;
    std::string contractPath;
    if (!findFile(modelCandidates, 2, modelPath))
    {
        WARN("masteringbrain_v2_decision.onnx not staged — skipping live C1 guard");
        return;
    }
    if (!findFile(contractCandidates, 2, contractPath))
    {
        WARN("masteringbrain_v2_decision.contract.json not staged — skipping live C1 guard");
        return;
    }

    more_phi::SonicMasterDecisionRunner runner;
    // This is the line that returned false before the C1 fix (commit c91ad2e).
    REQUIRE(runner.loadModel(modelPath, contractPath));
    REQUIRE(runner.isAvailable());

    // And inference must still succeed end-to-end on the contract-validated model.
    std::vector<float> interleaved(2 * more_phi::kSonicMasterSegmentFrames, 0.0f);
    for (std::size_t i = 0; i < more_phi::kSonicMasterSegmentFrames; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        const float v = 0.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * t));
        interleaved[2 * i + 0] = v;
        interleaved[2 * i + 1] = v;
    }
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    REQUIRE(runner.runDecision(interleaved.data(), decision, more_phi::kSonicMasterDecisionWidth));
    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        REQUIRE(std::isfinite(decision[i]));
}

#endif // MORE_PHI_HAS_ONNX
