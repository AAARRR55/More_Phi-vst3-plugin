#include <catch2/catch_test_macros.hpp>

#include "AI/LLMSettings.h"

using namespace more_phi;

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
    CHECK(getLLMProviderDefinition(LLMProviderId::Gemini).fixedBaseUrl == "https://generativelanguage.googleapis.com/v1beta");
    CHECK(getLLMProviderDefinition(LLMProviderId::ZAI).fixedBaseUrl == "https://api.z.ai/api/coding/paas/v4");
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
    // statusLabel() is the plain vocabulary (no colorblind-safety glyph prefix);
    // toDisplayString() prepends a glyph for the UI. Both must agree on the label.
    CHECK(statusLabel(LLMValidationStatus::NoProviderConfigured) == "No provider configured");
    CHECK(statusLabel(LLMValidationStatus::Untested) == "Untested");
    CHECK(statusLabel(LLMValidationStatus::Testing) == "Testing");
    CHECK(statusLabel(LLMValidationStatus::Active) == "Active");
    CHECK(statusLabel(LLMValidationStatus::Failed) == "Failed");

    // The glyph-prefixed display string must END WITH the plain label.
    using V = LLMValidationStatus;
    CHECK(toDisplayString(V::Active).endsWith(statusLabel(V::Active)));
    CHECK(toDisplayString(V::Failed).endsWith(statusLabel(V::Failed)));
    CHECK(toDisplayString(V::Untested).endsWith(statusLabel(V::Untested)));
}

// REGRESSION (COW-CORRUPT, 2026-06-27): juce::String is copy-on-write, so a
// copy of LLMProviderSettings shares the apiKey backing buffer with the original
// until a write forces a detach. The chat path captured such a copy into a
// background thread and then called zeroizeApiKey(), whose memset hit the SHARED
// buffer and wiped the caller's in-memory API key too ("the API key removes
// itself after a chat"). zeroizeApiKey() must now force a real detach first.
TEST_CASE("zeroizeApiKey() does not corrupt the source's API key (COW-CORRUPT)",
          "[unit][ai][llm][cow-corrupt]")
{
    LLMProviderSettings original;
    original.apiKey = "sk-secret-key-12345";

    // Copy out a second settings struct the way LLMChatClient::chat does.
    // Under COW, copy.apiKey and original.apiKey share one heap buffer until
    // a write detaches. The bug was zeroizeApiKey() memset-ing that shared buffer.
    LLMProviderSettings copy = original;
    REQUIRE(copy.apiKey == "sk-secret-key-12345");
    REQUIRE(original.apiKey == "sk-secret-key-12345");

    copy.zeroizeApiKey();

    // The copy is correctly wiped...
    CHECK(copy.apiKey.isEmpty());

    // ...but the ORIGINAL must still hold the key. Before the fix this failed.
    CHECK(original.apiKey == "sk-secret-key-12345");
}

TEST_CASE("zeroizeApiKey() is a no-op on an empty key", "[unit][ai][llm]")
{
    LLMProviderSettings original;
    original.apiKey = "";

    LLMProviderSettings copy = original;
    copy.zeroizeApiKey();

    CHECK(copy.apiKey.isEmpty());
    CHECK(original.apiKey.isEmpty());
}

TEST_CASE("getProvider() returns a copy whose zeroization leaves the owner intact",
          "[unit][ai][llm][cow-corrupt]")
{
    // This mirrors the exact LLMChatClient::chat() sequence: getProvider() (copy
    // out) -> capture -> background zeroizeApiKey(). The owner's key must survive.
    auto settings = LLMSettings::createDefault();
    settings.getProvider(LLMProviderId::OpenAI).apiKey = "sk-owner-key-abc";

    {
        const auto providerSettings = settings.getProvider(LLMProviderId::OpenAI);
        REQUIRE(providerSettings.apiKey == "sk-owner-key-abc");

        // Simulate the background-thread wipe on the captured copy.
        auto mutableCopy = providerSettings;
        mutableCopy.zeroizeApiKey();
        CHECK(mutableCopy.apiKey.isEmpty());
    }

    // The owner's key must be untouched.
    CHECK(settings.getProvider(LLMProviderId::OpenAI).apiKey == "sk-owner-key-abc");
}
