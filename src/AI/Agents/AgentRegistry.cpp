// src/AI/Agents/AgentRegistry.cpp
#include "AI/Agents/AgentRegistry.h"

#include <cassert>

namespace more_phi::agents {

AgentRegistry::~AgentRegistry()
{
    stopAll();
}

bool AgentRegistry::registerAgent(std::unique_ptr<IAgent> agent)
{
    // M3 FIX: debug-only contract guard. find()/registeredRoles() are read
    // concurrently from scheduler workers + MCP threads once the runtime has
    // started; a registerAgent() after seal() would be a data race. Fail loud
    // in debug so the mistake is caught before it ships as a latent race.
    // (In release we still reject via the role-duplicate check or accept the
    // pre-seal-only usage; the contract is documented in the header.)
    assert(! sealed_.load(std::memory_order_acquire)
           && "AgentRegistry::registerAgent called after seal() — register before start()");

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
