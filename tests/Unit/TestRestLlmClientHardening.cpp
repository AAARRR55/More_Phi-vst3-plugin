// tests/Unit/TestRestLlmClientHardening.cpp
//
// Phase 3.1/3.2: proves the real REST LLM client degrades gracefully and
// validates model output. Uses a fake ILLMHttpClient so no network is needed.
//
// Covers:
//  - 429 Retry-After honoured (retried, then succeeds)
//  - 5xx retried with backoff, eventually fails with http_5xx
//  - 4xx fail-fast (no retry — single execute call)
//  - network error (statusCode 0) → "network_error"
//  - malformed steps[] (invalid agent role, empty goal) filtered out → no
//    toolCalls promoted → ConductorAgent would fall back to deterministic
//  - valid steps[] promoted to a synthetic tool call with validated content
//  - token accounting: tokensUsed populated from response "usage"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Agents/Llm/RestLlmClient.h"
#include "AI/LLMConnectionValidator.h"
#include "AI/LLMSettings.h"

#include <atomic>
#include <memory>

using namespace more_phi;
using namespace more_phi::agents;
using json = nlohmann::json;

namespace {

// Fake HTTP client that returns a scripted sequence of responses. Records the
// number of execute() calls so retry behaviour is observable.
class ScriptedHttpClient final : public ILLMHttpClient
{
public:
    LLMHttpResponse execute(const LLMHttpRequest& /*request*/) override
    {
        ++callCount;
        const auto idx = static_cast<size_t>(callCount - 1);
        if (idx < responses.size())
            return responses[idx];
        // Default to a generic failure if the script runs out.
        return { 599, "script exhausted", "" };
    }

    std::vector<LLMHttpResponse> responses;
    std::atomic<int> callCount{ 0 };
};

LLMProviderSettings openAiSettings()
{
    LLMProviderSettings ps;
    ps.apiKey = "sk-test-key";
    ps.selectedModel = "gpt-4o-mini";
    return ps;
}

// Build a successful OpenAI-shaped body with the given content + usage.
juce::String openAiSuccessBody(const std::string& content, int totalTokens)
{
    json body;
    body["choices"] = json::array({
        json::object({ { "message", json::object({ { "content", content } }) } })
    });
    body["usage"] = { { "total_tokens", totalTokens },
                      { "prompt_tokens", 10 },
                      { "completion_tokens", totalTokens - 10 } };
    return juce::String(body.dump());
}

RestLlmClient makeClient(std::shared_ptr<ScriptedHttpClient> http)
{
    return { LLMProviderId::OpenAI, openAiSettings(), http };
}

ILlmClient::CompletionRequest sampleRequest()
{
    ILlmClient::CompletionRequest r;
    r.systemPrompt = "You decompose a mastering goal into specialist-agent steps.";
    r.userPrompt = "Make this track warmer and louder";
    r.maxTokens = 512;
    return r;
}

} // namespace

TEST_CASE("RestLlmClient retries 429 then succeeds", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = {
        { 429, "{}", "Retry-After: 1\r\nContent-Type: application/json\r\n" },
        { 429, "{}", "retry-after: 1\r\n" }, // case-insensitive header match
        { 200, openAiSuccessBody(R"({"steps":[{"agent":"analysis","goal":"analyse spectrum"}]})", 42).toStdString(), "" }
    };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(http->callCount == 3); // 2 retries + 1 success
    REQUIRE(resp.tokensUsed == 42);
    REQUIRE(resp.toolCalls.is_array());
    REQUIRE(resp.toolCalls.size() == 1);
}

TEST_CASE("RestLlmClient retries 5xx then fails fast on exhaustion", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = {
        { 503, "unavailable", "" },
        { 503, "unavailable", "" },
        { 503, "unavailable", "" },
    };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.errorCode == "http_503");
    REQUIRE(http->callCount == 3); // exactly kMaxAttempts
}

TEST_CASE("RestLlmClient fails fast on 4xx (no retry)", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 401, "bad key", "" } };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.errorCode == "http_401");
    REQUIRE(http->callCount == 1); // single attempt, no retry
}

TEST_CASE("RestLlmClient reports network_error on statusCode 0", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 0, "", "" }, { 0, "", "" }, { 0, "", "" } };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.errorCode == "network_error");
}

TEST_CASE("RestLlmClient filters malformed steps from model output", "[agents][llm]")
{
    // Model hallucinated an invalid agent role + an empty goal; only the one
    // valid step should survive validation. If NONE survive, toolCalls stays
    // empty so the Conductor falls back to deterministic decomposition.
    auto http = std::make_shared<ScriptedHttpClient>();
    const auto bad = R"({"steps":[
        {"agent":"bogus_role","goal":"should be dropped"},
        {"agent":"optimization","goal":""},
        {"agent":"analysis","goal":"valid step"}
    ]})";
    http->responses = { { 200, openAiSuccessBody(bad, 7).toStdString(), "" } };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    // The single valid step promoted → toolCalls has one entry whose
    // arguments.steps has exactly one validated step.
    REQUIRE(resp.toolCalls.is_array());
    REQUIRE(resp.toolCalls.size() == 1);
    const auto& args = resp.toolCalls[0]["arguments"];
    REQUIRE(args.contains("steps"));
    REQUIRE(args["steps"].size() == 1);
    REQUIRE(args["steps"][0]["agent"] == "analysis");
}

TEST_CASE("RestLlmClient promotes no toolCalls when all steps are invalid", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    const auto allBad = R"({"steps":[
        {"agent":"not-a-role","goal":"x"},
        {"foo":"bar"}
    ]})";
    http->responses = { { 200, openAiSuccessBody(allBad, 3).toStdString(), "" } };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(resp.tokensUsed == 3);
    // No validated step → no toolCalls → Conductor falls back. This is the
    // safety contract that keeps a hallucinating model from injecting bad work.
    REQUIRE(resp.toolCalls.empty());
}

TEST_CASE("RestLlmClient handles non-JSON content without crashing", "[agents][llm]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 200, openAiSuccessBody("Sure, here's a plan: ...", 5).toStdString(), "" } };

    auto client = makeClient(http);
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(resp.content.isNotEmpty());
    REQUIRE(resp.toolCalls.empty()); // not JSON → no promotion
}

TEST_CASE("RestLlmClient isConfigured requires key + model", "[agents][llm]")
{
    LLMProviderSettings empty;
    REQUIRE_FALSE(RestLlmClient::isConfigured(empty));

    auto ps = openAiSettings();
    REQUIRE(RestLlmClient::isConfigured(ps));

    ps.apiKey = "   "; // whitespace only
    REQUIRE_FALSE(RestLlmClient::isConfigured(ps));
}

TEST_CASE("RestLlmClient providerName is rest-prefixed", "[agents][llm]")
{
    auto client = makeClient(std::make_shared<ScriptedHttpClient>());
    REQUIRE(client.providerName().startsWith("rest-"));
}

// ── Gemini provider coverage ───────────────────────────────────────────────
// Mirrors the OpenAI cases but against Gemini's response shape
// ({"candidates":[{"content":{"parts":[{"text":"..."}]}}], "usageMetadata":{...}}).

LLMProviderSettings geminiSettings()
{
    LLMProviderSettings ps;
    ps.apiKey = "AIza-test-key";
    ps.selectedModel = "gemini-2.0-flash";
    return ps;
}

juce::String geminiSuccessBody(const std::string& content, int totalTokens)
{
    json body;
    body["candidates"] = json::array({
        json::object({
            { "content", json::object({ { "parts", json::array({ json::object({ { "text", content } }) }) } }) }
        })
    });
    body["usageMetadata"] = { { "totalTokenCount", totalTokens } };
    return juce::String(body.dump());
}

TEST_CASE("RestLlmClient parses Gemini response text + usageMetadata", "[agents][llm][gemini]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 200, geminiSuccessBody("plain text plan", 99).toStdString(), "" } };

    RestLlmClient client{ LLMProviderId::Gemini, geminiSettings(), http };
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(resp.content == "plain text plan");
    REQUIRE(resp.tokensUsed == 99);
}

TEST_CASE("RestLlmClient promotes validated steps from Gemini content", "[agents][llm][gemini]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    const auto plan = R"({"steps":[{"agent":"analysis","goal":"analyse spectrum"}]})";
    http->responses = { { 200, geminiSuccessBody(plan, 50).toStdString(), "" } };

    RestLlmClient client{ LLMProviderId::Gemini, geminiSettings(), http };
    auto resp = client.complete(sampleRequest());

    REQUIRE(resp.ok);
    REQUIRE(resp.tokensUsed == 50);
    REQUIRE(resp.toolCalls.is_array());
    REQUIRE(resp.toolCalls.size() == 1);
}

TEST_CASE("RestLlmClient reports network_error for Gemini when statusCode 0", "[agents][llm][gemini]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 0, "", "" }, { 0, "", "" }, { 0, "", "" } };

    RestLlmClient client{ LLMProviderId::Gemini, geminiSettings(), http };
    auto resp = client.complete(sampleRequest());

    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.errorCode == "network_error");
}

TEST_CASE("RestLlmClient fails fast on Gemini 4xx", "[agents][llm][gemini]")
{
    auto http = std::make_shared<ScriptedHttpClient>();
    http->responses = { { 403, "forbidden", "" } };

    RestLlmClient client{ LLMProviderId::Gemini, geminiSettings(), http };
    auto resp = client.complete(sampleRequest());

    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.errorCode == "http_403");
    REQUIRE(http->callCount == 1);
}

TEST_CASE("RestLlmClient Gemini providerName is rest-Google Gemini", "[agents][llm][gemini]")
{
    RestLlmClient client{ LLMProviderId::Gemini, geminiSettings(), std::make_shared<ScriptedHttpClient>() };
    CHECK(client.providerName() == "rest-Google Gemini");
}

TEST_CASE("RestLlmClient isConfigured works for Gemini settings", "[agents][llm][gemini]")
{
    REQUIRE(RestLlmClient::isConfigured(geminiSettings()));
    LLMProviderSettings empty;
    REQUIRE_FALSE(RestLlmClient::isConfigured(empty));
}
