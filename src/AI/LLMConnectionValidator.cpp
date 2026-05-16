#include "LLMConnectionValidator.h"

#include <nlohmann/json.hpp>
#include <thread>

namespace more_phi {

namespace {

constexpr auto testPrompt = "Reply with exactly: OK";

juce::String trimTrailingSlash(juce::String url)
{
    return url.trim().trimCharactersAtEnd("/");
}

bool isValidHttpsUrl(const juce::String& value)
{
    const auto trimmed = value.trim();
    return trimmed.startsWithIgnoreCase("https://") && trimmed.length() > 8
           && !trimmed.containsChar(' ');
}

juce::String baseUrlFor(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto& definition = getLLMProviderDefinition(providerId);
    if (definition.customBaseUrlAllowed)
        return trimTrailingSlash(settings.customBaseUrl);
    return trimTrailingSlash(definition.fixedBaseUrl);
}

LLMValidationResult inputError(const juce::String& message)
{
    return {false, LLMValidationStatus::Failed, message};
}

LLMModelFetchResult modelError(const juce::String& message)
{
    return {false, LLMValidationStatus::Failed, message, {}};
}

juce::String authHeadersFor(LLMProviderId providerId, const juce::String& apiKey)
{
    if (providerId == LLMProviderId::Anthropic)
        return "x-api-key: " + apiKey + "\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n";

    return "Authorization: Bearer " + apiKey + "\r\ncontent-type: application/json\r\n";
}

juce::String extractOpenAIText(const nlohmann::json& root)
{
    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty())
        return {};

    const auto& first = root["choices"].front();
    if (!first.contains("message") || !first["message"].is_object())
        return {};

    const auto& message = first["message"];
    if (!message.contains("content") || !message["content"].is_string())
        return {};

    return juce::String(message["content"].get<std::string>());
}

juce::String extractAnthropicText(const nlohmann::json& root)
{
    if (!root.contains("content") || !root["content"].is_array())
        return {};

    juce::String text;
    for (const auto& block : root["content"])
    {
        if (block.is_object() && block.contains("text") && block["text"].is_string())
            text += juce::String(block["text"].get<std::string>());
    }
    return text;
}

} // namespace

LLMHttpResponse JuceLLMHttpClient::execute(const LLMHttpRequest& request)
{
    int statusCode = 0;
    juce::StringPairArray responseHeaders;
    auto url = juce::URL(request.url);

    if (request.method == LLMHttpMethod::Post)
        url = url.withPOSTData(request.body);

    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(request.timeoutMs)
            .withExtraHeaders(request.extraHeaders)
            .withStatusCode(&statusCode)
            .withResponseHeaders(&responseHeaders)
            .withNumRedirectsToFollow(3));
    if (stream == nullptr)
        return {statusCode, {}, {}};

    return {statusCode, stream->readEntireStreamAsString(), responseHeaders.getDescription()};
}

LLMConnectionValidator::LLMConnectionValidator()
    : httpClient_(std::make_shared<JuceLLMHttpClient>())
{
}

LLMConnectionValidator::LLMConnectionValidator(std::shared_ptr<ILLMHttpClient> httpClient)
    : httpClient_(std::move(httpClient))
{
}

LLMValidationResult LLMConnectionValidator::validateInputsForTest(LLMProviderId providerId,
                                                                  const LLMProviderSettings& settings,
                                                                  LLMValidationOperation operation)
{
    if (settings.apiKey.trim().isEmpty())
        return inputError("Missing API key.");

    const auto baseUrl = baseUrlFor(providerId, settings);
    if (!isValidHttpsUrl(baseUrl))
        return inputError("Invalid OpenAI Compatible Base URL. Use an https:// URL.");

    if (operation == LLMValidationOperation::TestPrompt && settings.selectedModel.trim().isEmpty())
        return inputError("Missing model selection.");

    return {true, LLMValidationStatus::Untested, {}};
}

LLMHttpRequest LLMConnectionValidator::buildFetchModelsRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto baseUrl = baseUrlFor(providerId, settings);
    const auto path = providerId == LLMProviderId::Anthropic ? "/v1/models" : "/models";
    return {LLMHttpMethod::Get, baseUrl + path, authHeadersFor(providerId, settings.apiKey.trim()), {}, 15000};
}

LLMHttpRequest LLMConnectionValidator::buildTestPromptRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto baseUrl = baseUrlFor(providerId, settings);
    const auto model = settings.selectedModel.trim();

    if (providerId == LLMProviderId::Anthropic)
    {
        nlohmann::json body;
        body["model"] = model.toStdString();
        body["max_tokens"] = 16;
        body["messages"] = {{{"role", "user"}, {"content", testPrompt}}};
        return {LLMHttpMethod::Post, baseUrl + "/v1/messages", authHeadersFor(providerId, settings.apiKey.trim()), juce::String(body.dump()), 15000};
    }

    nlohmann::json body;
    body["model"] = model.toStdString();
    body["max_tokens"] = 16;
    body["messages"] = {{{"role", "user"}, {"content", testPrompt}}};
    return {LLMHttpMethod::Post, baseUrl + "/chat/completions", authHeadersFor(providerId, settings.apiKey.trim()), juce::String(body.dump()), 15000};
}

LLMModelFetchResult LLMConnectionValidator::parseModelListForTest(LLMProviderId /*providerId*/, int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return modelError("Authentication failed while fetching models.");
    if (statusCode <= 0)
        return modelError("Network timeout while fetching models.");
    if (statusCode < 200 || statusCode >= 300)
        return modelError("Model list request failed with HTTP " + juce::String(statusCode) + ".");

    try
    {
        const auto root = nlohmann::json::parse(body.toStdString());
        if (!root.contains("data") || !root["data"].is_array())
            return modelError("Unsupported model list response format.");

        juce::StringArray models;
        for (const auto& item : root["data"])
            if (item.is_object() && item.contains("id") && item["id"].is_string())
                models.add(juce::String(item["id"].get<std::string>()));

        models.removeEmptyStrings();
        models.removeDuplicates(false);
        if (models.isEmpty())
            return modelError("Model list parse failure: no model IDs returned.");

        return {true, LLMValidationStatus::Untested, "Fetched " + juce::String(models.size()) + " models.", models};
    }
    catch (const std::exception& e)
    {
        return modelError("Model list parse failure: " + juce::String(e.what()));
    }
}

LLMValidationResult LLMConnectionValidator::parseTestPromptForTest(LLMProviderId providerId, int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return inputError("Authentication failed while testing the provider.");
    if (statusCode <= 0)
        return inputError("Network timeout while testing the provider.");
    if (statusCode < 200 || statusCode >= 300)
        return inputError("Test prompt failed with HTTP " + juce::String(statusCode) + ".");

    try
    {
        const auto root = nlohmann::json::parse(body.toStdString());
        const auto text = providerId == LLMProviderId::Anthropic ? extractAnthropicText(root) : extractOpenAIText(root);
        if (!text.trim().contains("OK"))
            return inputError("Test prompt failure: provider did not reply with OK.");

        return {true, LLMValidationStatus::Active, "Connection test succeeded."};
    }
    catch (const std::exception& e)
    {
        return inputError("Unsupported provider response format: " + juce::String(e.what()));
    }
}

void LLMConnectionValidator::fetchModelsAsync(LLMProviderId providerId, LLMProviderSettings settings, ModelCallback callback) const
{
    const auto input = validateInputsForTest(providerId, settings, LLMValidationOperation::FetchModels);
    if (!input.success)
    {
        juce::MessageManager::callAsync([callback = std::move(callback), input]() mutable {
            callback({false, input.status, input.message, {}});
        });
        return;
    }

    auto client = httpClient_;
    std::thread([client, providerId, settings = std::move(settings), callback = std::move(callback)]() mutable {
        const auto request = buildFetchModelsRequestForTest(providerId, settings);
        const auto response = client->execute(request);
        auto result = parseModelListForTest(providerId, response.statusCode, response.body);
        juce::MessageManager::callAsync([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    }).detach();
}

void LLMConnectionValidator::testConnectionAsync(LLMProviderId providerId, LLMProviderSettings settings, TestCallback callback) const
{
    const auto input = validateInputsForTest(providerId, settings, LLMValidationOperation::TestPrompt);
    if (!input.success)
    {
        juce::MessageManager::callAsync([callback = std::move(callback), input]() mutable {
            callback(input);
        });
        return;
    }

    auto client = httpClient_;
    std::thread([client, providerId, settings = std::move(settings), callback = std::move(callback)]() mutable {
        const auto request = buildTestPromptRequestForTest(providerId, settings);
        const auto response = client->execute(request);
        auto result = parseTestPromptForTest(providerId, response.statusCode, response.body);
        juce::MessageManager::callAsync([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    }).detach();
}

} // namespace more_phi
