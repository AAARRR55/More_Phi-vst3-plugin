// src/AI/Agents/Logging/NullAgentLogger.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

class NullAgentLogger : public IAgentLogger
{
public:
    void log(const juce::String&, const juce::String&, const juce::String&,
             const nlohmann::json&) override
    {
        // no-op — used in tests
    }
};

} // namespace more_phi::agents
