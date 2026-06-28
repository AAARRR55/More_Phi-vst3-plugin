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
    const int ord = ordinal(role);
    if (ord < 0 || ord >= static_cast<int>(agentsByRole_.size()))
        return false;

    // H-1 FIX: O(1) duplicate check via enum-indexed array.
    if (agentsByRole_[static_cast<size_t>(ord)])
        return false;   // role already taken

    agentsByRole_[static_cast<size_t>(ord)] = std::move(agent);
    order_.push_back({ role, ord });
    return true;
}

IAgent* AgentRegistry::find(AgentRole role) const noexcept
{
    // H-1 FIX: O(1) array lookup, no iteration.
    const int ord = ordinal(role);
    if (ord < 0 || ord >= static_cast<int>(agentsByRole_.size()))
        return nullptr;
    return agentsByRole_[static_cast<size_t>(ord)].get();
}

std::vector<AgentRole> AgentRegistry::registeredRoles() const noexcept
{
    std::vector<AgentRole> r;
    r.reserve(order_.size());
    for (const auto& s : order_)
        r.push_back(s.role);
    return r;
}

void AgentRegistry::prepareAll(const AgentContext& ctx)
{
    for (auto& s : order_)
        if (auto& agent = agentsByRole_[static_cast<size_t>(s.ordinal)])
            agent->prepare(ctx);
}

void AgentRegistry::stopAll()
{
    for (auto& s : order_)
        if (auto& agent = agentsByRole_[static_cast<size_t>(s.ordinal)])
            agent->stop();
}

} // namespace more_phi::agents
