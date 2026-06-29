#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>

#include <juce_core/juce_core.h>

namespace more_phi {

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

enum class LLMValidationStatus
{
    NoProviderConfigured,
    Untested,
    Testing,
    Active,
    Failed
};

struct LLMProviderDefinition
{
    LLMProviderId id;
    juce::String storageKey;
    juce::String displayName;
    juce::String fixedBaseUrl;
    bool customBaseUrlAllowed = false;
};

struct LLMProviderSettings
{
    juce::String apiKey;

    // SECURITY (Finding #4): Zeroize the API key when this struct is discarded.
    // Call this explicitly at known exit points rather than relying solely on the
    // destructor.
    //
    // IMPORTANT (COW-CORRUPT fix, 2026-06-27): juce::String uses copy-on-write, so
    // a copy of this struct (e.g. one captured into a background thread) may share
    // the SAME backing buffer as the original (e.g. the panel's in-memory settings).
    // A raw memset on a shared buffer would zero out the caller's copy too — which
    // was the root cause of "the LLM provider API key removes itself after a chat".
    // We therefore force a real COW detach BEFORE wiping, so only our private copy
    // is touched. Detaching via fromUTF8(rawBytes) guarantees a fresh allocation.
    void zeroizeApiKey()
    {
        if (apiKey.isEmpty())
            return;

        // Force COW detach: copy out the UTF-8 bytes (a real allocation) then
        // rebuild the string from them. After this, apiKey owns a private buffer
        // even if it was previously shared with other juce::String instances.
        apiKey = juce::String::fromUTF8(apiKey.toRawUTF8(),
                                        static_cast<int>(apiKey.getNumBytesAsUTF8()));

        const int len = apiKey.length();
        auto* rawPtr = const_cast<juce::String::CharPointerType::CharType*>(
            apiKey.getCharPointer().getAddress());
        std::memset(rawPtr, 0, static_cast<size_t>(len * sizeof(juce::String::CharPointerType::CharType)));
        volatile const void* sink = rawPtr;
        (void)sink;
        apiKey = {};
    }
    juce::String customBaseUrl;
    juce::StringArray availableModels;
    juce::String selectedModel;
    LLMValidationStatus validationStatus = LLMValidationStatus::Untested;
    juce::int64 validationTimestampMs = 0;
    juce::String validationMessage;
};

constexpr std::size_t llmProviderCount = 7;

const std::array<LLMProviderDefinition, llmProviderCount>& getLLMProviderDefinitions();
const LLMProviderDefinition& getLLMProviderDefinition(LLMProviderId id);
std::optional<LLMProviderId> llmProviderIdFromStorageKey(const juce::String& storageKey);
std::size_t llmProviderIndex(LLMProviderId id) noexcept;
juce::String toStorageKey(LLMProviderId id);
juce::String toStorageKey(LLMValidationStatus status);
juce::String toDisplayString(LLMProviderId id);
juce::String toDisplayString(LLMValidationStatus status);
// Plain status label WITHOUT the colorblind-safety glyph prefix. Use this for
// non-UI contexts (storage keys, logging, tests) where the glyph would be noise.
// toDisplayString() == glyph + " " + statusLabel() for the UI.
juce::String statusLabel(LLMValidationStatus status);
std::optional<LLMValidationStatus> llmValidationStatusFromStorageKey(const juce::String& storageKey);

class LLMSettings
{
public:
    static LLMSettings createDefault();

    LLMProviderSettings& getProvider(LLMProviderId id) noexcept;
    const LLMProviderSettings& getProvider(LLMProviderId id) const noexcept;

    juce::String getBaseUrl(LLMProviderId id) const;
    bool activateProviderIfValidated(LLMProviderId id);
    LLMValidationStatus getToolbarStatus() const;
    juce::String getActiveProviderDisplayName() const;

    std::optional<LLMProviderId> activeProvider;
    std::array<LLMProviderSettings, llmProviderCount> providers;
};

}
