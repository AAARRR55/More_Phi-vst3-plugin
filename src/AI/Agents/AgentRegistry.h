// src/AI/Agents/AgentRegistry.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <memory>
#include <vector>

namespace more_phi::agents {

// Open registry. Adding an agent never touches core infrastructure.
// One agent per role (first registered wins; duplicates are rejected).
class AgentRegistry
{
public:
    AgentRegistry() = default;
    ~AgentRegistry();

    // Takes ownership. Returns true if registered, false if the role is already taken.
    bool registerAgent(std::unique_ptr<IAgent> agent);

    IAgent* find(AgentRole role) const noexcept;
    std::vector<AgentRole> registeredRoles() const noexcept;

    // Wires the shared AgentContext into every registered agent.
    void prepareAll(const AgentContext& ctx);

    // Cooperative cancel + mark Stopped.
    void stopAll();

private:
    struct Slot
    {
        AgentRole role = AgentRole::Custom;
        std::unique_ptr<IAgent> agent;
    };
    std::vector<Slot> slots_;
};

} // namespace more_phi::agents
