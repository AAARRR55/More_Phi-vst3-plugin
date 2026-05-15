#include <catch2/catch_test_macros.hpp>

#include "AI/LLMSettings.h"

using namespace more_phi;

TEST_CASE("LLM provider definitions are exactly the approved providers", "[unit][ai][llm]")
{
    const auto& providers = getLLMProviderDefinitions();

    REQUIRE(providers.size() == 6);
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
    CHECK(providers[5].id == LLMProviderId::OpenAICompatible);
    CHECK(providers[5].displayName == "OpenAI Compatible");
}

TEST_CASE("Known providers have fixed base URLs and OpenAI Compatible is editable", "[unit][ai][llm]")
{
    for (const auto& provider : getLLMProviderDefinitions())
    {
        if (provider.id == LLMProviderId::OpenAICompatible)
        {
            CHECK(provider.customBaseUrlAllowed);
            CHECK(provider.fixedBaseUrl.isEmpty());
        }
        else
        {
            CHECK_FALSE(provider.customBaseUrlAllowed);
            CHECK(provider.fixedBaseUrl.startsWith("https://"));
        }
    }

    CHECK(getLLMProviderDefinition(LLMProviderId::NVIDIA).fixedBaseUrl == "https://integrate.api.nvidia.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::DeepSeek).fixedBaseUrl == "https://api.deepseek.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenAI).fixedBaseUrl == "https://api.openai.com/v1");
    CHECK(getLLMProviderDefinition(LLMProviderId::Anthropic).fixedBaseUrl == "https://api.anthropic.com");
    CHECK(getLLMProviderDefinition(LLMProviderId::OpenRouter).fixedBaseUrl == "https://openrouter.ai/api/v1");
}

TEST_CASE("Default LLM settings have no active provider", "[unit][ai][llm]")
{
    const auto settings = LLMSettings::createDefault();

    REQUIRE_FALSE(settings.activeProvider.has_value());
    for (const auto& provider : getLLMProviderDefinitions())
    {
        const auto& providerSettings = settings.getProvider(provider.id);
        CHECK(providerSettings.apiKey.isEmpty());
        CHECK(providerSettings.selectedModel.isEmpty());
        CHECK(providerSettings.availableModels.isEmpty());
        CHECK(providerSettings.validationStatus == LLMValidationStatus::Untested);
        CHECK(providerSettings.validationMessage.isEmpty());
    }
}

TEST_CASE("Provider activation requires a successful validation status", "[unit][ai][llm]")
{
    auto settings = LLMSettings::createDefault();
    auto& openAI = settings.getProvider(LLMProviderId::OpenAI);
    openAI.apiKey = "sk-test";
    openAI.selectedModel = "gpt-test";

    openAI.validationStatus = LLMValidationStatus::Untested;
    CHECK_FALSE(settings.activateProviderIfValidated(LLMProviderId::OpenAI));
    CHECK_FALSE(settings.activeProvider.has_value());

    openAI.validationStatus = LLMValidationStatus::Failed;
    CHECK_FALSE(settings.activateProviderIfValidated(LLMProviderId::OpenAI));
    CHECK_FALSE(settings.activeProvider.has_value());

    openAI.validationStatus = LLMValidationStatus::Active;
    CHECK(settings.activateProviderIfValidated(LLMProviderId::OpenAI));
    REQUIRE(settings.activeProvider.has_value());
    CHECK(*settings.activeProvider == LLMProviderId::OpenAI);
}

TEST_CASE("Status labels match toolbar and modal vocabulary", "[unit][ai][llm]")
{
    CHECK(toDisplayString(LLMValidationStatus::NoProviderConfigured) == "No provider configured");
    CHECK(toDisplayString(LLMValidationStatus::Untested) == "Untested");
    CHECK(toDisplayString(LLMValidationStatus::Testing) == "Testing");
    CHECK(toDisplayString(LLMValidationStatus::Active) == "Active");
    CHECK(toDisplayString(LLMValidationStatus::Failed) == "Failed");
}
