// tests/Unit/TestSonicMasterContractParsing.cpp
//
// Regression guard for AUDIT finding C1 (2026-06-25): the shipped
// masteringbrain_v2_decision.contract.json had a totally different key schema
// than parseSonicMasterContract() + SonicMasterModelContract::validate()
// require (e.g. "schemaVersion"/"inputShape" instead of "schema"/"input_layout").
// The parser fail-closed at the very first j.contains("schema") check, so
// loadModel() returned false whenever the contract path was supplied — which
// is exactly what initializeSonicMaster() does in production (PluginProcessor
// extracts the embedded contract to a temp file and passes it unconditionally).
// The net effect: the primary ONNX inference path could never load in any
// ONNX-enabled production build and silently fell through to HTTP-only
// (PluginProcessor.cpp:315).
//
// This test is deliberately ONNX-INDEPENDENT: it exercises contract parsing +
// validation only, which is pure JSON logic. It runs in every CI config and
// would have caught the C1 defect regardless of whether the model was staged.
// It also writes synthetic bad-contract variants to temp files and asserts the
// parser remains fail-closed for each, pinning the AUDIT-FIX (A2) behaviour.
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterDecisionRunner.h"

#include <juce_core/juce_core.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

// Resolve the shipped contract relative to the test exe, the repo root, and a
// few common build layouts. Mirrors the candidate probing in the live test so
// the guard works from any CWD.
bool resolveShippedContract(std::string& outPath)
{
    using namespace juce;
    const File cwd = File::getCurrentWorkingDirectory();
    const char* const candidates[] = {
        // Repo layout: <root>/models/sonicmaster/...
        "models/sonicmaster/masteringbrain_v2_decision.contract.json",
        // Run from inside build/ or build-ninja/
        "../models/sonicmaster/masteringbrain_v2_decision.contract.json",
        "../../models/sonicmaster/masteringbrain_v2_decision.contract.json",
        "../../../models/sonicmaster/masteringbrain_v2_decision.contract.json",
    };
    for (const char* c : candidates)
    {
        const File f = File::isAbsolutePath(String(c)) ? File(c) : cwd.getChildFile(c);
        if (f.existsAsFile())
        {
            outPath = f.getFullPathName().toStdString();
            return true;
        }
    }
    return false;
}

// Write `body` to a unique temp file and return its absolute path. Used to feed
// synthetic broken contracts to the parser for the fail-closed assertions.
bool writeTempContract(const std::string& body, std::string& outPath)
{
    using namespace juce;
    const auto stamp = static_cast<unsigned long long>(Time::getMillisecondCounterHiRes());
    const File tmp = File::getSpecialLocation(File::SpecialLocationType::tempDirectory)
                         .getChildFile("morephi_sm_contract_" + String(stamp) + ".json");
    if (!tmp.replaceWithData(body.data(), static_cast<int>(body.size())))
        return false;
    outPath = tmp.getFullPathName().toStdString();
    return true;
}

} // namespace

TEST_CASE("SonicMaster contract parser accepts the shipped contract (C1 regression)",
          "[SonicMaster][Contract]")
{
    std::string shipped;
    if (!resolveShippedContract(shipped))
    {
        WARN("Shipped masteringbrain_v2_decision.contract.json not found relative to CWD — "
             "the C1 guard cannot run. Run this test from the repo root or a build subdir.");
        return;
    }

    more_phi::SonicMasterModelContract contract;
    REQUIRE(more_phi::parseSonicMasterContract(shipped, contract));

    // validate() must pass: every scalar must equal the runtime contract
    // constant (SonicMasterDecisionRunner.h:69-78). This is the exact check
    // loadModel() makes before it ever touches ORT.
    REQUIRE(contract.validate());

    // Belt-and-braces: assert each field against the documented runtime
    // constants so a silent numeric drift in the contract is caught with a
    // useful diff rather than a bare validate()==false.
    CHECK(contract.schema == more_phi::kSonicMasterContractSchema);
    CHECK(contract.inputLayout == more_phi::kSonicMasterInputLayout);
    CHECK(contract.normalization == more_phi::kSonicMasterNormalization);
    CHECK(contract.dtype == more_phi::kSonicMasterDtype);
    CHECK(contract.segmentFrames == more_phi::kSonicMasterSegmentFrames);
    CHECK(std::abs(contract.peakTargetLinear - more_phi::kSonicMasterPeakTargetLinear) < 1e-4f);
}

TEST_CASE("SonicMaster contract parser remains fail-closed on bad schemas (AUDIT-FIX A2)",
          "[SonicMaster][Contract]")
{
    // The pre-fix contract shape: schemaVersion/modelId/inputShape/outputShape
    // with NONE of the parser's required keys. Must be rejected.
    {
        std::string path;
        REQUIRE(writeTempContract(
            R"({"schemaVersion":1,"modelId":"x","inputName":"waveform",)"
            R"("outputName":"decision","inputShape":[1,2,262138],"outputShape":[1,44],)"
            R"("sampleRate":44100,"targetLufsDefault":-14.0})",
            path));
        more_phi::SonicMasterModelContract c;
        REQUIRE_FALSE(more_phi::parseSonicMasterContract(path, c));
    }
    // Missing one required key (schema) → reject.
    {
        std::string path;
        REQUIRE(writeTempContract(
            R"({"input_layout":"deinterleaved_chw","normalization":"peak_to_-1dBFS",)"
            R"("dtype":"float32","sample_rate":44100,"segment_frames":262138,)"
            R"("peak_target_linear":0.89125094})",
            path));
        more_phi::SonicMasterModelContract c;
        REQUIRE_FALSE(more_phi::parseSonicMasterContract(path, c));
    }
    // Right keys, wrong normalization string → parses but validate() fails.
    {
        std::string path;
        REQUIRE(writeTempContract(
            R"({"schema":1,"input_layout":"deinterleaved_chw","normalization":"lufs",)"
            R"("dtype":"float32","sample_rate":44100,"segment_frames":262138,)"
            R"("peak_target_linear":0.89125094})",
            path));
        more_phi::SonicMasterModelContract c;
        REQUIRE(more_phi::parseSonicMasterContract(path, c));
        REQUIRE_FALSE(c.validate());
    }
    // Right keys, wrong peak_target_linear → validate() fails (out-of-distribution).
    {
        std::string path;
        REQUIRE(writeTempContract(
            R"({"schema":1,"input_layout":"deinterleaved_chw",)"
            R"("normalization":"peak_to_-1dBFS","dtype":"float32",)"
            R"("sample_rate":44100,"segment_frames":262138,)"
            R"("peak_target_linear":0.5})",
            path));
        more_phi::SonicMasterModelContract c;
        REQUIRE(more_phi::parseSonicMasterContract(path, c));
        REQUIRE_FALSE(c.validate());
    }
    // Malformed JSON → reject, no throw.
    {
        std::string path;
        REQUIRE(writeTempContract("{ not json ", path));
        more_phi::SonicMasterModelContract c;
        REQUIRE_FALSE(more_phi::parseSonicMasterContract(path, c));
    }
    // Nonexistent path → reject.
    {
        more_phi::SonicMasterModelContract c;
        REQUIRE_FALSE(more_phi::parseSonicMasterContract(
            "definitely_not_a_contract_path_zzz.json", c));
    }
}
