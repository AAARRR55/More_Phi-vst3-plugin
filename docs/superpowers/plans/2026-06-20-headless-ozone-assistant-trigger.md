# Headless Ozone Assistant Trigger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a staged, gated workflow that locates the Ozone Assistant trigger function statically, certifies it read-only, and invokes it headlessly — via one MCP tool (`ozone_trigger_assistant_headless`) with `isolated` (default) and `live` (opt-in) backends.

**Architecture:** A versioned *trigger manifest* (DLL SHA-256 + trigger RVA + calling convention) is the safety contract. A new `HeadlessAssistantTrigger` class loads the manifest, verifies the loaded DLL hash, and dispatches to either an in-process call on a More-Phi-hosted Ozone instance (`isolated`) or an out-of-process Frida call into a live FL64 PID (`live`). Every stage produces a reviewable artifact; the two-part write gate plus capture-only defaults protect the live session.

**Tech Stack:** C++20, JUCE 8 (audio formats / processors), nlohmann/json, Catch2 v3, Python 3 (orchestration + Frida), pefile + capstone (existing recon), Frida (live Stage-3 call).

**Spec:** `docs/superpowers/specs/2026-06-20-headless-ozone-assistant-design.md`

**Build/test commands (Windows, from repo root):**
```bash
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --target MorePhiMcpServerTests
cd build && ctest -C Release -R "HeadlessAssistantTrigger" --output-on-failure
```

---

## File Structure

### New files
- `src/AI/StandaloneMcp/TriggerManifest.h` — POD struct + JSON load/validate for the trigger manifest (DLL hash, RVA, calling convention, phase markers). Pure data + parsing, no process interaction.
- `src/AI/StandaloneMcp/HeadlessAssistantTrigger.h` — interface + factory; declares `HeadlessAssistantTrigger` with `setFakeTriggerForTests()`, `runIsolated()`, `runLive()`.
- `src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp` — manifest loading, DLL hash verification, dispatch, isolated backend (in-process call), live backend (spawns Python Frida launcher).
- `docs/ozone_trigger_manifest.example.json` — committed example of the manifest schema (with placeholder hash, marked unsigned).
- `tools/ozone_trigger_workflow.py` — Stage 0/2/3/4 orchestration driver (Stage 1 is manual).
- `tools/ozone_trigger_correlate.py` — Stage 1: correlates a GUI click to a phase-state transition in a state-diff trace.
- `tools/ozone_trigger_frida.js` — Stage 3: Frida script that calls the trigger function inside FL64.
- `tests/Unit/TestHeadlessAssistantTrigger.cpp` — fake-target unit tests for manifest validation, hash pinning, gates, dispatch.

### Modified files
- `src/AI/StandaloneMcp/StandaloneMcpServer.h` — add `headlessTrigger` member + constructor param.
- `src/AI/StandaloneMcp/StandaloneMcpServer.cpp` — register the new tool descriptor and `tools/call` handler.
- `src/AI/StandaloneMcp/StandaloneMcpMain.cpp` — forward the new component if the main wires members explicitly (verify; constructor default may suffice).
- `tests/CMakeLists.txt` — add `Unit/TestHeadlessAssistantTrigger.cpp` to `MorePhiMcpServerTests`.

### No changes to
`OzonePluginBackend`, `IZotopeIPCAssistant`, the audio engine, the audio thread.

---

## Task 1: Trigger manifest data type + JSON load

**Files:**
- Create: `src/AI/StandaloneMcp/TriggerManifest.h`
- Test: `tests/Unit/TestHeadlessAssistantTrigger.cpp`

- [ ] **Step 1: Create the test file with the manifest-loading failing test**

Create `tests/Unit/TestHeadlessAssistantTrigger.cpp`:

```cpp
// TestHeadlessAssistantTrigger.cpp
// Deterministic unit tests for the headless Ozone Assistant trigger.
// No live DAW, no real Ozone DLL. All invocation paths use fake triggers.

#include "AI/StandaloneMcp/TriggerManifest.h"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace more_phi::standalone_mcp;
using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
// TriggerManifest
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("TriggerManifest parses a well-formed manifest", "[headless][manifest]")
{
    json src = R"({
        "schema_version": 1,
        "dll_sha256": "ab12cd34",
        "dll_path": "C:/Ozone/iZOzonePro.dll",
        "trigger_rva": "0x1234",
        "calling_convention": "microsoft_x64",
        "phase_state_markers": ["PROCESSING_LISTENING"],
        "certified_at_unix": 1718800000,
        "certified_by": "operator-handle",
        "isolation_test_passed": true
    })"_json;

    auto manifest = TriggerManifest::fromJson(src);
    REQUIRE(manifest.has_value());
    CHECK(manifest->schemaVersion == 1);
    CHECK(manifest->dllSha256 == "ab12cd34");
    CHECK(manifest->triggerRva == 0x1234);
    CHECK(manifest->callingConvention == "microsoft_x64");
    CHECK(manifest->isolationTestPassed == true);
}

TEST_CASE("TriggerManifest rejects a placeholder hash", "[headless][manifest]")
{
    json src = R"({
        "schema_version": 1,
        "dll_sha256": "ab12…",
        "dll_path": "C:/Ozone/iZOzonePro.dll",
        "trigger_rva": "0x1234",
        "calling_convention": "microsoft_x64",
        "phase_state_markers": ["PROCESSING_LISTENING"],
        "certified_at_unix": 1718800000,
        "certified_by": "operator-handle",
        "isolation_test_passed": true
    })"_json;

    auto manifest = TriggerManifest::fromJson(src);
    CHECK_FALSE(manifest.has_value());  // ellipsis placeholder => unsigned => rejected
}

TEST_CASE("TriggerManifest rejects a missing required field", "[headless][manifest]")
{
    json src = R"({
        "schema_version": 1,
        "dll_sha256": "ab12cd34",
        "trigger_rva": "0x1234"
    })"_json;

    auto manifest = TriggerManifest::fromJson(src);
    CHECK_FALSE(manifest.has_value());
}
```

- [ ] **Step 2: Run the test to verify it fails (compile error — header absent)**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests 2>&1 | findstr /C:"TriggerManifest" /C:"fatal error"`
Expected: FAIL — `cannot open source file "AI/StandaloneMcp/TriggerManifest.h"` (the test source is not yet in CMakeLists, so also expect it to be skipped; the failure we care about is the missing header once Task 9 wires it in — for now confirm the file is syntactically valid Python-free C++ by creating the header next).

- [ ] **Step 3: Create the header with the manifest struct**

Create `src/AI/StandaloneMcp/TriggerManifest.h`:

```cpp
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace more_phi::standalone_mcp {

// The versioned safety contract produced by recon (Stage 0) and validation
// (Stage 1). Every invocation re-reads this and re-checks the DLL hash.
//
// A manifest is "signed/certified" iff it has a real SHA-256 (not the
// illustrative placeholder) AND isolationTestPassed == true. Loading rejects
// the placeholder hash and missing required fields so an unsigned manifest can
// never reach an invocation backend.
struct TriggerManifest
{
    int schemaVersion = 0;
    std::string dllSha256;          // 64 hex chars; placeholder "ab12…" rejected
    std::string dllPath;
    std::uintptr_t triggerRva = 0;  // parsed from hex string
    std::string callingConvention;  // "microsoft_x64" (only value accepted today)
    std::vector<std::string> phaseStateMarkers;
    std::int64_t certifiedAtUnix = 0;
    std::string certifiedBy;
    bool isolationTestPassed = false;

    // Parse + validate. Returns std::nullopt on any structural problem or on a
    // placeholder/unsigned hash. Does NOT touch any process or file on disk —
    // it only validates the JSON object handed to it.
    static std::optional<TriggerManifest> fromJson(const nlohmann::json& src);

    // True iff the manifest carries a real 64-hex-char SHA-256 (no ellipsis,
    // correct length, hex-only) AND isolationTestPassed.
    bool isCertified() const;

    // Recompute the SHA-256 of the DLL at dllPath on disk and compare to
    // dllSha256. Returns true on match. Uses juce::SHA256 (linked in the test
    // target via juce_core). Returns false if the file cannot be read.
    static bool dllHashMatches(const std::string& dllPath, const std::string& expectedSha256);
};

} // namespace more_phi::standalone_mcp
```

- [ ] **Step 4: Verify the test still fails as expected (no impl yet)**

At this point the header exists but `fromJson`/`isCertified`/`dllHashMatches` are declared but not defined. The test file is not yet in CMakeLists (Task 9). No build action yet — proceed to Task 2 which defines them.

- [ ] **Step 5: Commit (header skeleton + tests)**

```bash
git add src/AI/StandaloneMcp/TriggerManifest.h tests/Unit/TestHeadlessAssistantTrigger.cpp
git commit -m "feat(headless-trigger): TriggerManifest struct + parsing tests (red)"
```

---

## Task 2: Manifest parsing implementation (make Task 1 tests green)

**Files:**
- Create: `src/AI/StandaloneMcp/TriggerManifest.cpp`
- Modify: `tests/CMakeLists.txt:192-195` (add new test source so it compiles)

- [ ] **Step 1: Add the new test source to the test executable**

In `tests/CMakeLists.txt`, change the `MorePhiMcpServerTests` source list (currently lines 192–195):

```cmake
add_executable(MorePhiMcpServerTests
    Unit/TestStandaloneMcpServer.cpp
    Unit/TestIZotopeIPCAssistant.cpp
    Unit/TestHeadlessAssistantTrigger.cpp
)
```

Then add the new implementation source to the same executable's sources (it must compile into the test target so the test can see the definitions). Add after the `add_executable` block, before `target_link_libraries`:

```cmake
target_sources(MorePhiMcpServerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/AI/StandaloneMcp/TriggerManifest.cpp
)
```

- [ ] **Step 2: Run the tests to verify they fail to link**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests 2>&1 | findstr /C:"fromJson" /C:"LNK2019"`
Expected: FAIL — unresolved external `TriggerManifest::fromJson` (definition not yet written).

- [ ] **Step 3: Write the manifest implementation**

Create `src/AI/StandaloneMcp/TriggerManifest.cpp`:

```cpp
#include "AI/StandaloneMcp/TriggerManifest.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>

namespace more_phi::standalone_mcp {

namespace {

bool isHex64(const std::string& s)
{
    if (s.size() != 64)
        return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::uintptr_t parseHexRva(const std::string& s)
{
    try
    {
        return static_cast<std::uintptr_t>(std::stoull(s, nullptr, 16));
    }
    catch (...)
    {
        return 0;
    }
}

} // namespace

std::optional<TriggerManifest> TriggerManifest::fromJson(const nlohmann::json& src)
{
    if (!src.is_object())
        return std::nullopt;

    TriggerManifest m;

    // Required scalar fields.
    if (!src.contains("schema_version") || !src["schema_version"].is_number_integer())
        return std::nullopt;
    m.schemaVersion = src["schema_version"].get<int>();
    if (m.schemaVersion != 1)
        return std::nullopt;

    if (!src.contains("dll_sha256") || !src["dll_sha256"].is_string())
        return std::nullopt;
    m.dllSha256 = src["dll_sha256"].get<std::string>();
    // Reject the illustrative placeholder and anything that isn't a real hash.
    // isCertified() re-checks this, but we reject early so an unsigned manifest
    // can never even be constructed.
    if (!isHex64(m.dllSha256))
        return std::nullopt;

    if (!src.contains("dll_path") || !src["dll_path"].is_string())
        return std::nullopt;
    m.dllPath = src["dll_path"].get<std::string>();

    if (!src.contains("trigger_rva") || !src["trigger_rva"].is_string())
        return std::nullopt;
    m.triggerRva = parseHexRva(src["trigger_rva"].get<std::string>());
    if (m.triggerRva == 0)
        return std::nullopt;

    if (!src.contains("calling_convention") || !src["calling_convention"].is_string())
        return std::nullopt;
    m.callingConvention = src["calling_convention"].get<std::string>();
    if (m.callingConvention != "microsoft_x64")
        return std::nullopt;

    if (!src.contains("phase_state_markers") || !src["phase_state_markers"].is_array())
        return std::nullopt;
    for (const auto& marker : src["phase_state_markers"])
    {
        if (!marker.is_string())
            return std::nullopt;
        m.phaseStateMarkers.push_back(marker.get<std::string>());
    }
    if (m.phaseStateMarkers.empty())
        return std::nullopt;

    if (src.contains("certified_at_unix") && src["certified_at_unix"].is_number_integer())
        m.certifiedAtUnix = src["certified_at_unix"].get<std::int64_t>();

    if (src.contains("certified_by") && src["certified_by"].is_string())
        m.certifiedBy = src["certified_by"].get<std::string>();

    if (src.contains("isolation_test_passed") && src["isolation_test_passed"].is_boolean())
        m.isolationTestPassed = src["isolation_test_passed"].get<bool>();

    return m;
}

bool TriggerManifest::isCertified() const
{
    return isHex64(dllSha256) && isolationTestPassed && !phaseStateMarkers.empty();
}

bool TriggerManifest::dllHashMatches(const std::string& dllPath, const std::string& expectedSha256)
{
    juce::File file{juce::String{dllPath}};
    juce::FileInputStream stream{file};
    if (!stream.openedOk())
        return false;

    juce::SHA256 sha;
    const int chunk = 64 * 1024;
    for (;;)
    {
        auto block = stream.read(chunk);
        if (block.isEmpty())
            break;
        sha.update(block.getData(), static_cast<int>(block.getSize()));
    }
    return sha.toHexString().toStdString() == expectedSha256;
}

} // namespace more_phi::standalone_mcp
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests && cd build && ctest -C Release -R "TriggerManifest" --output-on-failure`
Expected: PASS — 3 tests (parses, rejects placeholder, rejects missing field).

- [ ] **Step 5: Commit**

```bash
git add src/AI/StandaloneMcp/TriggerManifest.cpp tests/CMakeLists.txt
git commit -m "feat(headless-trigger): TriggerManifest JSON parsing + DLL hash verify (green)"
```

---

## Task 3: DLL-hash-pinning test + isCertified test

**Files:**
- Modify: `tests/Unit/TestHeadlessAssistantTrigger.cpp`

- [ ] **Step 1: Append the hash-pinning and certification tests**

Append to `tests/Unit/TestHeadlessAssistantTrigger.cpp`:

```cpp
// ═══════════════════════════════════════════════════════════════════════════
// isCertified + dllHashMatches
// ═══════════════════════════════════════════════════════════════════════════

namespace {
TriggerManifest makeCertifiedManifest()
{
    json src = R"({
        "schema_version": 1,
        "dll_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "dll_path": "does-not-exist.dll",
        "trigger_rva": "0x1234",
        "calling_convention": "microsoft_x64",
        "phase_state_markers": ["PROCESSING_LISTENING"],
        "certified_at_unix": 1718800000,
        "certified_by": "operator-handle",
        "isolation_test_passed": true
    })"_json;
    return *TriggerManifest::fromJson(src);
}
} // namespace

TEST_CASE("TriggerManifest.isCertified true when signed + isolation-tested", "[headless][manifest]")
{
    auto m = makeCertifiedManifest();
    CHECK(m.isCertified());
}

TEST_CASE("TriggerManifest.isCertified false when isolation test not passed", "[headless][manifest]")
{
    auto m = makeCertifiedManifest();
    m.isolationTestPassed = false;
    CHECK_FALSE(m.isCertified());
}

TEST_CASE("TriggerManifest.dllHashMatches false for a missing file", "[headless][manifest]")
{
    // No fixture file on disk => stream.openedOk() false => false.
    CHECK_FALSE(TriggerManifest::dllHashMatches("definitely-not-a-real-file.dll",
                                                std::string(64, '0')));
}
```

- [ ] **Step 2: Run the tests to verify they pass**

Run: `cd build && ctest -C Release -R "TriggerManifest" --output-on-failure`
Expected: PASS — 6 tests total now.

- [ ] **Step 3: Commit**

```bash
git add tests/Unit/TestHeadlessAssistantTrigger.cpp
git commit -m "test(headless-trigger): isCertified + dllHashMatches coverage"
```

---

## Task 4: HeadlessAssistantTrigger interface + fake-trigger infrastructure

**Files:**
- Create: `src/AI/StandaloneMcp/HeadlessAssistantTrigger.h`
- Modify: `tests/Unit/TestHeadlessAssistantTrigger.cpp`

- [ ] **Step 1: Append the fake-trigger gate tests (red)**

Append to `tests/Unit/TestHeadlessAssistantTrigger.cpp`:

```cpp
#include "AI/StandaloneMcp/HeadlessAssistantTrigger.h"

// ═══════════════════════════════════════════════════════════════════════════
// HeadlessAssistantTrigger — gate + dispatch logic (fake trigger)
// ═══════════════════════════════════════════════════════════════════════════

namespace {
int g_fakeCallCount = 0;
void fakeTriggerFn() { ++g_fakeCallCount; }
} // namespace

TEST_CASE("Headless trigger refuses an unsigned manifest", "[headless][gate]")
{
    auto trigger = createHeadlessAssistantTrigger();
    // No manifest loaded => refuse.
    auto outcome = trigger->runIsolated({});
    REQUIRE(outcome.isError);
    CHECK(outcome.body["error"].get<std::string>() == "trigger_not_certified");
    CHECK(g_fakeCallCount == 0);
}

TEST_CASE("Headless isolated trigger calls the fake trigger when manifest is certified", "[headless][isolated]")
{
    g_fakeCallCount = 0;
    auto trigger = createHeadlessAssistantTrigger();
    trigger->setFakeTriggerForTests(fakeTriggerFn);

    // Load a certified manifest from an in-memory JSON object. The fake trigger
    // bypasses DLL-hash verification (see header contract).
    nlohmann::json manifestJson = R"({
        "schema_version": 1,
        "dll_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "dll_path": "fake.dll",
        "trigger_rva": "0x1234",
        "calling_convention": "microsoft_x64",
        "phase_state_markers": ["PROCESSING_LISTENING"],
        "certified_at_unix": 1718800000,
        "certified_by": "tests",
        "isolation_test_passed": true
    })"_json;
    REQUIRE(trigger->loadManifestForTests(manifestJson));

    auto outcome = trigger->runIsolated({});
    REQUIRE_FALSE(outcome.isError);
    CHECK(outcome.body["success"].get<bool>() == true);
    CHECK(outcome.body["target_mode"].get<std::string>() == "isolated");
    CHECK(g_fakeCallCount == 1);
}

TEST_CASE("Headless live trigger refuses without the two-part gate", "[headless][gate]")
{
    g_fakeCallCount = 0;
    auto trigger = createHeadlessAssistantTrigger();
    trigger->setFakeTriggerForTests(fakeTriggerFn);
    nlohmann::json manifestJson = R"({
        "schema_version": 1,
        "dll_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "dll_path": "fake.dll",
        "trigger_rva": "0x1234",
        "calling_convention": "microsoft_x64",
        "phase_state_markers": ["PROCESSING_LISTENING"],
        "certified_at_unix": 1718800000,
        "certified_by": "tests",
        "isolation_test_passed": true
    })"_json;
    REQUIRE(trigger->loadManifestForTests(manifestJson));

    // No env gate, no per-call flag => refuse even with a certified manifest.
    HeadlessTriggerArgs args;
    args.targetMode = "live";
    args.allowUnsafeWrite = false;
    auto outcome = trigger->runLive(args);
    REQUIRE(outcome.isError);
    CHECK(outcome.body["error"].get<std::string>() == "write_gate_closed");
    CHECK(g_fakeCallCount == 0);
}
```

- [ ] **Step 2: Create the header declaring the interface**

Create `src/AI/StandaloneMcp/HeadlessAssistantTrigger.h`:

```cpp
#pragma once

#include "AI/StandaloneMcp/OzonePluginBackend.h"   // ToolCallOutcome
#include "AI/StandaloneMcp/TriggerManifest.h"

#include <functional>
#include <memory>
#include <string>

namespace more_phi::standalone_mcp {

// Arguments passed to a trigger invocation. Mirrors the MCP tool args.
struct HeadlessTriggerArgs
{
    std::string targetMode = "isolated";   // "isolated" | "live"
    bool allowUnsafeWrite = false;          // per-call gate (live only)
    bool applyResult = false;               // capture-only by default
    std::string inputAudioPath;             // optional analysis audio (isolated)
    uint32_t dawProcessId = 0;              // required for live
};

// Headless trigger front-end. Loads the certified trigger manifest, verifies
// the loaded DLL hash, and dispatches to an isolated (in-process) or live
// (out-of-process Frida) backend.
//
// TEST CONTRACT: when a fake trigger is installed via setFakeTriggerForTests(),
// the DLL-hash check is bypassed and the fake is called instead of resolving a
// real function pointer. This mirrors IZotopeIPCAssistant::setFakeSegmentForTests.
class HeadlessAssistantTrigger
{
public:
    HeadlessAssistantTrigger() = default;
    ~HeadlessAssistantTrigger() = default;

    // Load + validate the trigger manifest from disk. Refuses unsigned
    // manifests. Returns true on success.
    bool loadManifest(const std::string& manifestPath);

    // Isolated backend: calls the trigger in-process against a More-Phi-hosted
    // Ozone instance. No DAW. No write gate required (no production session).
    ToolCallOutcome runIsolated(const HeadlessTriggerArgs& args);

    // Live backend: spawns the Frida launcher to call the trigger inside a live
    // FL64 PID. Requires the two-part gate (env + per-call) and a certified
    // manifest, else returns write_gate_closed / trigger_not_certified.
    ToolCallOutcome runLive(const HeadlessTriggerArgs& args);

    // ── test seams ──────────────────────────────────────────────────────────
    using FakeTriggerFn = std::function<void()>;

    // Install a fake trigger; subsequent runIsolated/runLive call it instead of
    // a real function pointer, and skip the DLL-hash check.
    void setFakeTriggerForTests(FakeTriggerFn fn) { fakeTrigger = std::move(fn); }

    // Load a manifest from an in-memory JSON object (tests only).
    bool loadManifestForTests(const nlohmann::json& src);

private:
    std::optional<TriggerManifest> manifest;
    FakeTriggerFn fakeTrigger;

    ToolCallOutcome refuse(const std::string& code, const std::string& message) const;
    bool envWriteGateOpen() const;
    bool dllHashOk() const;
};

std::unique_ptr<HeadlessAssistantTrigger> createHeadlessAssistantTrigger();

} // namespace more_phi::standalone_mcp
```

- [ ] **Step 3: Verify the tests fail (unresolved `createHeadlessAssistantTrigger` etc.)**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests 2>&1 | findstr /C:"LNK2019" /C:"createHeadlessAssistantTrigger"`
Expected: FAIL — unresolved externals for `createHeadlessAssistantTrigger`, `loadManifestForTests`, `runIsolated`, `runLive`.

- [ ] **Step 4: Commit (header + tests, red on link)**

```bash
git add src/AI/StandaloneMcp/HeadlessAssistantTrigger.h tests/Unit/TestHeadlessAssistantTrigger.cpp
git commit -m "feat(headless-trigger): HeadlessAssistantTrigger interface + gate tests (red)"
```

---

## Task 5: HeadlessAssistantTrigger implementation (make Task 4 green)

**Files:**
- Create: `src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp`
- Modify: `tests/CMakeLists.txt` (add the .cpp to test sources)

- [ ] **Step 1: Add the new .cpp to the test target sources**

In `tests/CMakeLists.txt`, extend the `target_sources` block added in Task 2:

```cmake
target_sources(MorePhiMcpServerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/AI/StandaloneMcp/TriggerManifest.cpp
    ${CMAKE_SOURCE_DIR}/src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp
)
```

- [ ] **Step 2: Write the implementation**

Create `src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp`:

```cpp
#include "AI/StandaloneMcp/HeadlessAssistantTrigger.h"

#include <juce_core/juce_core.h>

#include <cstdlib>

namespace more_phi::standalone_mcp {

namespace {
constexpr const char* kWriteGateEnv = "MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE";
}

std::unique_ptr<HeadlessAssistantTrigger> createHeadlessAssistantTrigger()
{
    return std::make_unique<HeadlessAssistantTrigger>();
}

ToolCallOutcome HeadlessAssistantTrigger::refuse(const std::string& code,
                                                 const std::string& message) const
{
    return ToolCallOutcome::error(message.empty() ? code : message);
}

bool HeadlessAssistantTrigger::envWriteGateOpen() const
{
    const char* v = std::getenv(kWriteGateEnv);
    return v != nullptr && std::string{v} == "1";
}

bool HeadlessAssistantTrigger::dllHashOk() const
{
    if (!manifest)
        return false;
    if (fakeTrigger)
        return true;  // test seam: bypass hash check when a fake trigger is set
    return TriggerManifest::dllHashMatches(manifest->dllPath, manifest->dllSha256);
}

bool HeadlessAssistantTrigger::loadManifest(const std::string& manifestPath)
{
    juce::File file{juce::String{manifestPath}};
    if (!file.existsAsFile())
        return false;
    auto text = file.loadFileAsString().toStdString();
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded())
        return false;
    auto m = TriggerManifest::fromJson(parsed);
    if (!m || !m->isCertified())
        return false;
    manifest = std::move(*m);
    return true;
}

bool HeadlessAssistantTrigger::loadManifestForTests(const nlohmann::json& src)
{
    auto m = TriggerManifest::fromJson(src);
    if (!m)
        return false;
    // Tests install the fake trigger, so we accept the manifest even with a
    // placeholder-zero hash; isCertified() still requires isolationTestPassed.
    if (!m->isCertified())
        return false;
    manifest = std::move(*m);
    return true;
}

ToolCallOutcome HeadlessAssistantTrigger::runIsolated(const HeadlessTriggerArgs& args)
{
    if (!manifest || !manifest->isCertified())
        return refuse("trigger_not_certified",
                      "No certified trigger manifest is loaded. Run stages 0-2 first.");
    if (!dllHashOk())
        return refuse("dll_hash_mismatch",
                      "Loaded iZOzonePro.dll SHA-256 does not match the manifest.");

    // Isolated backend: no write gate (no production session). The fake seam is
    // exercised in tests; the real path resolves the function pointer from the
    // already-loaded DLL in the host's address space (Stage 2 wiring, Task 8).
    if (fakeTrigger)
        fakeTrigger();
    // else: real in-process resolution + call lands in Task 8 (Stage 2 harness).

    return {
        json{
            {"success", true},
            {"target_mode", "isolated"},
            {"trigger_rva", manifest->triggerRva},
            {"apply_result", args.applyResult},
        },
        false,
    };
}

ToolCallOutcome HeadlessAssistantTrigger::runLive(const HeadlessTriggerArgs& args)
{
    if (!manifest || !manifest->isCertified())
        return refuse("trigger_not_certified",
                      "No certified trigger manifest is loaded. Run stages 0-2 first.");
    if (!args.allowUnsafeWrite || !envWriteGateOpen())
        return refuse("write_gate_closed",
                      "Live trigger requires allow_unsafe_write=true AND "
                      "MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1.");
    if (!dllHashOk())
        return refuse("dll_hash_mismatch",
                      "Loaded iZOzonePro.dll SHA-256 does not match the manifest.");
    if (args.dawProcessId == 0)
        return refuse("invalid_params", "Live trigger requires daw_process_id.");

    // Fake seam first (tests). Real Frida spawn lands in Task 10 (Stage 3).
    if (fakeTrigger)
    {
        fakeTrigger();
        return {
            json{
                {"success", true},
                {"target_mode", "live"},
                {"daw_process_id", args.dawProcessId},
                {"apply_result", args.applyResult},
                {"note", "fake-trigger test path"},
            },
            false,
        };
    }

    // TODO(Task 10): spawn tools/ozone_trigger_workflow.py live mode, which
    // loads tools/ozone_trigger_frida.js against args.dawProcessId.
    return refuse("live_backend_not_wired",
                  "Live Frida backend is implemented in the Stage-3 tooling, not the C++ host.");
}

} // namespace more_phi::standalone_mcp
```

- [ ] **Step 3: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests && cd build && ctest -C Release -R "HeadlessAssistantTrigger|TriggerManifest" --output-on-failure`
Expected: PASS — all 9 tests (6 manifest + 3 trigger). Note the live-gate test sets neither the env var nor the flag, so it refuses with `write_gate_closed` as expected; `g_fakeCallCount` stays 0.

- [ ] **Step 4: Commit**

```bash
git add src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp tests/CMakeLists.txt
git commit -m "feat(headless-trigger): manifest load + isolated/live dispatch with gates (green)"
```

---

## Task 6: Wire the new tool into the MCP server

**Files:**
- Modify: `src/AI/StandaloneMcp/StandaloneMcpServer.h:3,32,50`
- Modify: `src/AI/StandaloneMcp/StandaloneMcpServer.cpp` (tool-list block ~lines 299–328; handler block ~lines 573–608)

- [ ] **Step 1: Add the include + member + constructor param to the header**

In `src/AI/StandaloneMcp/StandaloneMcpServer.h`:

Add the include after line 4 (`#include "OzonePluginBackend.h"`):
```cpp
#include "HeadlessAssistantTrigger.h"
```

Add the constructor param after the `ipcAssistant` param (line 32), keeping the default factory:
```cpp
    explicit StandaloneMcpServer(
        std::unique_ptr<OzonePluginBackend> backend = createOzonePluginBackend(),
        std::unique_ptr<IZotopeIPCDiscovery> ipcDiscovery = createIZotopeIPCDiscovery(),
        std::unique_ptr<IZotopeIPCAssistant> ipcAssistant = createIZotopeIPCAssistant(),
        std::unique_ptr<HeadlessAssistantTrigger> headlessTrigger = createHeadlessAssistantTrigger());
```

Add the member after `ipcAssistant` (line 50):
```cpp
    std::unique_ptr<HeadlessAssistantTrigger> headlessTrigger;
```

- [ ] **Step 2: Add the tool descriptor to the tool list**

In `StandaloneMcpServer.cpp`, in the function that builds the `tools` vector (the block ending at line 329 with the `ozone_run_assistant` entry), append one more descriptor before the closing `};`:

```cpp
        ,
        {
            "ozone_trigger_assistant_headless",
            "Trigger Ozone Assistant Headlessly",
            "Staged, gated trigger for the real Ozone Master Assistant via its "
            "internal trigger function. Isolated mode calls the trigger in-process "
            "on a More-Phi-hosted Ozone instance (no DAW). Live mode calls it "
            "out-of-process inside a live FL64 PID via Frida, behind the two-part "
            "write gate. Requires a certified trigger manifest whose DLL SHA-256 "
            "matches the loaded iZOzonePro.dll. Capture-only by default.",
            {
                {"type", "object"},
                {"properties", {
                    {"target_mode", {
                        {"type", "string"},
                        {"enum", {"isolated", "live"}},
                        {"default", "isolated"},
                        {"description", "isolated = More-Phi-hosted instance (no DAW); live = Frida into a live FL64 PID."}
                    }},
                    {"manifest_path", stringProperty(
                        "Path to a certified trigger manifest JSON. Falls back to "
                        "the path in IZOTOPE_TRIGGER_MANIFEST.")},
                    {"allow_unsafe_write", {
                        {"type", "boolean"},
                        {"default", false},
                        {"description", "Live mode only: must be true, with MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1."}
                    }},
                    {"apply_result", {
                        {"type", "boolean"},
                        {"default", false},
                        {"description", "Apply the Assistant's parameter decisions. Defaults to capture-only."}
                    }},
                    {"input_audio_path", stringProperty(
                        "isolated mode: audio file to feed for analysis.")},
                    {"daw_process_id", integerProperty(
                        "live mode: the FL64.exe PID to target.", 1, 0)}
                }},
                {"required", {"target_mode"}}
            },
            {{"title", "Trigger Ozone Assistant Headlessly"},
             {"destructiveHint", true},
             {"idempotentHint", false},
             {"openWorldHint", true}}
        }
```

- [ ] **Step 3: Add the handler branch to handleToolsCall**

In `StandaloneMcpServer.cpp`, in `handleToolsCall`, after the `ozone_run_assistant` branch (ends ~line 608 with the apply-result block), add:

```cpp
    else if (name == "ozone_trigger_assistant_headless")
    {
        if (args.contains("allow_unsafe_write") && !args["allow_unsafe_write"].is_boolean())
            return invalidParams(id, "allow_unsafe_write must be a boolean.");
        if (args.contains("apply_result") && !args["apply_result"].is_boolean())
            return invalidParams(id, "apply_result must be a boolean.");

        HeadlessTriggerArgs parsed;
        parsed.targetMode = optionalString(args, "target_mode").value_or("isolated");
        parsed.allowUnsafeWrite = optionalBool(args, "allow_unsafe_write", false);
        parsed.applyResult = optionalBool(args, "apply_result", false);
        parsed.inputAudioPath = optionalString(args, "input_audio_path").value_or("");
        if (auto pid = optionalSize(args, "daw_process_id"))
            parsed.dawProcessId = static_cast<uint32_t>(*pid);

        const auto manifestPath = optionalString(args, "manifest_path")
            .value_or(envOrDefault("IZOTOPE_TRIGGER_MANIFEST").toStdString());
        if (!headlessTrigger->loadManifest(manifestPath))
        {
            return JsonRpc::makeError(
                id, -32000,
                "trigger_not_certified: manifest missing, unsigned, or not found at " + manifestPath);
        }

        if (parsed.targetMode == "live")
            outcome = headlessTrigger->runLive(parsed);
        else
            outcome = headlessTrigger->runIsolated(parsed);
    }
```

- [ ] **Step 4: Update the constructor definition**

Find the `StandaloneMcpServer::StandaloneMcpServer(...)` definition in `StandaloneMcpServer.cpp` and add the `headlessTrigger` parameter + member init, matching the header signature. (Read the existing definition first to match its exact parameter names/initialization style.)

- [ ] **Step 5: Build the server target**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests 2>&1 | findstr /C:"error" /C:"LNK2019" /C:"warning.*HeadlessAssistant"`
Expected: clean build (no errors). The existing server tests still pass.

- [ ] **Step 6: Run the server test suite to confirm no regression**

Run: `cd build && ctest -C Release -R "McpServer|IZotopeIPC|Headless|TriggerManifest" --output-on-failure`
Expected: PASS — all prior tests plus the 9 new ones.

- [ ] **Step 7: Commit**

```bash
git add src/AI/StandaloneMcp/StandaloneMcpServer.h src/AI/StandaloneMcp/StandaloneMcpServer.cpp
git commit -m "feat(mcp): register ozone_trigger_assistant_headless tool + handler"
```

---

## Task 7: MCP server integration test (tool listed + dispatched)

**Files:**
- Modify: `tests/Unit/TestStandaloneMcpServer.cpp`

- [ ] **Step 1: Read the existing server test to match its request helper style**

Run: `grep -n "tools/list\|processJson\|TEST_CASE\|initialize\|bearer" tests/Unit/TestStandaloneMcpServer.cpp | head`
Use the established helper to send a `tools/list` and a `tools/call`. Match how existing tests construct the bearer-token `initialize` request.

- [ ] **Step 2: Add a test that the new tool appears in tools/list**

Append to `tests/Unit/TestStandaloneMcpServer.cpp` (adapt the server-construction + request helpers already present in that file):

```cpp
TEST_CASE("tools/list advertises ozone_trigger_assistant_headless", "[mcp][headless]")
{
    StandaloneMcpServer server;
    auto resp = server.processJson({
        {"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 1}
    });
    REQUIRE(resp.contains("result"));
    const auto& tools = resp["result"]["tools"];
    bool found = false;
    for (const auto& t : tools)
    {
        if (t["name"] == "ozone_trigger_assistant_headless")
            found = true;
    }
    REQUIRE(found);
}
```

- [ ] **Step 3: Add a test that calling it without a manifest returns trigger_not_certified**

Append:

```cpp
TEST_CASE("ozone_trigger_assistant_headless refuses without a certified manifest", "[mcp][headless]")
{
    StandaloneMcpServer server;
    auto resp = server.processJson({
        {"jsonrpc", "2.0"}, {"method", "tools/call"}, {"id", 2},
        {"params", {
            {"name", "ozone_trigger_assistant_headless"},
            {"arguments", {{"target_mode", "isolated"}, {"manifest_path", "nope.json"}}}
        }}
    });
    // The handler emits a JSON-RPC error code -32000 with trigger_not_certified text.
    REQUIRE(resp.contains("error"));
    CHECK(resp["error"]["message"].get<std::string>().find("trigger_not_certified") != std::string::npos);
}
```

- [ ] **Step 4: Build + run**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests && cd build && ctest -C Release -R "headless" --output-on-failure`
Expected: PASS — 2 new tests.

- [ ] **Step 5: Commit**

```bash
git add tests/Unit/TestStandaloneMcpServer.cpp
git commit -m "test(mcp): tools/list advertises + refuses uncertified headless trigger"
```

---

## Task 8: Isolated Stage-2 in-process function resolution (real path)

**Files:**
- Modify: `src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp` (the `else` branch of `runIsolated`)

> This task wires the *real* isolated backend. It is intentionally separated from
> Task 5 so the gate logic could ship and be tested with fakes first. It resolves
> the trigger function pointer from the already-loaded `iZOzonePro.dll` in the
> host's address space and calls it. No cross-process work.

- [ ] **Step 1: Write a failing test for the in-process resolution using a fake in-process function**

Append to `tests/Unit/TestHeadlessAssistantTrigger.cpp`. This test registers a *real exported symbol* from a tiny test-only DLL is overkill; instead we test the resolution helper against the test executable's own module, proving the address arithmetic + module-base lookup works without Ozone:

```cpp
#ifdef _WIN32
#  include <windows.h>
#endif

TEST_CASE("in-process module-base + RVA resolution is correct", "[headless][isolated][resolution]")
{
    // We can't load Ozone in CI. Instead, prove the resolution math: take the
    // base address of the current module (Windows: GetModuleHandle with NULL),
    // pick a known exported function address, and confirm base + (fn - base)
    // round-trips to the same function pointer.
#ifdef _WIN32
    HMODULE base = GetModuleHandleW(nullptr);
    REQUIRE(base != nullptr);
    auto fnAddr = reinterpret_cast<std::uintptr_t>(&fakeTriggerFn);
    auto baseAddr = reinterpret_cast<std::uintptr_t>(base);
    std::uintptr_t rva = fnAddr - baseAddr;
    REQUIRE(reinterpret_cast<void*>(baseAddr + rva) == reinterpret_cast<void*>(fnAddr));
#else
    // On non-Windows the isolated Windows-only backend is not built; skip.
    SUCCEED("isolated resolution is Windows-only");
#endif
}
```

- [ ] **Step 2: Run it to verify it passes (pure arithmetic, no impl needed yet)**

Run: `cd build && ctest -C Release -R "resolution" --output-on-failure`
Expected: PASS on Windows.

- [ ] **Step 3: Implement the real isolated backend behind a Windows guard**

In `HeadlessAssistantTrigger.cpp`, replace the `// else: real in-process resolution + call ...` comment in `runIsolated` with:

```cpp
    else
    {
#ifdef _WIN32
        HMODULE ozoneBase = GetModuleHandleW(L"iZOzonePro.dll");
        if (ozoneBase == nullptr)
            return refuse("ozone_dll_not_loaded",
                          "iZOzonePro.dll is not loaded in this process. The "
                          "isolated backend requires More-Phi to host Ozone first.");
        // The trigger function takes the Assistant state pointer in RCX
        // (resolved during Stage 1). For the certified calling convention we
        // pass a pointer resolved from the hosted instance; this pointer is
        // obtained from the OzonePluginBackend in Task 8b (host integration).
        // Until that wiring exists, refuse rather than call with a null arg.
        using TriggerFn = void (*)(void*);
        auto fn = reinterpret_cast<TriggerFn>(
            reinterpret_cast<std::uintptr_t>(ozoneBase) + manifest->triggerRva);
        // Safety: refuse if the resolved address is obviously bad (null or in
        // the first page). The DLL-hash check above already guaranteed the
        // offset is valid for this exact DLL build.
        if (fn == nullptr)
            return refuse("trigger_resolution_failed",
                          "Resolved trigger function pointer is null.");
        // Task 8b will supply the Assistant state pointer; until then we do not
        // call, to avoid passing an unvalidated argument.
        return refuse("isolated_state_pointer_not_wired",
                      "Host integration (state pointer acquisition) lands in Task 8b.");
#else
        return refuse("unsupported_platform",
                      "Isolated in-process trigger is Windows-only.");
#endif
    }
```

- [ ] **Step 4: Build + run the headless suite**

Run: `cmake --build build --config Release --target MorePhiMcpServerTests && cd build && ctest -C Release -R "Headless|TriggerManifest|resolution" --output-on-failure`
Expected: PASS — all tests still green (the real path refuses cleanly without Ozone loaded; the fake path tests still call the fake).

- [ ] **Step 5: Commit**

```bash
git add src/AI/StandaloneMcp/HeadlessAssistantTrigger.cpp tests/Unit/TestHeadlessAssistantTrigger.cpp
git commit -m "feat(headless-trigger): isolated in-process trigger resolution (Windows, guarded)"
```

> **Note on Task 8b (host integration):** acquiring the Ozone Assistant *state
> pointer* to pass in RCX depends on Stage-1 validation output (the argument
> layout). It is deliberately left as a clean refusal (`isolated_state_pointer_not_wired`)
> rather than guessed. This is task #1 in the "Deferred" section below.

---

## Task 9: Stage-1 correlation tool (click → phase transition)

**Files:**
- Create: `tools/ozone_trigger_correlate.py`

- [ ] **Step 1: Write the correlation script**

Create `tools/ozone_trigger_correlate.py`:

```python
#!/usr/bin/env python3
"""Stage 1: correlate a manual Assistant GUI click with a phase-state transition.

Reads a state-diff trace JSONL produced by tools/izotope_state_diff_trace.py and
the linked-instance parameter poll, and reports which candidate trigger function
transitioned idle -> PROCESSING_LISTENING -> ...SETTING_SIGNAL_CHAIN immediately
after the first observed parameter burst (the proxy for the click).

This is the read-only validation stage. It does NOT call any function, attach
for writes, or mutate anything. It reads saved artifacts only.

Usage:
    py tools/ozone_trigger_correlate.py \\
        --trace tools/live_captures/ipc_decode/izotope_state_diff_trace_<ts>.jsonl \\
        --params tools/live_captures/linked_instances/linked_instance_monitor_<ts>.jsonl \\
        --out tools/live_captures/static/validated_trigger.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PHASE_ORDER = [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
]


def load_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def first_param_burst_ts(param_rows: list[dict]) -> float | None:
    """Return the monotonic timestamp of the first parameter-change burst."""
    for row in param_rows:
        changes = row.get("parameter_changes") or row.get("changes")
        if changes:
            ts = row.get("elapsed_s", row.get("timestamp"))
            if ts is not None:
                return float(ts)
    return None


def correlate(trace_rows: list[dict], click_ts: float | None, window_s: float = 3.0) -> dict | None:
    """Find the trace row whose state advanced through >=2 phases within +/-window_s of click_ts."""
    if click_ts is None:
        return None
    best = None
    for row in trace_rows:
        ts = row.get("elapsed_s", row.get("timestamp"))
        if ts is None:
            continue
        if abs(float(ts) - click_ts) > window_s:
            continue
        new_state = row.get("new_state") or row.get("state_label")
        if new_state in PHASE_ORDER and (best is None or new_state > best.get("new_state", "")):
            best = row
    return best


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trace", type=Path, required=True)
    ap.add_argument("--params", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--window-s", type=float, default=3.0)
    args = ap.parse_args(argv)

    trace_rows = load_jsonl(args.trace)
    param_rows = load_jsonl(args.params)
    click_ts = first_param_burst_ts(param_rows)
    if click_ts is None:
        print("error: no parameter burst found in the poll; did you click Assistant?", file=sys.stderr)
        return 1

    hit = correlate(trace_rows, click_ts, args.window_s)
    if hit is None:
        print("error: no phase transition correlated with the first parameter burst.", file=sys.stderr)
        return 2

    candidate = {
        "schema_version": 1,
        # NOTE: dll_sha256 is a PLACEHOLDER here. The operator must paste the
        # real `sha256sum iZOzonePro.dll` before this manifest is certified.
        "dll_sha256": "PASTE_REAL_SHA256_HERE",
        "dll_path": r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll",
        "trigger_rva": hit.get("target_rva") or hit.get("rva"),
        "calling_convention": "microsoft_x64",
        "phase_state_markers": PHASE_ORDER,
        "certified_at_unix": 0,
        "certified_by": "",
        "isolation_test_passed": False,
        "correlation": {
            "click_ts": click_ts,
            "matched_state": hit.get("new_state") or hit.get("state_label"),
            "window_s": args.window_s,
            "trace": str(args.trace),
            "params": str(args.params),
        },
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(candidate, indent=2), encoding="utf-8")
    print(f"candidate trigger -> {args.out}")
    print("NEXT: paste the real DLL SHA-256 into dll_sha256, then run Stage 2.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
```

- [ ] **Step 2: Lint-check the script parses**

Run: `python -m py_compile tools/ozone_trigger_correlate.py && echo OK`
Expected: `OK`

- [ ] **Step 3: Commit**

```bash
git add tools/ozone_trigger_correlate.py
git commit -m "feat(tools): Stage-1 click→phase correlation tool (read-only)"
```

---

## Task 10: Stage-3 Frida live-call script + orchestration driver

**Files:**
- Create: `tools/ozone_trigger_frida.js`
- Create: `tools/ozone_trigger_workflow.py`

- [ ] **Step 1: Write the Frida script**

Create `tools/ozone_trigger_frida.js`:

```javascript
// Stage 3: call the certified Ozone Assistant trigger function inside FL64.
// Loaded by tools/ozone_trigger_workflow.py (live mode) against a target PID.
//
// Receives the trigger RVA + DLL base via Frida RPC (args passed from Python).
// Performs ONE function call. Does not patch code, install hooks, or loop.
// The DLL-hash + gate checks are enforced by the C++ tool / Python driver
// before this script is ever loaded.

const moduleName = 'iZOzonePro.dll';

rpc.exports = {
    // triggerRva: number, e.g. 0x1234
    // Returns { called: true, base: '0x...', fn: '0x...' } or throws.
    callTrigger: function (triggerRva) {
        const base = Module.findBaseAddress(moduleName);
        if (base === null) {
            throw new Error('iZOzonePro.dll is not loaded in the target process');
        }
        const fn = base.add(triggerRva);
        // Microsoft x64 calling convention: first integer arg in RCX.
        // The Assistant state pointer is acquired in Stage 1/8b; for the
        // capture-only path we invoke with the state pointer resolved by the
        // driver. Until that pointer is wired, we refuse to call (safety).
        // The driver only reaches this script when the manifest's
        // argument_layout is fully resolved.
        const argLayout = this.state_pointer;  // set by driver from manifest
        if (!argLayout) {
            throw new Error('state_pointer not supplied; refusing to call trigger');
        }
        const NativeFn = new NativeFunction(fn, 'void', ['pointer']);
        NativeFn(ptr(argLayout));
        return { called: true, base: base.toString(), fn: fn.toString() };
    }
};
```

- [ ] **Step 2: Write the orchestration driver**

Create `tools/ozone_trigger_workflow.py`:

```python
#!/usr/bin/env python3
"""Orchestration driver for the headless Ozone Assistant trigger workflow.

Stages:
    0  recon       — run tools/ozone_static_recon.py (no attach)
    1  correlate   — MANUAL: click Assistant once, run ozone_trigger_correlate.py
    2  isolated    — call the trigger in-process via the MCP tool (isolated mode)
    3  live        — call the trigger inside FL64 via Frida (opt-in, full gate)
    4  capture     — before/after param diff via tools/ozone_assistant_diff.py

Stages 0, 2, 3, 4 are automated. Stage 1 needs one manual GUI click.

Safety: every stage reads the trigger manifest and refuses if it is unsigned or
its DLL SHA-256 does not match the loaded iZOzonePro.dll. Live (Stage 3) also
requires MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1 and --allow-unsafe-write.

Usage:
    py tools/ozone_trigger_workflow.py recon   --dll "<path to iZOzonePro.dll>"
    py tools/ozone_trigger_workflow.py isolated --manifest docs/ozone_trigger_manifest.json
    py tools/ozone_trigger_workflow.py live     --manifest docs/ozone_trigger_manifest.json \\
        --pid 19604 --allow-unsafe-write
    py tools/ozone_trigger_workflow.py capture  --port 30001 --token "..."
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_manifest(path: Path) -> dict:
    m = json.loads(path.read_text(encoding="utf-8"))
    if not m.get("dll_sha256") or m["dll_sha256"] == "PASTE_REAL_SHA256_HERE":
        sys.exit("error: manifest dll_sha256 is a placeholder; certify it first.")
    if not m.get("isolation_test_passed"):
        sys.exit("error: manifest isolation_test_passed is false; run Stage 2 first.")
    return m


def verify_dll_hash(manifest: dict) -> None:
    dll = Path(manifest["dll_path"])
    if not dll.exists():
        sys.exit(f"error: DLL not found at {dll}")
    actual = sha256_file(dll)
    if actual != manifest["dll_sha256"]:
        sys.exit(f"error: DLL hash mismatch.\n  manifest: {manifest['dll_sha256']}\n  actual:   {actual}")


def cmd_recon(args: argparse.Namespace) -> int:
    dll = Path(args.dll)
    if not dll.exists():
        sys.exit(f"error: DLL not found at {dll}")
    out = REPO / "tools/live_captures/static/ozone_recon.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([sys.executable, str(REPO / "tools/ozone_static_recon.py"),
                           "--dll", str(dll), "--out", str(out), "--self-check"])
    print(f"recon report -> {out}")
    print("NEXT: review candidates, then run Stage 1 (correlate) manually.")
    return 0


def cmd_isolated(args: argparse.Namespace) -> int:
    manifest = load_manifest(Path(args.manifest))
    verify_dll_hash(manifest)
    print(f"[isolated] DLL hash verified: {manifest['dll_sha256']}")
    # The in-process call is performed by the MCP host. Here we just confirm the
    # manifest is valid + hash matches; the actual call goes through the C++ tool
    # via tools/call ozone_trigger_assistant_headless {target_mode: isolated}.
    print("NEXT: invoke the MCP tool:")
    print('  tools/call ozone_trigger_assistant_headless {target_mode:"isolated",'
          f' manifest_path:"{args.manifest}"}}')
    return 0


def cmd_live(args: argparse.Namespace) -> int:
    if not args.allow_unsafe_write:
        sys.exit("error: live mode requires --allow-unsafe-write")
    if os.environ.get("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE") != "1":
        sys.exit("error: live mode requires MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1")
    manifest = load_manifest(Path(args.manifest))
    verify_dll_hash(manifest)

    try:
        import frida  # type: ignore
    except ImportError:
        sys.exit("error: frida not installed (pip install frida frida-tools)")

    script_path = REPO / "tools/ozone_trigger_frida.js"
    session = frida.attach(args.pid)
    script = session.create_script(script_path.read_text(encoding="utf-8"))
    script.load()
    rva = int(manifest["trigger_rva"], 16) if isinstance(manifest["trigger_rva"], str) else manifest["trigger_rva"]
    # state_pointer wiring is the Task 8b deferred item; refuse if absent.
    state_ptr = manifest.get("assistant_state_pointer")
    if not state_ptr:
        session.detach()
        sys.exit("error: manifest has no assistant_state_pointer (Stage-1 output). Refusing to call.")
    script.exports_sync.state_pointer = state_ptr
    result = script.exports_sync.call_trigger(rva)
    print(f"[live] trigger called: {result}")
    session.detach()
    return 0


def cmd_capture(args: argparse.Namespace) -> int:
    # Delegates to the existing, verified capture/diff tool.
    before = REPO / "tools/live_captures/before_assistant.json"
    after = REPO / "tools/live_captures/after_assistant.json"
    diff = REPO / "tools/live_captures/assistant_diff.json"
    diff_tool = REPO / "tools/ozone_assistant_diff.py"
    subprocess.check_call([sys.executable, str(diff_tool), "capture",
                           "--port", str(args.port), "--token", args.token, "--out", str(before)])
    print("[capture] before snapshot taken. (Run the trigger, then re-run this stage to capture after.)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="stage", required=True)

    p_recon = sub.add_parser("recon", help="Stage 0: static recon (no attach)")
    p_recon.add_argument("--dll", default=DEFAULT_DLL)
    p_recon.set_defaults(func=cmd_recon)

    p_iso = sub.add_parser("isolated", help="Stage 2: in-process call")
    p_iso.add_argument("--manifest", required=True)
    p_iso.set_defaults(func=cmd_isolated)

    p_live = sub.add_parser("live", help="Stage 3: Frida call into FL64 (opt-in)")
    p_live.add_argument("--manifest", required=True)
    p_live.add_argument("--pid", type=int, required=True)
    p_live.add_argument("--allow-unsafe-write", action="store_true")
    p_live.set_defaults(func=cmd_live)

    p_cap = sub.add_parser("capture", help="Stage 4: before/after param diff")
    p_cap.add_argument("--port", type=int, default=30001)
    p_cap.add_argument("--token", required=True)
    p_cap.set_defaults(func=cmd_capture)

    return ap


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
```

- [ ] **Step 3: Lint-check both scripts parse**

Run: `python -m py_compile tools/ozone_trigger_frida.js 2>nul & python -m py_compile tools/ozone_trigger_workflow.py && echo OK`
(`.js` won't compile as Python — that's fine; only the `.py` must report OK.)
Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add tools/ozone_trigger_frida.js tools/ozone_trigger_workflow.py
git commit -m "feat(tools): Stage-3 Frida live-call script + orchestration driver"
```

---

## Task 11: Example manifest + documentation

**Files:**
- Create: `docs/ozone_trigger_manifest.example.json`
- Modify: `docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md` (append a section)

- [ ] **Step 1: Create the example manifest**

Create `docs/ozone_trigger_manifest.example.json`:

```json
{
  "schema_version": 1,
  "_comment": "ILLUSTRATIVE / UNSIGNED. A certified manifest is produced by Stage 1 (ozone_trigger_correlate.py) with a real DLL SHA-256 pasted in. The C++ tool refuses any manifest whose dll_sha256 is not a 64-char hex string or whose isolation_test_passed is false.",
  "dll_sha256": "PASTE_REAL_SHA256_HERE",
  "dll_path": "C:\\Program Files\\Common Files\\VST3\\iZotope\\iZOzonePro.dll",
  "trigger_rva": "0x00000000",
  "calling_convention": "microsoft_x64",
  "phase_state_markers": [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN"
  ],
  "certified_at_unix": 0,
  "certified_by": "",
  "isolation_test_passed": false,
  "assistant_state_pointer": null
}
```

- [ ] **Step 2: Append a capability section**

Append to `docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md` (after the "Operational Workflow" section):

```markdown
## Headless Assistant Trigger

When the manifest-defined IPC ring is not available (the current Ozone 11
reality), More-Phi can still trigger Ozone's real Master Assistant headlessly by
calling the Assistant's internal trigger function directly.

- **Tool:** `ozone_trigger_assistant_headless` (`target_mode: isolated` default, `live` opt-in).
- **Contract:** a certified *trigger manifest* whose `dll_sha256` matches the
  loaded `iZOzonePro.dll`. An Ozone update silently invalidates the manifest and
  the tool no-ops rather than calling a stale offset.
- **Stages:** recon (static, no attach) → validate (read-only click correlation)
  → isolated dry-run (in-process, no DAW) → live (Frida, opt-in, two-part gate)
  → capture (before/after param diff).
- **Default:** capture-only. Applying the Assistant's decisions is a separate,
  transactional call.

See `docs/superpowers/specs/2026-06-20-headless-ozone-assistant-design.md` for
the full design and `tools/ozone_trigger_workflow.py` for the stage driver.
```

- [ ] **Step 3: Commit**

```bash
git add docs/ozone_trigger_manifest.example.json docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md
git commit -m "docs(headless-trigger): example manifest + capability section"
```

---

## Task 12: Full build + full test gate

- [ ] **Step 1: Configure + build everything**

Run:
```bash
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release
```
Expected: clean build, no errors.

- [ ] **Step 2: Run the full test suite**

Run:
```bash
cd build && ctest -C Release --output-on-failure --parallel 4
```
Expected: all tests PASS, including the new `TriggerManifest`, `HeadlessAssistantTrigger`, `resolution`, and `headless` MCP tests. No regressions.

- [ ] **Step 3: Confirm the new tool is advertised end-to-end**

Run the standalone MCP server and list tools:
```bash
build\MorePhiStandaloneMcp.exe 2>nul
```
(pipe a `{"jsonrpc":"2.0","method":"tools/list","id":1}` line; confirm `ozone_trigger_assistant_headless` appears.) If the executable name differs, check `CMakeLists.txt` for the standalone MCP target name.

- [ ] **Step 4: Final commit if any formatting fixes were needed**

```bash
git add -A
git commit -m "chore(headless-trigger): full build + test gate green" || echo "nothing to commit"
```

---

## Deferred (require Stage-1 live output; cannot be pre-coded)

These are intentionally *not* tasks above because they depend on data only a real
Stage-1 correlation run produces. They are the next actions *after* this plan
ships and the operator runs Stage 1:

1. **Task 8b — Assistant state-pointer acquisition.** The `isolated_state_pointer_not_wired`
   refusal in `runIsolated` and the `assistant_state_pointer` check in the Frida
   script both wait on the argument-layout the Stage-1 trace reveals. Once
   Stage 1 names the RCX state-pointer source (e.g. a hosted-instance field),
   wire `OzonePluginBackend` to expose it and thread it into the trigger args.
2. **Calling-convention arg count.** `microsoft_x64` is the only accepted value
   today; if Stage 1 shows extra args (e.g. an analysis-duration in RDX), extend
   `TriggerManifest::argument_layout` and the `NativeFunction` signature together.
3. **Anti-tamper observation.** If a live Stage-3 call trips an Ozone integrity
   heuristic, the response is an Ozone crash/log — by design we do not probe or
   bypass it. Record it in the live-findings doc as evidence, do not code around it.

---

## Self-review notes (completed by plan author)

- **Spec coverage:** Section 1 (one tool, two backends, manifest contract) → Tasks 4–7. Section 2 (five stages) → Tasks 8–10 (isolated/live backends) + Task 9 (Stage 1) + Task 10 (Stages 0/3/4 driver). Section 3 (safety: hash pin, auth, gates, capture-only) → Tasks 2–5 (hash + gates) + Task 10 (env gate). Section 4 (file layout, three-tier tests) → all tasks. The `isolated_state_pointer_not_wired` refusal is an explicit, honest gap, not a hidden TODO.
- **Placeholder scan:** No "TBD"/"implement later" in actionable steps. The two `TODO(...)` / `_state_pointer_not_wired` markers point to named deferred items with their own section, and the Frida script's refusal path is real (throws, not silently skipped).
- **Type consistency:** `HeadlessTriggerArgs`, `createHeadlessAssistantTrigger`, `loadManifestForTests`, `setFakeTriggerForTests`, `runIsolated`, `runLive` are used identically across the header, the impl, and the tests. `TriggerManifest::fromJson/isCertified/dllHashMatches` signatures match between header, impl, and tests. `ToolCallOutcome::error` matches `OzonePluginBackend.h:24`.
- **Known limitation surfaced honestly:** this plan delivers the *infrastructure* (manifest, gates, dispatch, tool registration, orchestration, fake-tested). The actual *function call* with a real argument waits on Stage-1 output (Deferred §1). Shipping the infra first is correct: it lets Stage 0–1 run, and the call lands as a small follow-up once the state pointer is known.
