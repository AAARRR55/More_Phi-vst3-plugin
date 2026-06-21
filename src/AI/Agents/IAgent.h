// src/AI/Agents/IAgent.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <vector>

namespace more_phi::agents {

// Forward declarations to avoid heavy includes in this contract header.
struct AgentContext;
class IntegrationEvent;     // defined in AI/AutomationControlPlane.h (real type)
// Note: the real IntegrationEvent is in more_phi:: (not agents::). We reuse it
// via the include in AgentContext.h. Here we only need it as a struct tag for
// AgentResult::emitEvents and IAgent::onEvent, so a forward decl in more_phi:: suffices.

enum class AgentRole
{
    Conductor,
    Analysis,
    Optimization,
    Creative,
    RealtimeControl,
    QualitySafety,
    Memory,
    Custom
};

enum class AgentState
{
    Unregistered,
    Idle,
    Busy,
    Draining,
    Stopped,
    Failed
};

enum class TaskPriority
{
    Background,        // bookkeeping, memory compaction
    Normal,            // typical analysis/optimization
    High,              // user-initiated goal subtasks
    RealtimeCritical   // reactive correction — jumps the AGENT queue only (NOT audio thread)
};

inline juce::String toString(AgentRole r)
{
    switch (r)
    {
        case AgentRole::Conductor:       return "conductor";
        case AgentRole::Analysis:        return "analysis";
        case AgentRole::Optimization:    return "optimization";
        case AgentRole::Creative:        return "creative";
        case AgentRole::RealtimeControl: return "realtime";
        case AgentRole::QualitySafety:   return "quality";
        case AgentRole::Memory:          return "memory";
        case AgentRole::Custom:          return "custom";
    }
    return "custom";
}

inline juce::String toString(AgentState s)
{
    switch (s)
    {
        case AgentState::Unregistered: return "unregistered";
        case AgentState::Idle:         return "idle";
        case AgentState::Busy:         return "busy";
        case AgentState::Draining:     return "draining";
        case AgentState::Stopped:      return "stopped";
        case AgentState::Failed:       return "failed";
    }
    return "idle";
}

inline juce::String toString(TaskPriority p)
{
    switch (p)
    {
        case TaskPriority::Background:        return "background";
        case TaskPriority::Normal:            return "normal";
        case TaskPriority::High:              return "high";
        case TaskPriority::RealtimeCritical:  return "realtime-critical";
    }
    return "normal";
}

inline AgentRole agentRoleFromString(const juce::String& s)
{
    const auto v = s.toLowerCase().trim();
    if (v == "conductor")        return AgentRole::Conductor;
    if (v == "analysis")         return AgentRole::Analysis;
    if (v == "optimization")     return AgentRole::Optimization;
    if (v == "creative")         return AgentRole::Creative;
    if (v == "realtime" || v == "realtimecontrol") return AgentRole::RealtimeControl;
    if (v == "quality" || v == "qualitysafety")    return AgentRole::QualitySafety;
    if (v == "memory")           return AgentRole::Memory;
    return AgentRole::Custom;
}

struct AgentTask
{
    juce::String id;
    juce::String runId;            // originating conductor workflow run (empty for top-level)
    AgentRole   targetRole = AgentRole::Custom;
    juce::String intent;           // NL or structured
    nlohmann::json payload = nlohmann::json::object();
    TaskPriority priority = TaskPriority::Normal;
    juce::int64  deadlineMs = 0;   // soft deadline, 0 = none (stored as ms since epoch)
    juce::String origin;           // "user" | "conductor" | <agentId> | "mcp"
};

// We reuse more_phi::IntegrationEvent directly (it already has source/type/payload/timestamp).
// To avoid a hard include cycle, AgentResult holds them as nlohmann::json envelopes that the
// runtime converts to more_phi::IntegrationEvent at publish time. This keeps IAgent.h light.
struct AgentEventEnvelope
{
    juce::String type;
    nlohmann::json payload = nlohmann::json::object();
};

struct AgentResult
{
    juce::String taskId;
    bool success = false;
    juce::String errorCode;
    nlohmann::json findings = nlohmann::json::object();
    std::vector<nlohmann::json> proposedActions;        // tool calls for conductor to re-dispatch
    std::vector<AgentEventEnvelope> emitEvents;         // blackboard posts (runtime publishes as IntegrationEvent)
    std::vector<AgentTask>           followUps;          // honored ONLY if returned by Conductor
    nlohmann::json telemetry = nlohmann::json::object(); // tokens, latencyMs, toolsCalled[]
};

class IAgent
{
public:
    virtual ~IAgent() = default;

    virtual AgentRole    role() const noexcept = 0;
    virtual juce::String id() const noexcept = 0;
    virtual std::vector<juce::String> allowedTools() const = 0;        // capability scope
    virtual std::vector<juce::String> subscribedEventTypes() const { return {}; }
    virtual bool requireApprovalRegardlessOfAutonomy() const { return false; }

    virtual void prepare(const AgentContext& ctx) = 0;                 // wire dependencies
    virtual AgentResult execute(const AgentTask& task) = 0;            // sync; runs on a scheduler worker
    virtual void onEvent(const juce::String& eventType,
                         const nlohmann::json& payload,
                         const juce::String& source,
                         const juce::String& runId) { (void) eventType; (void) payload; (void) source; (void) runId; }

    virtual AgentState state() const noexcept = 0;
    virtual void stop() = 0;                                           // cooperative cancel
};

} // namespace more_phi::agents
