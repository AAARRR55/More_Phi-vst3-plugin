// src/AI/Agents/AgentRegistry.cpp
#include "AI/Agents/AgentRegistry.h"

namespace more_phi::agents {

AgentRegistry::~AgentRegistry()
{
    stopAll();
}

bool AgentRegistry::registerAgent(std::unique_ptr<IAgent> agent)
{
    if (! agent)
        return false;
    const auto role = agent->role();
    for (const auto& slot : slots_)
        if (slot.role == role)
            return false;   // role already taken
    slots_.push_back({ role, std::move(agent) });
    return true;
}

IAgent* AgentRegistry::find(AgentRole role) const noexcept
{
    for (const auto& slot : slots_)
        if (slot.role == role)
            return slot.agent.get();
    return nullptr;
}

std::vector<AgentRole> AgentRegistry::registeredRoles() const noexcept
{
    std::vector<AgentRole> r;
    r.reserve(slots_.size());
    for (const auto& s : slots_)
        r.push_back(s.role);
    return r;
}

void AgentRegistry::prepareAll(const AgentContext& ctx)
{
    for (auto& slot : slots_)
        if (slot.agent)
            slot.agent->prepare(ctx);
}

void AgentRegistry::stopAll()
{
    for (auto& slot : slots_)
        if (slot.agent)
            slot.agent->stop();
}

} // namespace more_phi::agents
