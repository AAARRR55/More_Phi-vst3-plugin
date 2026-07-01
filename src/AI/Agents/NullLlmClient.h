// src/AI/Agents/NullLlmClient.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

// Always reports unavailable. Agents must handle this gracefully (degrade to
// deterministic logic). Used when no LLM transport is wired.
class NullLlmClient : public ILlmClient
{
public:
    CompletionResponse complete(const CompletionRequest&) override
    {
        CompletionResponse r;
        r.ok = false;
        r.errorCode = "llm_unavailable";
        return r;
    }
    juce::String providerName() const override { return "null"; }
};

} // namespace more_phi::agents
