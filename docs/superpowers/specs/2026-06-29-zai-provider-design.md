# Z.AI LLM Provider — Design

**Date:** 2026-06-29
**Status:** Approved (Approach A)
**Spec author:** ZCode
**Source docs:** https://docs.z.ai/devpack/tool/others

## 1. Goal

Add **Z.AI** (Zhipu AI / GLM Coding Plan) as a first-class, selectable LLM provider in
More-Phi's agent layer (`RestLlmClient`, used by `ConductorAgent`) and chat panel
(`LLMChatClient`), so a user with a Z.AI API key can drive the conductor goal-decomposition
and the AI chat against GLM models.

## 2. Provider facts (from Z.AI docs)

Z.AI exposes an **OpenAI-compatible** chat-completions surface:

| Property | Value |
|----------|-------|
| Base URL | `https://api.z.ai/api/coding/paas/v4` (note `/v4`, not `/v1`) |
| Chat path | `/chat/completions` (appended to base) → `…/v4/chat/completions` |
| Models path | `/models` (appended to base) → `…/v4/models` |
| Auth scheme | `Authorization: Bearer <ZAI_API_KEY>` |
| Request body | OpenAI shape (`{model, max_tokens, messages:[{role,content}]}`) |
| Response body | OpenAI shape (`{choices:[{message:{content}}], usage:{total_tokens}}`) |
| Example model | `glm-5.2` |

Because the body shape, auth header, and response parsing are all the standard OpenAI form,
Z.AI fits cleanly into the existing generic OpenAI-style code path. No new request/response
parsing logic is required.

## 3. Approach (chosen: A — fixed-URL first-class provider)

Three approaches were considered:

- **A. ZAI as a fixed-URL first-class provider** *(chosen)* — one new enum value + one row
  in the provider-definitions table; `customBaseUrlAllowed = false`. Mirrors NVIDIA /
  DeepSeek / OpenRouter. Users paste a key + pick a model.
- **B. Reuse the existing "OpenAI Compatible" provider** — zero code, but poor UX (user must
  know and type the `/v4` URL) and no first-class identity in the toolbar/agent ledger.
- **C. Fixed-URL provider *with* editable base URL** — most flexible, but only
  `OpenAICompatible` currently allows custom URLs and the UI/validation has special logic
  for it; enabling it for ZAI adds branching for no real benefit.

**Why A:** it is the pattern every other real provider follows, gives Z.AI a clean identity
(toolbar display, `providerName` = `rest-Z.AI`, ledger attribution), and reduces the change
to a pure configuration-table edit. B hurts UX; C adds complexity for no gain.

## 4. Architecture

The provider layer is already table-driven. Every consumer iterates
`getLLMProviderDefinitions()`, so adding a row propagates automatically to:

- `RestLlmClient::buildRequest_` (agent / conductor path)
- `LLMChatClient::buildHttpRequest` (chat panel path)
- `LLMConnectionValidator` (fetch-models + test-prompt)
- `LLMSettingsDialog` (provider combobox)
- `LLMSettingsStore` (serialization)
- `PluginProcessor` (`RestLlmClient::isConfigured` + constructor are provider-agnostic)

Z.AI rides the **generic OpenAI-style branch** in all three request builders (the `else`
after the Gemini/Anthropic special cases): path `/chat/completions`, `Authorization: Bearer`,
OpenAI request/response JSON. No provider-specific branch is added.

### 4.1 The `/v4` path question

The base URL ends in `/v4`. The existing builders append a bare `/chat/completions`
(non-Gemini, non-Anthropic) or `/models` (validator fetch). For Z.AI this yields:

- Chat: `https://api.z.ai/api/coding/paas/v4` + `/chat/completions` = correct
- Models: `https://api.z.ai/api/coding/paas/v4` + `/models` = correct

No path special-casing is needed. The `/v4` lives in the fixed base URL, exactly as `/v1`
lives in OpenAI's / DeepSeek's / OpenRouter's fixed base URLs today.

## 5. Detailed changes

### 5.1 `src/AI/LLMSettings.h`

Add `ZAI` to `LLMProviderId`, inserted before `OpenAICompatible` (which remains last as the
generic catch-all):

```cpp
enum class LLMProviderId
{
    NVIDIA,
    DeepSeek,
    OpenAI,
    Anthropic,
    OpenRouter,
    Gemini,
    ZAI,                 // <-- new
    OpenAICompatible
};
```

Bump the count:

```cpp
constexpr std::size_t llmProviderCount = 8;   // was 7
```

### 5.2 `src/AI/LLMSettings.cpp`

Add the definition row (before `OpenAICompatible`):

```cpp
{ LLMProviderId::ZAI, "zai", "Z.AI", "https://api.z.ai/api/coding/paas/v4", false },
```

- `storageKey = "zai"` — lowercase, matches the existing convention; this is the on-disk key
  used by `LLMSettingsStore`, so it must be stable and unique.
- `displayName = "Z.AI"`
- `fixedBaseUrl = "https://api.z.ai/api/coding/paas/v4"`
- `customBaseUrlAllowed = false` (Approach A)

Update `llmProviderIndex()`:

```cpp
case LLMProviderId::ZAI: return 6;
case LLMProviderId::OpenAICompatible: return 7;   // was 6
```

No other functions in this file need changes — `getBaseUrl`, `activateProviderIfValidated`,
`getToolbarStatus`, etc. are all index-driven via the definitions table.

### 5.3 No changes required in the request builders

Confirmed by reading the source:

- **`RestLlmClient::buildRequest_`** (`src/AI/Agents/Llm/RestLlmClient.cpp:255`): ZAI is not
  Gemini or Anthropic, so it takes the generic `else` branch → path `/chat/completions`,
  OpenAI body, Bearer auth. `baseUrlFor`/`authHeadersFor` are definition-driven.
  `timeoutMsForProvider` returns `kTimeoutMsDefault` (60s) for ZAI — no cold-start behavior
  is documented for Z.AI, so no NVIDIA-style override.
- **`LLMChatClient::buildHttpRequest`** (`src/AI/LLMChatClient.cpp:683`): identical generic
  branch. Timeout `(id == NVIDIA) ? kTimeoutMsNvidia : kTimeoutMs` → ZAI gets `kTimeoutMs`
  (60s).
- **`LLMConnectionValidator`** (`src/AI/LLMConnectionValidator.cpp`): `buildFetchModelsRequestForTest`
  uses path `/models` (non-Anthropic) → `…/v4/models`, valid. `buildTestPromptRequestForTest`
  uses the OpenAI-shape body for non-Gemini/non-Anthropic. `authHeadersFor` returns Bearer.
  `validateInputsForTest` checks `isValidHttpsUrl(baseUrl)` — ZAI's fixed URL is https, passes.

### 5.4 No changes in UI / store / processor

- **`LLMSettingsDialog`** (`src/UI/LLMSettingsDialog.cpp:30,103`): populates the combobox by
  iterating `getLLMProviderDefinitions()` → "Z.AI" appears automatically. Index math uses
  `llmProviderCount`, which is bumped, so selection stays correct.
- **`LLMSettingsStore`** (`src/AI/LLMSettingsStore.cpp:248,276`): serializes per-provider via
  the definitions loop; only `OpenAICompatible` is special-cased (for its custom-URL
  validation). ZAI needs no special case.
- **`PluginProcessor`** (`src/Plugin/PluginProcessor.cpp:3813`): `RestLlmClient::isConfigured(ps)`
  (non-empty key + model) and the `RestLlmClient` constructor are provider-agnostic; a
  configured ZAI provider wires `RestLlmClient` exactly as NVIDIA/OpenAI/etc. do.

## 6. State migration

`LLMSettingsStore` keys per-provider settings by `storageKey`. `"zai"` is a new key. Existing
saved settings for other providers are untouched. A ZAI row simply does not exist in old state
files and defaults to empty (empty key, empty model, `Untested`) — identical to any other
provider a given user has never configured. **No migration code is needed.**

A user who previously drove Z.AI through the "OpenAI Compatible" provider (with the `/v4` URL
typed manually) will not be auto-migrated to the new first-class ZAI provider; they can
re-select "Z.AI" and re-enter their key. This is acceptable: the OpenAI-Compatible path was
undocumented for Z.AI, and auto-migration would require guessing which custom-URL entries map
to Z.AI.

## 7. Tests

### 7.1 Update `tests/Unit/TestLLMSettings.cpp`

- `providers.size() == 7` → `== 8`
- Add, at index 6:
  ```cpp
  CHECK(providers[6].id == LLMProviderId::ZAI);
  CHECK(providers[6].displayName == "Z.AI");
  ```
- Shift the existing `OpenAICompatible` assertions from index 6 to index 7.
- In the "fixed base URLs" test, add:
  ```cpp
  CHECK(getLLMProviderDefinition(LLMProviderId::ZAI).fixedBaseUrl
        == "https://api.z.ai/api/coding/paas/v4");
  ```

### 7.2 Extend `tests/Unit/TestRestLlmClientHardening.cpp`

Add a ZAI test block mirroring the existing Gemini block, but exercising the generic
OpenAI-style path.

The existing `ScriptedHttpClient` ignores its `request` argument (it only counts calls and
returns scripted responses), so it cannot observe the built URL or headers. The existing
Gemini tests therefore only assert response-parsing behaviour, not request shape. For ZAI we
want to additionally pin the request endpoint, so add a small **capturing** test double local
to this test file:

```cpp
// Captures the last request so the ZAI endpoint/auth can be asserted. Returns a
// scripted response so complete() still resolves.
class CapturingHttpClient final : public ILLMHttpClient
{
public:
    LLMHttpResponse execute(const LLMHttpRequest& request) override
    {
        ++callCount;
        lastUrl    = request.url;
        lastHeaders = request.extraHeaders;
        const auto idx = static_cast<size_t>(callCount - 1);
        if (idx < responses.size())
            return responses[idx];
        return { 599, "script exhausted", "" };
    }
    std::vector<LLMHttpResponse> responses;
    std::atomic<int> callCount{ 0 };
    juce::String lastUrl;
    juce::String lastHeaders;
};
```

Two `TEST_CASE`s:

1. **Behaviour** — ZAI `LLMProviderSettings` (key + `selectedModel = "glm-5.2"`); a
   `CapturingHttpClient` returning a 200 with an OpenAI-shape body
   `{"choices":[{"message":{"content":"{\"steps\":[]}"}}],"usage":{"total_tokens":12}}`.
   Assert: `out.ok == true`, `out.content` is the parsed content string, `out.tokensUsed == 12`,
   and `client.providerName() == "rest-Z.AI"`.

2. **Request shape** — ZAI settings, `CapturingHttpClient` with one 200 response. After
   `complete()`, assert:
   - `lastUrl` ends with `/v4/chat/completions` (i.e. the `/v4` base was used, not `/v1`).
   - `lastHeaders` contains `Authorization: Bearer` and does NOT contain `x-api-key` or
     `x-goog-api-key`.

   This guards against ZAI accidentally being routed to the Anthropic or Gemini branch, and
   pins the `/v4` endpoint so a future base-URL refactor cannot silently break it.

## 8. Out of scope (YAGNI)

- No streaming. `RestLlmClient`/`LLMChatClient` are non-streaming today; Z.AI uses the same
  non-streaming path.
- No Z.AI-specific model list hardcoding. Models are fetched at runtime via `/models`
  (validator) and/or entered manually, like every other provider.
- No `customBaseUrl` toggle for ZAI (Approach A). If a user needs a proxy/alternate endpoint,
  the existing "OpenAI Compatible" provider remains available.
- No special timeout override. Z.AI gets the default 60s.

## 9. Risk

- **Index shift.** Bumping `OpenAICompatible` from index 6 → 7 changes its positional index.
  All consumers that use the index go through `llmProviderIndex()` / the definitions array,
  which are updated together. The only place the raw index is asserted is
  `TestLLMSettings.cpp`, which this spec updates. On-disk state is keyed by `storageKey`, not
  index, so existing saved settings are unaffected.
- **Endpoint correctness.** The design relies on `/chat/completions` and `/models` being valid
  under `/v4`. This matches the Z.AI docs. The test in 7.2 pins the exact built URL so a
  future refactor cannot silently break it.
