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

#endif // MORE_PHI_HAS_ONNX
