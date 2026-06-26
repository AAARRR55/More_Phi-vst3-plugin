#pragma once

#include <array>
#include <cstddef>
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
    juce::String customBaseUrl;
    juce::StringArray availableModels;
    juce::String selectedModel;
    LLMValidationStatus validationStatus = LLMValidationStatus::Untested;
    juce::int64 validationTimestampMs = 0;
    juce::String validationMessage;
};

constexpr std::size_t llmProviderCount = 6;

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
