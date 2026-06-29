# Z.AI LLM Provider Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Z.AI (Zhipu AI / GLM Coding Plan) as a first-class, selectable LLM provider that drives the agent layer (`RestLlmClient`/`ConductorAgent`) and the chat panel (`LLMChatClient`) over its OpenAI-compatible `/v4` endpoint.

**Architecture:** Pure configuration-table change. Add one `LLMProviderId::ZAI` enum value + one row in the provider-definitions table (`{ "zai", "Z.AI", "https://api.z.ai/api/coding/paas/v4", false }`) + the index-shift in `llmProviderIndex()`. Z.AI rides the existing generic OpenAI-style branch in all three request builders (`RestLlmClient`, `LLMChatClient`, `LLMConnectionValidator`) — no new request/response code paths. UI combobox, settings store, and processor wiring are definition-driven and pick it up automatically.

**Tech Stack:** C++20, JUCE 8, nlohmann/json, Catch2 v3, CMake. Windows/MSVC build via `build-ninja.bat` (bundled Ninja). All tests in the single `MorePhiTests` executable, discovered via `catch_discover_tests`; run subsets with `ctest -R "<Catch2 test name>"`.

**Spec:** `docs/superpowers/specs/2026-06-29-zai-provider-design.md`

---

## File Structure

**Files modified (3):**
- `src/AI/LLMSettings.h` — add `ZAI` to the `LLMProviderId` enum (before `OpenAICompatible`); bump `llmProviderCount` 7 → 8.
- `src/AI/LLMSettings.cpp` — add the ZAI definition row; update the `llmProviderIndex()` switch (`ZAI`=6, `OpenAICompatible` shifts 6→7).
- `tests/Unit/TestLLMSettings.cpp` — update the count assertion (7→8), add ZAI at index 6, shift `OpenAICompatible` assertions to index 7, add the ZAI fixed-base-URL assertion.

**Files extended (1):**
- `tests/Unit/TestRestLlmClientHardening.cpp` — add a `CapturingHttpClient` test double + two ZAI `TEST_CASE`s (behaviour + request-shape endpoint/auth pinning).

**Files NOT modified (confirmed):** `RestLlmClient.cpp`, `LLMChatClient.cpp`, `LLMConnectionValidator.cpp`, `LLMSettingsDialog.cpp`, `LLMSettingsStore.cpp`, `PluginProcessor.cpp` — all are definition-driven and need no change (verified in the spec §5.3/5.4).

---

## Task 1: Add the ZAI enum value and bump the provider count

**Files:**
- Modify: `src/AI/LLMSettings.h` (the `LLMProviderId` enum, ~line 12-21; the `llmProviderCount` constant, ~line 83)

This task adds the enum slot and count only. The definition row and index come in later tasks. Building stays green because the enum has no consumers that switch exhaustively yet — `llmProviderIndex()` is updated in Task 3 before any test exercises the new value.

- [ ] **Step 1: Add `ZAI` to the enum**

In `src/AI/LLMSettings.h`, change:

```cpp
enum class LLMProviderId
{
    NVIDIA,
    DeepSeek,
    OpenAI,
    Anthropic,
    OpenRouter,
    Gemini,
    OpenAICompatible
};
```

to:

```cpp
enum class LLMProviderId
{
    NVIDIA,
    DeepSeek,
    OpenAI,
    Anthropic,
    OpenRouter,
    Gemini,
    ZAI,
    OpenAICompatible
};
```

- [ ] **Step 2: Bump the provider count**

In the same file, change:

```cpp
constexpr std::size_t llmProviderCount = 7;
```

to:

```cpp
constexpr std::size_t llmProviderCount = 8;
```

- [ ] **Step 3: Verify it compiles**

Run:
```bash
build-ninja.bat target MorePhi
```
Expected: builds the SharedCode lib without error. (The new enum value is declared but not yet referenced; the definitions array still has 7 entries, which is fine — `llmProviderCount` only sizes the `std::array<LLMProviderSettings, llmProviderCount>` storage in `LLMSettings`, not the definitions array. The definitions array is sized by its initializer, so adding the count mismatch is harmless until Task 2.)

- [ ] **Step 4: Commit**

```bash
git add src/AI/LLMSettings.h
git commit -m "feat(llm): add LLMProviderId::ZAI enum slot (count 7→8)"
```

---

## Task 2: Add the ZAI definition row

**Files:**
- Modify: `src/AI/LLMSettings.cpp` (the `definitions` array, ~line 7-15)

- [ ] **Step 1: Add the definition row before `OpenAICompatible`**

In `src/AI/LLMSettings.cpp`, change:

```cpp
const std::array<LLMProviderDefinition, llmProviderCount> definitions {{
    { LLMProviderId::NVIDIA, "nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1", false },
    { LLMProviderId::DeepSeek, "deepseek", "DeepSeek", "https://api.deepseek.com/v1", false },
    { LLMProviderId::OpenAI, "openai", "OpenAI", "https://api.openai.com/v1", false },
    { LLMProviderId::Anthropic, "anthropic", "Anthropic", "https://api.anthropic.com", false },
    { LLMProviderId::OpenRouter, "openrouter", "OpenRouter", "https://openrouter.ai/api/v1", false },
    { LLMProviderId::Gemini, "gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta", false },
    { LLMProviderId::OpenAICompatible, "openai_compatible", "OpenAI Compatible", {}, true },
}};
```

to:

```cpp
const std::array<LLMProviderDefinition, llmProviderCount> definitions {{
    { LLMProviderId::NVIDIA, "nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1", false },
    { LLMProviderId::DeepSeek, "deepseek", "DeepSeek", "https://api.deepseek.com/v1", false },
    { LLMProviderId::OpenAI, "openai", "OpenAI", "https://api.openai.com/v1", false },
    { LLMProviderId::Anthropic, "anthropic", "Anthropic", "https://api.anthropic.com", false },
    { LLMProviderId::OpenRouter, "openrouter", "OpenRouter", "https://openrouter.ai/api/v1", false },
    { LLMProviderId::Gemini, "gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta", false },
    { LLMProviderId::ZAI, "zai", "Z.AI", "https://api.z.ai/api/coding/paas/v4", false },
    { LLMProviderId::OpenAICompatible, "openai_compatible", "OpenAI Compatible", {}, true },
}};
```

The `std::array` template arg is `llmProviderCount` (now 8) and the initializer now has 8 entries — they match.

- [ ] **Step 2: Verify it compiles**

Run:
```bash
build-ninja.bat target MorePhi
```
Expected: builds without error.

- [ ] **Step 3: Commit**

```bash
git add src/AI/LLMSettings.cpp
git commit -m "feat(llm): add Z.AI provider definition (fixed /v4 base URL)"
```

---

## Task 3: Update the `llmProviderIndex()` switch

**Files:**
- Modify: `src/AI/LLMSettings.cpp` (the `llmProviderIndex` function, ~line 40-55)

`llmProviderIndex()` must return the array position for each enum value. With ZAI inserted at array position 6, `OpenAICompatible` moves to position 7.

- [ ] **Step 1: Add the ZAI case and shift OpenAICompatible**

In `src/AI/LLMSettings.cpp`, change:

```cpp
std::size_t llmProviderIndex(LLMProviderId id) noexcept
{
    switch (id)
    {
        case LLMProviderId::NVIDIA: return 0;
        case LLMProviderId::DeepSeek: return 1;
        case LLMProviderId::OpenAI: return 2;
        case LLMProviderId::Anthropic: return 3;
        case LLMProviderId::OpenRouter: return 4;
        case LLMProviderId::Gemini: return 5;
        case LLMProviderId::OpenAICompatible: return 6;
    }

    jassertfalse;
    return 0;
}
```

to:

```cpp
std::size_t llmProviderIndex(LLMProviderId id) noexcept
{
    switch (id)
    {
        case LLMProviderId::NVIDIA: return 0;
        case LLMProviderId::DeepSeek: return 1;
        case LLMProviderId::OpenAI: return 2;
        case LLMProviderId::Anthropic: return 3;
        case LLMProviderId::OpenRouter: return 4;
        case LLMProviderId::Gemini: return 5;
        case LLMProviderId::ZAI: return 6;
        case LLMProviderId::OpenAICompatible: return 7;
    }

    jassertfalse;
    return 0;
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```bash
build-ninja.bat target MorePhi
```
Expected: builds without error. The switch is now exhaustive over all 8 enum values (MSVC may emit a non-fatal warning about the `return 0` after the switch being unreachable — that is pre-existing behaviour for every other fully-covered switch in this file and is fine).

- [ ] **Step 3: Commit**

```bash
git add src/AI/LLMSettings.cpp
git commit -m "feat(llm): map LLMProviderId::ZAI to index 6, shift OpenAICompatible to 7"
```

---

## Task 4: Update `TestLLMSettings.cpp` to assert the new provider set

**Files:**
- Modify: `tests/Unit/TestLLMSettings.cpp` (the first two `TEST_CASE`s, lines 7-50)

This is the TDD step that locks in the table change. Update the existing assertions to expect 8 providers with ZAI at index 6 and `OpenAICompatible` at index 7, plus the ZAI fixed-base-URL check.

- [ ] **Step 1: Update the "exactly the approved providers" test**

In `tests/Unit/TestLLMSettings.cpp`, change:

```cpp
TEST_CASE("LLM provider definitions are exactly the approved providers", "[unit][ai][llm]")
{
    const auto& providers = getLLMProviderDefinitions();

    REQUIRE(providers.size() == 7);
    CHECK(providers[0].id == LLMProviderId::NVIDIA);
    CHECK(providers[0].displayName == "NVIDIA");
    CHECK(providers[1].id == LLMProviderId::DeepSeek);
    CHECK(providers[1].displayName == "DeepSeek");
    CHECK(providers[2].id == LLMProviderId::OpenAI);
    CHECK(providers[2].displayName == "OpenAI");
    CHECK(providers[3].id == LLMProviderId::Anthropic);
    CHECK(providers[3].displayName == "Anthropic");
    CHECK(providers[4].id == LLMProviderId::OpenRouter);
    CHECK(providers[4].displayName == "OpenRouter");
    CHECK(providers[5].id == LLMProviderId::Gemini);
    CHECK(providers[5].displayName == "Google Gemini");
    CHECK(providers[6].id == LLMProviderId::OpenAICompatible);
    CHECK(providers[6].displayName == "OpenAI Compatible");
}
```

to:

```cpp
TEST_CASE("LLM provider definitions are exactly the approved providers", "[unit][ai][llm]")
{
    const auto& providers = getLLMProviderDefinitions();

    REQUIRE(providers.size() == 8);
    CHECK(providers[0].id == LLMProviderId::NVIDIA);
    CHECK(providers[0].displayName == "NVIDIA");
    CHECK(providers[1].id == LLMProviderId::DeepSeek);
    CHECK(providers[1].displayName == "DeepSeek");
    CHECK(providers[2].id == LLMProviderId::OpenAI);
    CHECK(providers[2].displayName == "OpenAI");
    CHECK(providers[3].id == LLMProviderId::Anthropic);
    CHECK(providers[3].displayName == "Anthropic");
    CHECK(providers[4].id == LLMProviderId::OpenRouter);
    CHECK(providers[4].displayName == "OpenRouter");
    CHECK(providers[5].id == LLMProviderId::Gemini);
    CHECK(providers[5].displayName == "Google Gemini");
    CHECK(providers[6].id == LLMProviderId::ZAI);
    CHECK(providers[6].displayName == "Z.AI");
    CHECK(providers[7].id == LLMProviderId::OpenAICompatible);
    CHECK(providers[7].displayName == "OpenAI Compatible");
}
```

- [ ] **Step 2: Add the ZAI fixed-base-URL assertion**

In the same file, in the "Known providers have fixed base URLs and OpenAI Compatible is editable" `TEST_CASE`, the `for` loop already covers ZAI correctly (ZAI is not `OpenAICompatible`, so it asserts `customBaseUrlAllowed == false` and the URL starts with `https://`). Add the explicit ZAI URL check after the Gemini line. Change:

```cpp
    CHECK(getLLMProviderDefinition(LLMProviderId::NVIDIA).fixedBaseUrl == "https://integrate.api.nvidia.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::DeepSeek).fixedBaseUrl == "https://api.deepseek.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenAI).fixedBaseUrl == "https://api.openai.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::Anthropic).fixedBaseUrl == "https://api.anthropic.com");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenRouter).fixedBaseUrl == "https://openrouter.ai/api/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::Gemini).fixedBaseUrl == "https://generativelanguage.googleapis.com/v1beta");
}
```

to:

```cpp
    CHECK(getLLMProviderDefinition(LLMProviderId::NVIDIA).fixedBaseUrl == "https://integrate.api.nvidia.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::DeepSeek).fixedBaseUrl == "https://api.deepseek.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenAI).fixedBaseUrl == "https://api.openai.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::Anthropic).fixedBaseUrl == "https://api.anthropic.com");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenRouter).fixedBaseUrl == "https://openrouter.ai/api/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::Gemini).fixedBaseUrl == "https://generativelanguage.googleapis.com/v1beta");
    CHECK(getLLMProviderDefinition(LLMProviderId::ZAI).fixedBaseUrl == "https://api.z.ai/api/coding/paas/v4");
}
```

- [ ] **Step 3: Build the test target**

Run:
```bash
build-ninja.bat target MorePhiTests
```
Expected: builds without error.

- [ ] **Step 4: Run the LLMSettings tests and verify they pass**

Run:
```bash
build-ninja.bat testonly -R "LLM" --output-on-failure
```
Expected: PASS. The `ctest -R "LLM"` filter matches Catch2 test names containing "LLM" (e.g. "LLM provider definitions are exactly the approved providers", "Known providers have fixed base URLs..."). All should pass — the table now matches the assertions.

- [ ] **Step 5: Commit**

```bash
git add tests/Unit/TestLLMSettings.cpp
git commit -m "test(llm): assert Z.AI provider in definitions table (index 6, /v4 base URL)"
```

---

## Task 5: Extend the existing `CapturingHttpClient` and add the ZAI behaviour test

**Files:**
- Modify: `tests/Unit/TestRestLlmClientHardening.cpp` (the existing `CapturingHttpClient` class near end of file; add `zaiSettings()` helper; add new `TEST_CASE`s at end of file)

> **IMPORTANT — pre-existing `CapturingHttpClient`:** This file already contains a `CapturingHttpClient` class (added by the uncommitted "transport-timeout" audit work). That class captures `timeoutMs` only:
> ```cpp
> class CapturingHttpClient final : public ILLMHttpClient
> {
> public:
>     LLMHttpResponse execute(const LLMHttpRequest& request) override
>     {
>         lastTimeoutMs = request.timeoutMs;
>         ++callCount;
>         return { 200, openAiSuccessBody("ok", 1).toStdString(), "" };
>     }
>     std::atomic<int> lastTimeoutMs{ 0 };
>     std::atomic<int> callCount{ 0 };
> };
> ```
> Do NOT add a second `CapturingHttpClient` — that would be a duplicate-definition compile error. Instead, **extend the existing one** to also capture `url` + `extraHeaders` and to serve scripted responses, so the existing transport tests still pass and the new ZAI tests can inspect the request shape.

This task extends the existing capturing fake (the existing `ScriptedHttpClient` discards the request, and the existing `CapturingHttpClient` only captures `timeoutMs`) so it can also observe the built URL/headers, then adds a behaviour test proving ZAI parses an OpenAI-shape response through `RestLlmClient`.

- [ ] **Step 1: Extend the existing `CapturingHttpClient` to also capture URL + headers and serve scripted responses**

In `tests/Unit/TestRestLlmClientHardening.cpp`, locate the existing `CapturingHttpClient` (near the end of the file, under the `// ── AUDIT-FIX (transport-timeout, 2026-06-29)` comment). Change it from:

```cpp
// Capturing variant of the fake client: records the timeoutMs of the last
// request so the per-provider selection is observable without a real network.
class CapturingHttpClient final : public ILLMHttpClient
{
public:
    LLMHttpResponse execute(const LLMHttpRequest& request) override
    {
        lastTimeoutMs = request.timeoutMs;
        ++callCount;
        // Return a minimal 200 so complete() succeeds and doesn't retry.
        return { 200, openAiSuccessBody("ok", 1).toStdString(), "" };
    }

    std::atomic<int> lastTimeoutMs{ 0 };
    std::atomic<int> callCount{ 0 };
};
```

to:

```cpp
// Capturing variant of the fake client: records the timeoutMs, URL, and
// headers of the last request so the per-provider selection and routing
// are observable without a real network. Serves a scripted response list
// (falling back to a minimal 200 so complete() succeeds and doesn't retry
// when no script is set — preserves the original transport-test behaviour).
class CapturingHttpClient final : public ILLMHttpClient
{
public:
    LLMHttpResponse execute(const LLMHttpRequest& request) override
    {
        lastTimeoutMs = request.timeoutMs;
        lastUrl = request.url;
        lastHeaders = request.extraHeaders;
        ++callCount;
        const auto idx = static_cast<size_t>(callCount - 1);
        if (idx < responses.size())
            return responses[idx];
        // Default to a minimal 200 so complete() succeeds and doesn't retry
        // (preserves the existing transport-timeout tests' expectations).
        return { 200, openAiSuccessBody("ok", 1).toStdString(), "" };
    }

    std::vector<LLMHttpResponse> responses;
    std::atomic<int> lastTimeoutMs{ 0 };
    std::atomic<int> callCount{ 0 };
    juce::String lastUrl;
    juce::String lastHeaders;
};
```

Notes:
- The two existing transport tests set `http->callCount`/`lastTimeoutMs` directly and rely on the default 200 fallback. That fallback is preserved (the `responses` vector is empty for them), so they keep passing unchanged.
- `lastTimeoutMs` and `callCount` remain `std::atomic<int>` (unchanged); `lastUrl`/`lastHeaders` are plain `juce::String` (only written/read on the test thread).

- [ ] **Step 2: Add a `zaiSettings()` helper after `geminiSettings()`**

The Gemini settings helper `geminiSettings()` is defined at file scope (line ~231, after the anonymous namespace closes at line 87) — it sits between the OpenAI-shape `TEST_CASE` block and the Gemini `TEST_CASE` block, under the `// ── Gemini provider coverage ──` comment. Add a ZAI sibling immediately after `geminiSettings()` (which ends at line ~237):

```cpp
LLMProviderSettings geminiSettings()
{
    LLMProviderSettings ps;
    ps.apiKey = "AIza-test-key";
    ps.selectedModel = "gemini-2.0-flash";
    return ps;
}

LLMProviderSettings zaiSettings()
{
    LLMProviderSettings ps;
    ps.apiKey = "zai-test-key";
    ps.selectedModel = "glm-5.2";
    return ps;
}
```

Place `zaiSettings()` at file scope (not inside the anonymous namespace), matching where `geminiSettings()` itself lives.

- [ ] **Step 3: Add the ZAI behaviour `TEST_CASE` at the end of the file**

Append at the very end of `tests/Unit/TestRestLlmClientHardening.cpp` (after the last existing `TEST_CASE`, at file scope):

```cpp
// ── Z.AI provider coverage ─────────────────────────────────────────────────
// Z.AI (Zhipu GLM Coding Plan) exposes an OpenAI-compatible /v4 endpoint and
// rides the generic OpenAI-style branch in RestLlmClient::buildRequest_. These
// tests pin that routing and the OpenAI response parsing.

TEST_CASE("RestLlmClient parses Z.AI OpenAI-shape response + providerName", "[agents][llm][zai]")
{
    auto http = std::make_shared<CapturingHttpClient>();
    http->responses = {
        { 200, openAiSuccessBody(R"({"steps":[]})", 12).toStdString(), "" }
    };

    RestLlmClient client{ LLMProviderId::ZAI, zaiSettings(), http };
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(resp.tokensUsed == 12);
    REQUIRE(resp.content.isNotEmpty());
    // No valid steps → no toolCalls promoted (steps array is empty), matching
    // the documented safe behaviour; ConductorAgent falls back to deterministic.
    REQUIRE_FALSE(resp.toolCalls.is_array() && resp.toolCalls.size() > 0);
    REQUIRE(client.providerName() == "rest-Z.AI");
}
```

- [ ] **Step 4: Build the test target**

Run:
```bash
build-ninja.bat target MorePhiTests
```
Expected: builds without error.

- [ ] **Step 5: Run the ZAI behaviour test AND the pre-existing transport tests, verify all pass**

Run:
```bash
build-ninja.bat testonly -R "\[zai\]|\[transport\]" --output-on-failure
```
Expected: PASS for all. This runs the new ZAI behaviour test plus the two pre-existing transport-timeout tests — confirming the `CapturingHttpClient` extension did not regress the transport contract. (If the filter is too narrow on your ctest, broaden to `-R "RestLlmClient"`.)

- [ ] **Step 6: Commit**

```bash
git add tests/Unit/TestRestLlmClientHardening.cpp
git commit -m "test(llm): Z.AI RestLlmClient behaviour; extend CapturingHttpClient to capture URL+headers"
```

---

## Task 6: Add the ZAI request-shape test (endpoint + auth pinning)

**Files:**
- Modify: `tests/Unit/TestRestLlmClientHardening.cpp` (append one more `TEST_CASE`)

This test pins that ZAI is routed to the `/v4/chat/completions` endpoint with `Authorization: Bearer` — guarding against a future refactor accidentally sending ZAI down the Anthropic (`x-api-key`) or Gemini (`x-goog-api-key`) branch, and pinning the `/v4` base URL.

- [ ] **Step 1: Add the request-shape `TEST_CASE` at the end of the file**

Append after the behaviour test from Task 5:

```cpp
TEST_CASE("RestLlmClient routes Z.AI to /v4/chat/completions with Bearer auth", "[agents][llm][zai]")
{
    auto http = std::make_shared<CapturingHttpClient>();
    http->responses = {
        { 200, openAiSuccessBody("ok", 5).toStdString(), "" }
    };

    RestLlmClient client{ LLMProviderId::ZAI, zaiSettings(), http };
    client.complete(sampleRequest());

    REQUIRE(http->callCount == 1);
    // /v4 base URL + /chat/completions path (NOT /v1, NOT an Anthropic/Gemini path).
    REQUIRE(http->lastUrl.endsWith("/v4/chat/completions"));
    // Bearer auth (OpenAI-style), not x-api-key (Anthropic) or x-goog-api-key (Gemini).
    REQUIRE(http->lastHeaders.contains("Authorization: Bearer"));
    REQUIRE_FALSE(http->lastHeaders.contains("x-api-key"));
    REQUIRE_FALSE(http->lastHeaders.contains("x-goog-api-key"));
}
```

- [ ] **Step 2: Build the test target**

Run:
```bash
build-ninja.bat target MorePhiTests
```
Expected: builds without error.

- [ ] **Step 3: Run the ZAI request-shape test and verify it passes**

Run:
```bash
build-ninja.bat testonly -R "routes Z.AI" --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestRestLlmClientHardening.cpp
git commit -m "test(llm): pin Z.AI /v4 endpoint + Bearer auth routing"
```

---

## Task 7: Full verification — run the complete LLM test surface

**Files:** none (verification only)

- [ ] **Step 1: Run the full LLM-related test set**

Run:
```bash
build-ninja.bat testonly -R "LLM|RestLlmClient|llm" --output-on-failure
```
Expected: all PASS. This sweeps `TestLLMSettings`, `TestLLMSettingsStore`, `TestLLMSettingsDialog`, `TestLLMConnectionValidator`, `TestLLMChatClient`, `TestRestLlmClientHardening`. The new ZAI tests are included via the `[zai]`/`llm` tag match.

- [ ] **Step 2: Build the full plugin (smoke check that no consumer broke)**

Run:
```bash
build-ninja.bat build
```
Expected: builds the DAW-loadable VST3 without error. This confirms the definition-driven consumers (`LLMSettingsDialog`, `LLMSettingsStore`, `LLMConnectionValidator`, `LLMChatClient`, `PluginProcessor`) all compile against the new 8-provider table.

- [ ] **Step 3: If everything is green, no further commit needed**

Tasks 1-6 each committed their own changes. This task is verification-only. If a fix was needed during verification, commit it with a descriptive message; otherwise there is nothing to commit here.

---

## Done criteria

- `LLMProviderId::ZAI` exists; `llmProviderCount == 8`.
- `getLLMProviderDefinitions()` contains the ZAI row at index 6 with `fixedBaseUrl == "https://api.z.ai/api/coding/paas/v4"` and `customBaseUrlAllowed == false`.
- `llmProviderIndex(ZAI) == 6`, `llmProviderIndex(OpenAICompatible) == 7`.
- `TestLLMSettings` passes with the 8-provider assertions.
- `TestRestLlmClientHardening` has two new ZAI tests (behaviour + request-shape) that pass.
- Full plugin (`MorePhi` VST3) and test executable (`MorePhiTests`) build clean.
- No source changes outside `src/AI/LLMSettings.{h,cpp}` and the two test files.
