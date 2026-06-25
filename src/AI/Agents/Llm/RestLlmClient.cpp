// src/AI/Agents/Llm/RestLlmClient.cpp
#include "AI/Agents/Llm/RestLlmClient.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

namespace more_phi::agents {

using json = nlohmann::json;

RestLlmClient::RestLlmClient(LLMProviderId provider,
                             LLMProviderSettings providerSettings,
                             std::shared_ptr<ILLMHttpClient> httpClient)
    : provider_(provider),
      providerSettings_(std::move(providerSettings)),
      httpClient_(std::move(httpClient))
{
}

bool RestLlmClient::isConfigured(const LLMProviderSettings& ps) noexcept
{
    return ps.apiKey.trim().isNotEmpty() && ps.selectedModel.trim().isNotEmpty();
}

juce::String RestLlmClient::providerName() const
{
    return "rest-" + toDisplayString(provider_);
}

// Mirrors LLMChatClient's baseUrlFor / authHeadersFor (kept local to avoid
// pulling the whole chat client into the agent layer).
namespace {
juce::String baseUrlFor(LLMProviderId id, const LLMProviderSettings& ps)
{
    const auto& def = getLLMProviderDefinition(id);
    auto base = def.customBaseUrlAllowed ? ps.customBaseUrl.trim() : def.fixedBaseUrl;
    return base.trimCharactersAtEnd("/");
}

juce::String authHeadersFor(LLMProviderId id, const juce::String& apiKey)
{
    if (id == LLMProviderId::Anthropic)
        return "x-api-key: " + apiKey
               + "\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n";
    return "Authorization: Bearer " + apiKey + "\r\ncontent-type: application/json\r\n";
}
} // namespace

LLMHttpRequest RestLlmClient::buildRequest_(const CompletionRequest& request) const
{
    const auto base    = baseUrlFor(provider_, providerSettings_);
    const auto path    = (provider_ == LLMProviderId::Anthropic) ? "/v1/messages" : "/chat/completions";
    const auto headers = authHeadersFor(provider_, providerSettings_.apiKey.trim());
    const auto model   = providerSettings_.selectedModel.trim();
    const int  maxTok  = request.maxTokens > 0 ? request.maxTokens : 1024;

    json body;
    std::string bodyStr;
    try
    {
        if (provider_ == LLMProviderId::Anthropic)
        {
            json messages = json::array();
            messages.push_back({ { "role",    "user" },
                                 { "content", request.userPrompt.toStdString() } });
            body["model"]       = model.toStdString();
            body["max_tokens"]  = maxTok;
            body["system"]      = request.systemPrompt.toStdString();
            body["messages"]    = messages;
        }
        else
        {
            // Instruct the model to return strict JSON so we can parse "steps"
            // out of the content and surface them as a synthetic tool call that
            // ConductorAgent consumes. (No response_format: not all OpenAI-
            // compatible providers support it; the system prompt is portable.)
            json messages = json::array();
            const std::string systemText =
                request.systemPrompt.toStdString()
                + "\n\nRespond with STRICT JSON only, no prose. "
                  "Schema: {\"steps\":[{\"agent\":\"<role>\",\"goal\":\"<one-line>\"}]} "
                  "where role is one of: analysis, optimization, creative, "
                  "realtime, quality, memory.";
            messages.push_back({ { "role", "system" }, { "content", systemText } });
            messages.push_back({ { "role", "user" },   { "content", request.userPrompt.toStdString() } });
            body["model"]      = model.toStdString();
            body["max_tokens"] = maxTok;
            body["messages"]   = messages;
        }
        bodyStr = body.dump();
    }
    catch (...)
    {
        bodyStr = "{}";
    }

    return { LLMHttpMethod::Post, base + path, headers, juce::String::fromUTF8(bodyStr.c_str()), 30000 };
}

ILlmClient::CompletionResponse RestLlmClient::complete(const CompletionRequest& request)
{
    CompletionResponse out;
    if (! httpClient_)
    {
        out.errorCode = "no_http_client";
        return out;
    }

    const auto httpReq  = buildRequest_(request);
    const auto httpResp = httpClient_->execute(httpReq);

    if (httpResp.statusCode < 200 || httpResp.statusCode >= 300)
    {
        out.errorCode = "http_" + juce::String(httpResp.statusCode);
        out.content   = httpResp.body;
        return out;
    }

    // Extract the assistant text from OpenAI- or Anthropic-shaped responses.
    juce::String text;
    try
    {
        auto parsed = json::parse(httpResp.body.toStdString());
        if (provider_ == LLMProviderId::Anthropic)
        {
            // {"content":[{"type":"text","text":"..."}]}
            if (parsed.contains("content") && parsed["content"].is_array()
                && ! parsed["content"].empty() && parsed["content"][0].contains("text"))
                text = juce::String::fromUTF8(parsed["content"][0]["text"].get<std::string>().c_str());
        }
        else
        {
            // {"choices":[{"message":{"content":"..."}}]}
            if (parsed.contains("choices") && parsed["choices"].is_array()
                && ! parsed["choices"].empty() && parsed["choices"][0]["message"].contains("content"))
            {
                const auto& c = parsed["choices"][0]["message"]["content"];
                text = c.is_string() ? juce::String::fromUTF8(c.get<std::string>().c_str())
                                     : juce::String::fromUTF8(c.dump().c_str());
            }
        }
    }
    catch (...)
    {
        out.errorCode = "bad_response_json";
        out.content   = httpResp.body;
        return out;
    }

    out.ok      = true;
    out.content = text;

    // If the model obeyed the JSON instruction and returned a "steps" object,
    // surface it as a synthetic tool call so ConductorAgent::decomposeGoal
    // consumes it (it looks for arguments.steps). This is what makes a real LLM
    // drive the agent decomposition instead of the deterministic fallback.
    if (text.isNotEmpty())
    {
        try
        {
            auto asJson = json::parse(text.toStdString());
            if (asJson.is_object() && asJson.contains("steps"))
            {
                json tc = { { "arguments", asJson } };
                out.toolCalls = json::array({ tc });
            }
        }
        catch (...)
        {
            // Not JSON — leave toolCalls empty; the Conductor will fall back to
            // deterministic decomposition, which is the safe behaviour.
        }
    }

    return out;
}

} // namespace more_phi::agents
