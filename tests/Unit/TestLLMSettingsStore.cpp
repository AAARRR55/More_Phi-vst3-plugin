#include <catch2/catch_test_macros.hpp>

#include "AI/LLMSettingsStore.h"

using namespace more_phi;

namespace {

juce::File makeConfigFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("morephi_llm_settings_store_" + juce::Uuid().toString());
    REQUIRE(directory.createDirectory().wasOk());
    return directory.getChildFile("llm_settings.json");
}

void cleanupConfigFile(const juce::File& file)
{
    file.getParentDirectory().deleteRecursively();
}

}

TEST_CASE("LLM settings store round-trips provider credentials, models, and status", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    auto& anthropic = settings.getProvider(LLMProviderId::Anthropic);
    anthropic.apiKey = "anthropic-key";
    anthropic.selectedModel = "claude-opus-test";
    anthropic.availableModels = { "claude-opus-test", "claude-sonnet-test" };
    anthropic.validationStatus = LLMValidationStatus::Active;
    anthropic.validationTimestampMs = 9876543210;
    anthropic.validationMessage = "Anthropic validated";
    settings.activeProvider = LLMProviderId::Anthropic;

    auto& compatible = settings.getProvider(LLMProviderId::OpenAICompatible);
    compatible.apiKey = "compatible-key";
    compatible.customBaseUrl = "https://llm.example.test/v1";
    compatible.selectedModel = "custom-model";
    compatible.availableModels = { "custom-model", "custom-fallback" };
    compatible.validationStatus = LLMValidationStatus::Failed;
    compatible.validationTimestampMs = 1234567890;
    compatible.validationMessage = "Custom endpoint failed";

    juce::String error;
    REQUIRE(store.save(settings, error));
    CHECK(error.isEmpty());

    auto loaded = LLMSettings::createDefault();
    REQUIRE(store.load(loaded, error));
    CHECK(error.isEmpty());

    REQUIRE(loaded.activeProvider.has_value());
    CHECK(*loaded.activeProvider == LLMProviderId::Anthropic);

    const auto& loadedAnthropic = loaded.getProvider(LLMProviderId::Anthropic);
    CHECK(loadedAnthropic.apiKey == "anthropic-key");
    CHECK(loadedAnthropic.selectedModel == "claude-opus-test");
    CHECK(loadedAnthropic.availableModels == juce::StringArray({ "claude-opus-test", "claude-sonnet-test" }));
    CHECK(loadedAnthropic.validationStatus == LLMValidationStatus::Active);
    CHECK(loadedAnthropic.validationTimestampMs == 9876543210);
    CHECK(loadedAnthropic.validationMessage == "Anthropic validated");

    const auto& loadedCompatible = loaded.getProvider(LLMProviderId::OpenAICompatible);
    CHECK(loadedCompatible.apiKey == "compatible-key");
    CHECK(loadedCompatible.customBaseUrl == "https://llm.example.test/v1");
    CHECK(loadedCompatible.selectedModel == "custom-model");
    CHECK(loadedCompatible.availableModels == juce::StringArray({ "custom-model", "custom-fallback" }));
    CHECK(loadedCompatible.validationStatus == LLMValidationStatus::Failed);
    CHECK(loadedCompatible.validationTimestampMs == 1234567890);
    CHECK(loadedCompatible.validationMessage == "Custom endpoint failed");

    cleanupConfigFile(configFile);
}

TEST_CASE("LLM settings store omits custom base URLs for fixed providers", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    settings.getProvider(LLMProviderId::Anthropic).customBaseUrl = "https://ignored.example.test";
    settings.getProvider(LLMProviderId::OpenAICompatible).customBaseUrl = "https://kept.example.test/v1";

    juce::String error;
    REQUIRE(store.save(settings, error));

    // SECURITY (Finding #2): the saved file is now an encrypted envelope, not
    // plaintext JSON. The on-disk content must NOT contain the customBaseUrl
    // value in cleartext, and must NOT parse as a plaintext providers object.
    const auto onDisk = configFile.loadFileAsString();
    CHECK_FALSE(onDisk.contains("https://ignored.example.test"));
    CHECK_FALSE(onDisk.contains("https://kept.example.test/v1"));

    // The envelope structure is visible (schema/method/enc) but the inner
    // providers object is encrypted — parsing the envelope as a flat providers
    // tree should NOT yield a providers field.
    const auto parsed = juce::JSON::parse(onDisk);
    if (parsed.isObject())
    {
        const auto providers = parsed.getProperty("providers", juce::var());
        CHECK_FALSE(providers.isObject()); // plaintext providers must not be present
    }

    // Round-trip through the store still preserves the customBaseUrl rule:
    // Anthropic's customBaseUrl is dropped on save; OpenAICompatible's is kept.
    auto loaded = LLMSettings::createDefault();
    REQUIRE(store.load(loaded, error));
    CHECK(loaded.getProvider(LLMProviderId::Anthropic).customBaseUrl.isEmpty());
    CHECK(loaded.getProvider(LLMProviderId::OpenAICompatible).customBaseUrl == "https://kept.example.test/v1");

    cleanupConfigFile(configFile);
}

TEST_CASE("Unvalidated LLM provider settings do not replace the active provider", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    settings.getProvider(LLMProviderId::Anthropic).validationStatus = LLMValidationStatus::Active;
    REQUIRE(settings.activateProviderIfValidated(LLMProviderId::Anthropic));

    auto& openAI = settings.getProvider(LLMProviderId::OpenAI);
    openAI.apiKey = "openai-key";
    openAI.selectedModel = "gpt-test";
    openAI.validationStatus = LLMValidationStatus::Untested;
    CHECK_FALSE(settings.activateProviderIfValidated(LLMProviderId::OpenAI));

    juce::String error;
    REQUIRE(store.save(settings, error));

    auto loaded = LLMSettings::createDefault();
    REQUIRE(store.load(loaded, error));

    REQUIRE(loaded.activeProvider.has_value());
    CHECK(*loaded.activeProvider == LLMProviderId::Anthropic);
    CHECK(loaded.getProvider(LLMProviderId::OpenAI).apiKey == "openai-key");
    CHECK(loaded.getProvider(LLMProviderId::OpenAI).validationStatus == LLMValidationStatus::Untested);

    cleanupConfigFile(configFile);
}

TEST_CASE("Missing LLM settings config loads defaults and clears the active provider", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    configFile.deleteFile();
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    settings.activeProvider = LLMProviderId::OpenAI;
    settings.getProvider(LLMProviderId::OpenAI).apiKey = "stale-key";

    juce::String error = "previous error";
    REQUIRE(store.load(settings, error));
    CHECK(error.isEmpty());
    CHECK_FALSE(settings.activeProvider.has_value());
    CHECK(settings.getProvider(LLMProviderId::OpenAI).apiKey.isEmpty());

    cleanupConfigFile(configFile);
}

TEST_CASE("Malformed LLM settings config reports a read error and resets defaults", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    REQUIRE(configFile.replaceWithText("{ malformed json"));
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    settings.activeProvider = LLMProviderId::Anthropic;
    settings.getProvider(LLMProviderId::Anthropic).apiKey = "stale-key";

    juce::String error;
    CHECK_FALSE(store.load(settings, error));
    CHECK(error.isNotEmpty());
    CHECK_FALSE(settings.activeProvider.has_value());
    CHECK(settings.getProvider(LLMProviderId::Anthropic).apiKey.isEmpty());

    cleanupConfigFile(configFile);
}

TEST_CASE("Unknown LLM active provider keys are ignored during load", "[unit][ai][llm][store]")
{
    const auto configFile = makeConfigFile();
    REQUIRE(configFile.replaceWithText(R"json({
        "version": 1,
        "activeProvider": "unknown_provider",
        "providers": {
            "anthropic": {
                "apiKey": "anthropic-key",
                "selectedModel": "claude-test",
                "availableModels": ["claude-test"],
                "validationStatus": "active",
                "validationTimestampMs": 111,
                "validationMessage": "ok"
            }
        }
    })json"));
    const LLMSettingsStore store(configFile);

    auto settings = LLMSettings::createDefault();
    juce::String error;
    REQUIRE(store.load(settings, error));
    CHECK(error.isEmpty());
    CHECK_FALSE(settings.activeProvider.has_value());
    CHECK(settings.getProvider(LLMProviderId::Anthropic).apiKey == "anthropic-key");
    CHECK(settings.getProvider(LLMProviderId::Anthropic).validationStatus == LLMValidationStatus::Active);

    cleanupConfigFile(configFile);
}

