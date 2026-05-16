#include <catch2/catch_test_macros.hpp>

#include "AI/LLMConnectionValidator.h"

using namespace more_phi;

TEST_CASE("Validator builds provider-specific model fetch requests", "[unit][ai][llm][validator]")
{
    LLMProviderSettings settings;
    settings.apiKey = "test-key";
    settings.customBaseUrl = "https://compatible.example/v1/";

    auto openAIRequest = LLMConnectionValidator::buildFetchModelsRequestForTest(LLMProviderId::OpenAI, settings);
    CHECK(openAIRequest.url == "https://api.openai.com/v1/models");
    CHECK(openAIRequest.method == LLMHttpMethod::Get);
    CHECK(openAIRequest.extraHeaders.contains("Authorization: Bearer test-key"));

    auto anthropicRequest = LLMConnectionValidator::buildFetchModelsRequestForTest(LLMProviderId::Anthropic, settings);
    CHECK(anthropicRequest.url == "https://api.anthropic.com/v1/models");
    CHECK(anthropicRequest.extraHeaders.contains("x-api-key: test-key"));
    CHECK(anthropicRequest.extraHeaders.contains("anthropic-version: 2023-06-01"));

    auto compatibleRequest = LLMConnectionValidator::buildFetchModelsRequestForTest(LLMProviderId::OpenAICompatible, settings);
    CHECK(compatibleRequest.url == "https://compatible.example/v1/models");
}

TEST_CASE("Validator rejects missing API key and invalid compatible base URL", "[unit][ai][llm][validator]")
{
    LLMProviderSettings settings;
    settings.selectedModel = "model";

    auto result = LLMConnectionValidator::validateInputsForTest(LLMProviderId::OpenAI, settings, LLMValidationOperation::FetchModels);
    CHECK_FALSE(result.success);
    CHECK(result.message.containsIgnoreCase("API key"));

    settings.apiKey = "key";
    settings.customBaseUrl = "not-a-url";
    result = LLMConnectionValidator::validateInputsForTest(LLMProviderId::OpenAICompatible, settings, LLMValidationOperation::FetchModels);
    CHECK_FALSE(result.success);
    CHECK(result.message.containsIgnoreCase("Base URL"));
}

TEST_CASE("Validator parses OpenAI-style and Anthropic-style model lists", "[unit][ai][llm][validator]")
{
    const auto openAI = LLMConnectionValidator::parseModelListForTest(
        LLMProviderId::OpenAI,
        200,
        R"json({"data":[{"id":"gpt-a"},{"id":"gpt-b"}]})json");
    REQUIRE(openAI.success);
    REQUIRE(openAI.models.size() == 2);
    CHECK(openAI.models[0] == "gpt-a");
    CHECK(openAI.models[1] == "gpt-b");

    const auto anthropic = LLMConnectionValidator::parseModelListForTest(
        LLMProviderId::Anthropic,
        200,
        R"json({"data":[{"id":"claude-a"}]})json");
    REQUIRE(anthropic.success);
    REQUIRE(anthropic.models.size() == 1);
    CHECK(anthropic.models[0] == "claude-a");
}

TEST_CASE("Validator reports auth failure and malformed model responses", "[unit][ai][llm][validator]")
{
    const auto auth = LLMConnectionValidator::parseModelListForTest(LLMProviderId::OpenAI, 401, R"json({"error":"bad key"})json");
    CHECK_FALSE(auth.success);
    CHECK(auth.message.containsIgnoreCase("auth"));

    const auto malformed = LLMConnectionValidator::parseModelListForTest(LLMProviderId::OpenAI, 200, R"json({"models":[]})json");
    CHECK_FALSE(malformed.success);
    CHECK(malformed.message.containsIgnoreCase("model list"));
}

TEST_CASE("Validator parses test prompt responses and requires OK", "[unit][ai][llm][validator]")
{
    const auto openAISuccess = LLMConnectionValidator::parseTestPromptForTest(
        LLMProviderId::OpenAI,
        200,
        R"json({"choices":[{"message":{"content":" OK \n"}}]})json");
    CHECK(openAISuccess.success);

    const auto anthropicSuccess = LLMConnectionValidator::parseTestPromptForTest(
        LLMProviderId::Anthropic,
        200,
        R"json({"content":[{"type":"text","text":"OK"}]})json");
    CHECK(anthropicSuccess.success);

    const auto promptFailure = LLMConnectionValidator::parseTestPromptForTest(
        LLMProviderId::OpenAI,
        200,
        R"json({"choices":[{"message":{"content":"NO"}}]})json");
    CHECK_FALSE(promptFailure.success);
    CHECK(promptFailure.message.containsIgnoreCase("test prompt"));
}
