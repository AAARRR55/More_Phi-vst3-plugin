// src/AI/Agents/DeterministicFallbackLlmClient.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

// A deterministic stand-in for the (currently broken) real LLM transport.
// It does not call out over HTTP; it returns canned, intent-keyed plans so the
// agent loop is testable and degrades gracefully end-to-end (Risk R1 mitigation).
// This mirrors the pattern in scripts/neural-mastering/control/agentic_mastering_demo.py.
class DeterministicFallbackLlmClient : public ILlmClient
{
public:
    CompletionResponse complete(const CompletionRequest& request) override;
    juce::String providerName() const override { return "deterministic-fallback"; }

    // Parses a free-form intent into a structured decomposition the Conductor can use
    // without an LLM. Recognizes mastering-target keywords; returns a generic plan
    // otherwise. Public so tests can drive it directly.
    static nlohmann::json decomposeIntent(const juce::String& intent);
};

} // namespace more_phi::agents
