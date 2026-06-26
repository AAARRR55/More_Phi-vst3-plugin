#include "LLMSettings.h"

namespace more_phi {

namespace {

const std::array<LLMProviderDefinition, llmProviderCount> definitions {{
    { LLMProviderId::NVIDIA, "nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1", false },
    { LLMProviderId::DeepSeek, "deepseek", "DeepSeek", "https://api.deepseek.com/v1", false },
    { LLMProviderId::OpenAI, "openai", "OpenAI", "https://api.openai.com/v1", false },
    { LLMProviderId::Anthropic, "anthropic", "Anthropic", "https://api.anthropic.com", false },
    { LLMProviderId::OpenRouter, "openrouter", "OpenRouter", "https://openrouter.ai/api/v1", false },
    { LLMProviderId::OpenAICompatible, "openai_compatible", "OpenAI Compatible", {}, true },
}};

}

const std::array<LLMProviderDefinition, llmProviderCount>& getLLMProviderDefinitions()
{
    return definitions;
}

const LLMProviderDefinition& getLLMProviderDefinition(LLMProviderId id)
{
    return definitions[llmProviderIndex(id)];
}

std::optional<LLMProviderId> llmProviderIdFromStorageKey(const juce::String& storageKey)
{
    for (const auto& definition : definitions)
    {
        if (definition.storageKey == storageKey)
            return definition.id;
    }

    return std::nullopt;
}

std::size_t llmProviderIndex(LLMProviderId id) noexcept
{
    switch (id)
    {
        case LLMProviderId::NVIDIA: return 0;
        case LLMProviderId::DeepSeek: return 1;
        case LLMProviderId::OpenAI: return 2;
        case LLMProviderId::Anthropic: return 3;
        case LLMProviderId::OpenRouter: return 4;
        case LLMProviderId::OpenAICompatible: return 5;
    }

    jassertfalse;
    return 0;
}

juce::String toStorageKey(LLMProviderId id)
{
    return getLLMProviderDefinition(id).storageKey;
}

juce::String toStorageKey(LLMValidationStatus status)
{
    switch (status)
    {
        case LLMValidationStatus::NoProviderConfigured: return "no_provider_configured";
        case LLMValidationStatus::Untested: return "untested";
        case LLMValidationStatus::Testing: return "testing";
        case LLMValidationStatus::Active: return "active";
        case LLMValidationStatus::Failed: return "failed";
    }

    jassertfalse;
    return {};
}

juce::String toDisplayString(LLMProviderId id)
{
    return getLLMProviderDefinition(id).displayName;
}

juce::String statusLabel(LLMValidationStatus status)
{
    switch (status)
    {
        case LLMValidationStatus::NoProviderConfigured: return "No provider configured";
        case LLMValidationStatus::Untested: return "Untested";
        case LLMValidationStatus::Testing: return "Testing";
        case LLMValidationStatus::Active: return "Active";
        case LLMValidationStatus::Failed:  return "Failed";
    }

    jassertfalse;
    return {};
}

juce::String toDisplayString(LLMValidationStatus status)
{
    // R6: glyph prefix gives shape redundancy so state isn't carried by colour
    // alone (colourblind-safe). Kept ASCII-safe + ✓/✕ which JUCE's default
    // fonts render reliably. The plain label is factored into statusLabel() so
    // non-UI contexts (storage keys, logging, tests) don't carry the glyph.
    switch (status)
    {
        case LLMValidationStatus::NoProviderConfigured: return "-- " + statusLabel(status);
        case LLMValidationStatus::Untested: return "? " + statusLabel(status);
        case LLMValidationStatus::Testing: return "... " + statusLabel(status);
        case LLMValidationStatus::Active: return juce::String::charToString(0x2713) + " " + statusLabel(status);   // check mark
        case LLMValidationStatus::Failed:  return juce::String::charToString(0x2715) + " " + statusLabel(status);  // ballot X
    }

    jassertfalse;
    return {};
}

std::optional<LLMValidationStatus> llmValidationStatusFromStorageKey(const juce::String& storageKey)
{
    if (storageKey == "no_provider_configured")
        return LLMValidationStatus::NoProviderConfigured;
    if (storageKey == "untested")
        return LLMValidationStatus::Untested;
    if (storageKey == "testing")
        return LLMValidationStatus::Testing;
    if (storageKey == "active")
        return LLMValidationStatus::Active;
    if (storageKey == "failed")
        return LLMValidationStatus::Failed;

    return std::nullopt;
}

LLMSettings LLMSettings::createDefault()
{
    return {};
}

LLMProviderSettings& LLMSettings::getProvider(LLMProviderId id) noexcept
{
    return providers[llmProviderIndex(id)];
}

const LLMProviderSettings& LLMSettings::getProvider(LLMProviderId id) const noexcept
{
    return providers[llmProviderIndex(id)];
}

juce::String LLMSettings::getBaseUrl(LLMProviderId id) const
{
    const auto& definition = getLLMProviderDefinition(id);
    if (definition.customBaseUrlAllowed)
        return getProvider(id).customBaseUrl;

    return definition.fixedBaseUrl;
}

bool LLMSettings::activateProviderIfValidated(LLMProviderId id)
{
    if (getProvider(id).validationStatus != LLMValidationStatus::Active)
        return false;

    activeProvider = id;
    return true;
}

LLMValidationStatus LLMSettings::getToolbarStatus() const
{
    if (! activeProvider.has_value())
        return LLMValidationStatus::NoProviderConfigured;

    return getProvider(*activeProvider).validationStatus;
}

juce::String LLMSettings::getActiveProviderDisplayName() const
{
    if (! activeProvider.has_value())
        return {};

    return toDisplayString(*activeProvider);
}

}
