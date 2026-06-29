// src/AI/Agents/Tooling/AgentToolError.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

namespace more_phi::agents {

inline nlohmann::json agentToolError(const juce::String& code, const juce::String& message)
{
    return nlohmann::json::object({
        { "error", nlohmann::json::object({
            { "code", code.toStdString() },
            { "message", message.toStdString() }
        }) }
    });
}

} // namespace more_phi::agents
