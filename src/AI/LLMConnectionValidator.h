#pragma once

#include "LLMSettings.h"

#include <juce_events/juce_events.h>
#include <functional>
#include <memory>

namespace more_phi {

enum class LLMHttpMethod
{
    Get,
    Post
};

enum class LLMValidationOperation
{
    FetchModels,
    TestPrompt
};

struct LLMHttpRequest
{
    LLMHttpMethod method = LLMHttpMethod::Get;
    juce::String url;
    juce::String extraHeaders;
    juce::String body;
    int timeoutMs = 15000;
};

struct LLMHttpResponse
{
    int statusCode = 0;
    juce::String body;
    juce::String responseHeaders;
};

struct LLMValidationResult
{
    bool success = false;
    LLMValidationStatus status = LLMValidationStatus::Failed;
    juce::String message;
};

struct LLMModelFetchResult
{
    bool success = false;
    LLMValidationStatus status = LLMValidationStatus::Failed;
    juce::String message;
    juce::StringArray models;
};

class ILLMHttpClient
{
public:
    virtual ~ILLMHttpClient() = default;
    virtual LLMHttpResponse execute(const LLMHttpRequest& request) = 0;
};

class JuceLLMHttpClient final : public ILLMHttpClient
{
public:
    LLMHttpResponse execute(const LLMHttpRequest& request) override;
};

class LLMConnectionValidator final
{
public:
    using ModelCallback = std::function<void(LLMModelFetchResult)>;
    using TestCallback  = std::function<void(LLMValidationResult)>;

    LLMConnectionValidator();
    explicit LLMConnectionValidator(std::shared_ptr<ILLMHttpClient> httpClient);

    void fetchModelsAsync(LLMProviderId providerId, LLMProviderSettings settings, ModelCallback callback) const;
    void testConnectionAsync(LLMProviderId providerId, LLMProviderSettings settings, TestCallback callback) const;

    static LLMValidationResult    validateInputsForTest(LLMProviderId providerId,
                                                        const LLMProviderSettings& settings,
                                                        LLMValidationOperation operation);
    static LLMHttpRequest         buildFetchModelsRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings);
    static LLMHttpRequest         buildTestPromptRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings);
    static LLMModelFetchResult    parseModelListForTest(LLMProviderId providerId, int statusCode, const juce::String& body);
    static LLMValidationResult    parseTestPromptForTest(LLMProviderId providerId, int statusCode, const juce::String& body);

private:
    std::shared_ptr<ILLMHttpClient> httpClient_;
};

} // namespace more_phi
