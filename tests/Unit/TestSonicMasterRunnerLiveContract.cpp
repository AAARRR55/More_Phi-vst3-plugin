// tests/Unit/TestSonicMasterRunnerLiveContract.cpp
//
// C1 empirical proof: exercise the EXACT production load path —
// SonicMasterDecisionRunner::loadModel(modelPath, contractPath) — with the real
// 3.4 MB embedded ONNX model AND its sibling contract. This is the call
// initializeSonicMaster() makes in every ONNX-enabled production build
// (PluginProcessor.cpp extracts both from BinaryData and passes them both).
//
// Why this test exists separately from TestSonicMasterRunnerLive.cpp: that test
// calls loadModel(modelPath) with NO contract, which skips the parser entirely
// (SonicMasterDecisionRunner.cpp:58-63). That gap is precisely what let C1
// (contract schema mismatch) hide — the no-contract test passed while every
// production load failed. This test forces the contract path so the defect
// cannot recur silently.
//
// Opt-in: only compiles + runs when MORE_PHI_HAS_ONNX AND the model + contract
// are staged next to the test exe. Skips cleanly otherwise.
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterDecisionRunner.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#if MORE_PHI_HAS_ONNX

namespace {
bool findStaged(std::string& modelOut, std::string& contractOut)
{
    using namespace juce;
    // Probe next to the test EXECUTABLE first (deterministic — the CMake staging
    // step copies the model + contract to $<TARGET_FILE_DIR:MorePhiTests>), then
    // fall back to cwd-relative for IDE/manual runs. Earlier this only probed
    // cwd-relative, which made the test pass/fail depending on the shell's cwd.
    const File exeDir = File::getSpecialLocation(File::currentApplicationFile).getParentDirectory();
    const File cwd = File::getCurrentWorkingDirectory();
    const char* const modelNames[] = {
        "masteringbrain_v2_decision.onnx",
    };
    const char* const contractNames[] = {
        "masteringbrain_v2_contract.json",
        "masteringbrain_v2_decision.contract.json",
    };
    auto findIn = [&](const File& dir, const char* const* names, int count, std::string& out) -> bool {
        for (int i = 0; i < count; ++i)
        {
            const File f = dir.getChildFile(String(names[i]));
            if (f.existsAsFile()) { out = f.getFullPathName().toStdString(); return true; }
        }
        return false;
    };
    bool gotModel = findIn(exeDir, modelNames, 1, modelOut)
                 || findIn(cwd, modelNames, 1, modelOut)
                 || findIn(cwd.getChildFile("tests"), modelNames, 1, modelOut);
    bool gotContract = findIn(exeDir, contractNames, 2, contractOut)
                    || findIn(cwd, contractNames, 2, contractOut)
                    || findIn(cwd.getChildFile("tests"), contractNames, 2, contractOut);
    return gotModel && gotContract;
}
} // namespace

TEST_CASE("SonicMasterDecisionRunner loads + infers via the PRODUCTION contract path (C1 proof)",
          "[SonicMaster][Live][Contract]")
{
    std::string modelPath, contractPath;
    if (!findStaged(modelPath, contractPath))
    {
        WARN("masteringbrain_v2_decision.onnx + contract not staged — skipping live C1 proof");
        return;
    }

    more_phi::SonicMasterDecisionRunner runner;

    // The exact production call: model + contract. Pre-C1 fix this returned
    // false (parseSonicMasterContract fail-closed on the wrong schema) and the
    // plugin fell through to HTTP-only. Post-fix it must load in-process.
    REQUIRE(runner.loadModel(modelPath, contractPath));
    REQUIRE(runner.isAvailable());

    // Run a real inference on a synthetic 5.94 s stereo signal and assert the
    // model produces 44 finite floats + a sane inference latency (Stage 5a
    // telemetry). This proves the whole chain: parse → ORT session → tensor →
    // inference → output, in-process, no HTTP server involved.
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

    // Stage 5a latency telemetry must report a real, finite, sub-3s-budget value.
    const float lastMs = runner.getLastInferenceMs();
    REQUIRE(std::isfinite(lastMs));
    REQUIRE(lastMs > 0.0f);
    REQUIRE(lastMs < 3000.0f);  // analysis cycle budget
}
#endif // MORE_PHI_HAS_ONNX
