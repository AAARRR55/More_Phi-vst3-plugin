// src/AI/Agents/Llm/RestLlmClient.cpp
#include "AI/Agents/Llm/RestLlmClient.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <thread>

namespace more_phi::agents {

using json = nlohmann::json;

// ── Phase 3 hardening constants ──────────────────────────────────────────────
// Retry policy for transient failures. complete() runs on agent scheduler
// workers only (never audio/message thread), so bounded sleeps are safe.
namespace {
constexpr int  kMaxAttempts       = 3;     // initial + 2 retries
constexpr int  kBaseBackoffMs     = 1000;  // 1s, 2s, (4s) exponential
constexpr int  kMaxBackoffMs      = 8000;
constexpr int  kRateLimitDefaultMs= 5000;  // if server omits Retry-After
}

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
    if (id == LLMProviderId::Gemini)
        return "x-goog-api-key: " + apiKey + "\r\ncontent-type: application/json\r\n";
    return "Authorization: Bearer " + apiKey + "\r\ncontent-type: application/json\r\n";
}

// Should this HTTP status be retried? 429 (rate limit) and 5xx (server) yes;
// 4xx (client error — bad key, bad request) no — retrying wastes the user's
// quota and won't fix the problem.
bool isRetriableStatus(int status) noexcept
{
    return status == 429 || (status >= 500 && status <= 599);
}

// Parse a Retry-After header (seconds or HTTP-date). We only honour the integer
// seconds form; HTTP-date is rare for LLM APIs and falls back to the default.
int parseRetryAfterMs(const juce::String& responseHeaders) noexcept
{
    if (responseHeaders.isEmpty())
        return kRateLimitDefaultMs;

    // Header map is "\r\n"-separated "Name: value" lines (juce URL style). Case
    // fold to lower for a tolerant match.
    auto lower = responseHeaders.toLowerCase();
    auto idx = lower.indexOf("retry-after:");
    if (idx < 0)
        return kRateLimitDefaultMs;

    auto rest = responseHeaders.substring(idx + static_cast<int>(juce::String("retry-after:").length()))
                    .upToFirstOccurrenceOf("\r\n", false, false)
                    .trim();
    auto seconds = rest.getIntValue();
    if (seconds <= 0)
        return kRateLimitDefaultMs;
    // Cap at kMaxBackoffMs so a hostile/misconfigured server can't stall an
    // agent worker for minutes.
    return std::min(seconds * 1000, kMaxBackoffMs);
}

int backoffMsForAttempt(int attempt)
{
    // Exponential 1s, 2s, 4s... capped, plus up to 25% jitter to decorrelate
    // concurrent clients retrying in lockstep.
    const int base = std::min(kBaseBackoffMs * (1 << (attempt - 1)), kMaxBackoffMs);
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> jitter(0, base / 4);
    return base + jitter(rng);
}

// The set of valid specialist-agent roles the Conductor may dispatch to.
// Mirrors the 6 specialists + conductor in src/AI/Agents/Agents/. Used to
// validate the model's returned steps[] before promoting them to toolCalls —
// a hallucinated agent:"foo" must NOT propagate into the scheduler.
bool isValidAgentRole(const std::string& role) noexcept
{
    static const std::array<const char*, 7> kValid = {
        "analysis", "optimization", "creative",
        "realtime", "quality", "memory", "conductor"
    };
    return std::find(kValid.begin(), kValid.end(), role) != kValid.end();
}

// Validate and (re)shape a model-returned steps[] object. Returns true and
// populates `clean` only if every step has a valid agent role + non-empty goal.
// This is the gate that turns "any JSON with a steps key" into a trusted plan.
bool validateSteps(const json& steps, json& clean) noexcept
{
    if (! steps.is_array() || steps.empty())
        return false;

    json validated = json::array();
    for (const auto& step : steps)
    {
        if (! step.is_object())
            continue;
        const auto roleIt = step.find("agent");
        const auto goalIt = step.find("goal");
        if (roleIt == step.end() || goalIt == step.end())
            continue;
        if (! roleIt->is_string() || ! goalIt->is_string())
            continue;
        const auto role = roleIt->get<std::string>();
        const auto goal = goalIt->get<std::string>();
        if (! isValidAgentRole(role))
            continue;
        if (goal.empty())
            continue;
        validated.push_back({ { "agent", role }, { "goal", goal } });
    }

    if (validated.empty())
        return false;
    clean = std::move(validated);
    return true;
}
} // namespace

LLMHttpRequest RestLlmClient::buildRequest_(const CompletionRequest& request) const
{
    const auto base    = baseUrlFor(provider_, providerSettings_);
    const auto model   = providerSettings_.selectedModel.trim();
    const int  maxTok  = request.maxTokens > 0 ? request.maxTokens : 1024;

    juce::String path;
    if (provider_ == LLMProviderId::Gemini)
        path = "/models/" + model + ":generateContent";
    else if (provider_ == LLMProviderId::Anthropic)
        path = "/v1/messages";
    else
        path = "/chat/completions";

    const auto headers = authHeadersFor(provider_, providerSettings_.apiKey.trim());

    json body;
    std::string bodyStr;
    try
    {
        if (provider_ == LLMProviderId::Gemini)
        {
            json contents = json::array();
            contents.push_back({ { "parts", json::array({ { { "text", request.userPrompt.toStdString() } } }) } });
            body["contents"] = contents;
            if (request.systemPrompt.isNotEmpty())
                body["systemInstruction"] = { { "parts", json::array({ { { "text", request.systemPrompt.toStdString() } } }) } };
            body["generationConfig"] = { { "maxOutputTokens", maxTok } };
        }
        else if (provider_ == LLMProviderId::Anthropic)
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

    const auto httpReq = buildRequest_(request);

    // ── Phase 3: retry transient failures with backoff ─────────────────────
    // 429 → honour Retry-After. 5xx → exponential backoff + jitter. 4xx → fail
    // fast (no retry). Network errors (statusCode==0) are retried like 5xx.
    LLMHttpResponse httpResp{};
    int lastStatus = 0;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
    {
        httpResp = httpClient_->execute(httpReq);
        lastStatus = httpResp.statusCode;

        // Success or non-retriable → stop.
        if (! isRetriableStatus(lastStatus) && lastStatus != 0)
            break;

        // Last attempt exhausted — don't sleep, just fall through with the error.
        if (attempt == kMaxAttempts)
            break;

        const int sleepMs = (lastStatus == 429)
            ? parseRetryAfterMs(httpResp.responseHeaders)
            : backoffMsForAttempt(attempt);

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    if (lastStatus == 0)
    {
        out.errorCode = "network_error";
        out.content   = httpResp.body;
        return out;
    }
    if (lastStatus < 200 || lastStatus >= 300)
    {
        out.errorCode = "http_" + juce::String(lastStatus);
        out.content   = httpResp.body;
        return out;
    }

    // Extract the assistant text from OpenAI- or Anthropic-shaped responses.
    juce::String text;
    int tokensUsed = 0;
    try
    {
        auto parsed = json::parse(httpResp.body.toStdString());

        // Phase 3: token accounting from the response "usage" envelope so the
        // existing TokenOptimizer budget enforcement sees real cost.
        const auto usageIt = parsed.find("usage");
        if (usageIt != parsed.end() && usageIt->is_object())
        {
            const auto totIt = usageIt->find("total_tokens");
            if (totIt != usageIt->end() && totIt->is_number_integer())
                tokensUsed = totIt->get<int>();
        }

        if (provider_ == LLMProviderId::Gemini)
        {
            // {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
            if (parsed.contains("candidates") && parsed["candidates"].is_array()
                && !parsed["candidates"].empty()
                && parsed["candidates"][0].contains("content")
                && parsed["candidates"][0]["content"].contains("parts")
                && parsed["candidates"][0]["content"]["parts"].is_array()
                && !parsed["candidates"][0]["content"]["parts"].empty()
                && parsed["candidates"][0]["content"]["parts"][0].contains("text"))
            {
                text = juce::String::fromUTF8(parsed["candidates"][0]["content"]["parts"][0]["text"].get<std::string>().c_str());
            }
            // Gemini uses usageMetadata.totalTokenCount instead of usage.total_tokens
            const auto usageMdIt = parsed.find("usageMetadata");
            if (usageMdIt != parsed.end() && usageMdIt->is_object())
            {
                const auto totIt = usageMdIt->find("totalTokenCount");
                if (totIt != usageMdIt->end() && totIt->is_number_integer())
                    tokensUsed = totIt->get<int>();
            }
        }
        else if (provider_ == LLMProviderId::Anthropic)
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

    out.ok          = true;
    out.content     = text;
    out.tokensUsed  = tokensUsed;

    // ── Phase 3: validated steps[] promotion ───────────────────────────────
    // If the model obeyed the JSON instruction and returned a "steps" object,
    // validate every step (valid agent role + non-empty goal) BEFORE surfacing
    // it as a synthetic tool call. Previously any {"steps":[...]} shape was
    // accepted, letting a hallucinated agent:"foo" propagate into the scheduler.
    if (text.isNotEmpty())
    {
        try
        {
            auto asJson = json::parse(text.toStdString());
            if (asJson.is_object() && asJson.contains("steps"))
            {
                json validated;
                if (validateSteps(asJson["steps"], validated))
                {
                    json cleanPlan = { { "steps", validated },
                                       { "source", "rest-" + toDisplayString(provider_).toStdString() } };
                    out.toolCalls = json::array({ { { "arguments", cleanPlan } } });
                }
                // else: steps present but none validated → leave toolCalls
                // empty; ConductorAgent falls back to deterministic decomposition
                // (the documented safe behaviour).
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
