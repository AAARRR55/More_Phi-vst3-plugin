// src/AI/Agents/AgentContext.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Agents/IAgent.h"

namespace more_phi {
class MorePhiProcessor;
struct InstanceIdentity;
class AutomationRuntime;
} // namespace more_phi

namespace more_phi::agents {

class BlackboardBridge;   // defined later in this phase

// The single chokepoint agents use to act. Default impl wraps MCPToolHandler::handle.
class IToolInvoker
{
public:
    virtual ~IToolInvoker() = default;
    // Returns the tool result as parsed JSON. On policy/capability/budget failure,
    // returns an object with { "error": { "code": "...", "message": "..." } }.
    virtual nlohmann::json invoke(const juce::String& toolName,
                                  const nlohmann::json& params,
                                  const juce::String& agentId) = 0;
};

class IAgentLogger
{
public:
    virtual ~IAgentLogger() = default;
    virtual void log(const juce::String& agentId,
                     const juce::String& level,            // error|warn|info|debug|trace
                     const juce::String& message,
                     const nlohmann::json& fields = nlohmann::json::object()) = 0;
};

// Injectable LLM seam (Risk R1 mitigation). The real transport is currently broken;
// agents that need an LLM receive this and fall back to a deterministic client.
class ILlmClient
{
public:
    virtual ~ILlmClient() = default;

    struct CompletionRequest
    {
        juce::String systemPrompt;
        juce::String userPrompt;
        nlohmann::json tools = nlohmann::json::array();  // optional tool schema list
        int maxTokens = 1024;
    };
    struct CompletionResponse
    {
        bool ok = false;
        juce::String content;
        nlohmann::json toolCalls = nlohmann::json::array();
        int tokensUsed = 0;
        juce::String errorCode;
    };

    virtual CompletionResponse complete(const CompletionRequest& request) = 0;
    virtual juce::String providerName() const = 0;
};

struct AgentContext
{
    MorePhiProcessor*       processor = nullptr;
    const InstanceIdentity* identity  = nullptr;
    AutomationRuntime*      runtime   = nullptr;     // ledger/permissions/events/workflows/memory
    IToolInvoker*           tools     = nullptr;     // the chokepoint wrapper (never null at runtime)
    BlackboardBridge*       blackboard= nullptr;     // never null at runtime
    IAgentLogger*           logger    = nullptr;     // never null at runtime (NullAgentLogger in tests)
    ILlmClient*             llm       = nullptr;     // may be null; agents must handle gracefully
};

} // namespace more_phi::agents
