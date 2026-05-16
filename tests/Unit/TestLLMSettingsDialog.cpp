#include <catch2/catch_test_macros.hpp>

#include "UI/LLMSettingsDialog.h"

using namespace more_phi;

TEST_CASE("LLM settings dialog exposes exactly the approved provider names", "[unit][ui][llm]")
{
    const auto names = LLMSettingsDialog::providerNamesForMenu();

    REQUIRE(names.size() == 6);
    CHECK(names[0] == "NVIDIA");
    CHECK(names[1] == "DeepSeek");
    CHECK(names[2] == "OpenAI");
    CHECK(names[3] == "Anthropic");
    CHECK(names[4] == "OpenRouter");
    CHECK(names[5] == "OpenAI Compatible");
}

TEST_CASE("LLM settings dialog only allows OpenAI Compatible base URL edits", "[unit][ui][llm]")
{
    CHECK_FALSE(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::NVIDIA));
    CHECK_FALSE(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::DeepSeek));
    CHECK_FALSE(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::OpenAI));
    CHECK_FALSE(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::Anthropic));
    CHECK_FALSE(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::OpenRouter));
    CHECK(LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId::OpenAICompatible));
}
