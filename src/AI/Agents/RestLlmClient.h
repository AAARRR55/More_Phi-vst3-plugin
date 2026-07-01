// src/AI/Agents/RestLlmClient.h
#pragma once

#include "AI/Agents/AgentContext.h"          // ILlmClient
#include "AI/LLMConnectionValidator.h"       // ILLMHttpClient
#include "AI/LLMSettings.h"

#include <memory>

namespace more_phi::agents {

// AUDIT-FIX (close the "AI theater" gap): a REAL ILlmClient that drives the
// agent layer (ConductorAgent::decomposeGoal etc.) against a configured LLM
// provider (OpenAI / Anthropic / OpenAI-compatible). Mirrors the proven request
// format used by the chat-panel LLMChatClient. Used when the user has validated
// an API key; otherwise the runtime falls back to DeterministicFallbackLlmClient.
//
// complete() is synchronous and may block for seconds — it is only ever called
// on the agent scheduler worker threads, never on the audio or message thread.
class RestLlmClient : public ILlmClient
{
public:
    RestLlmClient(LLMProviderId provider,
                  LLMProviderSettings providerSettings,
                  std::shared_ptr<ILLMHttpClient> httpClient);

    CompletionResponse complete(const CompletionRequest& request) override;
    juce::String providerName() const override;

    // Returns true if the supplied settings are sufficient to make a call
    // (non-empty key + model). Used by the runtime wiring to decide whether to
    // use this client or the deterministic fallback.
    static bool isConfigured(const LLMProviderSettings& ps) noexcept;

private:
    LLMProviderId provider_;
    LLMProviderSettings providerSettings_;
    std::shared_ptr<ILLMHttpClient> httpClient_;

    LLMHttpRequest buildRequest_(const CompletionRequest& request) const;
};

} // namespace more_phi::agents
