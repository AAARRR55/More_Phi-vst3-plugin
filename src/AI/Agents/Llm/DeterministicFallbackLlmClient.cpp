// src/AI/Agents/Llm/DeterministicFallbackLlmClient.cpp
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"

namespace more_phi::agents {

namespace {
bool containsCi(const juce::String& haystack, const char* needle)
{
    return haystack.toLowerCase().contains(juce::String(needle).toLowerCase());
}
} // namespace

nlohmann::json DeterministicFallbackLlmClient::decomposeIntent(const juce::String& intent)
{
    // Deterministic decomposition: detect mastering-for-streaming intent and emit
    // a fixed plan (Analysis → Memory-recall → Optimization with streaming targets).
    // Intentionally simple; the real LLM would produce richer plans.
    nlohmann::json plan = nlohmann::json::object();
    plan["intent"] = intent.toStdString();

    nlohmann::json steps = nlohmann::json::array();
    steps.push_back({ { "agent", "analysis" }, { "intent", "measure current state" } });
    steps.push_back({ { "agent", "memory" },   { "intent", "recall relevant priors" } });

    nlohmann::json target = nlohmann::json::object();
    target["lufsIntegrated"] = -14.0;
    target["truePeakMaxDb"]  = -1.0;
    if (containsCi(intent, "warm"))
        target["preserveLowShelf"] = true;
    if (containsCi(intent, "bright") || containsCi(intent, "sparkle"))
        target["airShelf"] = true;

    nlohmann::json optStep = nlohmann::json::object();
    optStep["agent"]  = "optimization";
    optStep["intent"] = "optimize toward streaming target";
    optStep["payload"] = { { "target", target } };
    steps.push_back(optStep);

    plan["steps"]  = steps;
    plan["source"] = "deterministic-fallback";
    return plan;
}

ILlmClient::CompletionResponse DeterministicFallbackLlmClient::complete(const CompletionRequest& request)
{
    CompletionResponse r;
    r.ok = true;
    r.content = "(deterministic plan)";
    r.toolCalls = nlohmann::json::array();
    r.toolCalls.push_back({ { "name", "conductor.apply_plan" },
                            { "arguments", decomposeIntent(request.userPrompt) } });
    r.tokensUsed = 0;
    return r;
}

} // namespace more_phi::agents
