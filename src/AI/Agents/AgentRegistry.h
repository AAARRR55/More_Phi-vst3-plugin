// src/AI/Agents/AgentRegistry.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <memory>
#include <array>
#include <vector>

namespace more_phi::agents {

// Open registry. Adding an agent never touches core infrastructure.
// One agent per role (first registered wins; duplicates are rejected).
//
// Threading contract (M3 FIX): registerAgent() is NOT synchronized. It MUST be
// called only BEFORE start() seals the registry and spawns workers — find() and
// registeredRoles() are read concurrently from scheduler workers and MCP threads
// once running, and a late registerAgent() would race those readers. seal() is
// invoked by AgentRuntime::start() immediately before prepareAll(); a debug-only
// assert in registerAgent() catches violations early. No mutex: the registry is
// frozen-at-startup by contract, and adding one would be over-engineering.
class AgentRegistry
{
public:
    AgentRegistry() = default;
    ~AgentRegistry();

    // Takes ownership. Returns true if registered, false if the role is already taken.
    // Pre-seal only — see class doc.
    bool registerAgent(std::unique_ptr<IAgent> agent);

    // H-1 FIX: O(1) enum-indexed array lookup rather than O(n) linear scan.
    IAgent* find(AgentRole role) const noexcept;
    std::vector<AgentRole> registeredRoles() const noexcept;

    // Wires the shared AgentContext into every registered agent.
    void prepareAll(const AgentContext& ctx);

    // Cooperative cancel + mark Stopped.
    void stopAll();

    // M3: freezes the registry against further registrations. Called by the
    // runtime immediately before workers start. Idempotent.
    void seal() noexcept { sealed_.store(true, std::memory_order_release); }

private:
    // H-1: maps AgentRole ordinal → agent pointer. O(1) find(). Role count
    // matches the enum range (Conductor=0 … Custom=7). Unused slots are null.
    std::array<std::unique_ptr<IAgent>, 8> agentsByRole_;

    // Preserves registration order for iteration in prepareAll/stopAll.
    struct Slot { AgentRole role = AgentRole::Custom; int ordinal = 0; };
    std::vector<Slot> order_;

    std::atomic<bool> sealed_{ false };

    static constexpr int ordinal(AgentRole r) noexcept { return static_cast<int>(r); }
};

} // namespace more_phi::agents
