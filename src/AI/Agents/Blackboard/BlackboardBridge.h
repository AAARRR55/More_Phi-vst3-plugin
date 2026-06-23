// src/AI/Agents/Blackboard/BlackboardBridge.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace more_phi {
class IntegrationEventBus;
} // namespace more_phi

namespace more_phi::agents {

// Typed pub/sub OVER the existing IntegrationEventBus. Does not modify it.
// poll() must be called on a scheduler/message thread to fan out events to subscribers.
//
// C1 FIX: cursoring is by monotonic event sequence (IntegrationEvent::sequence),
// not by count. The bus is a bounded ring that evicts old entries, so a count
// cursor would either skip new events (ring shrunk) or re-deliver old ones
// (ring transiently under-filled). The sequence cursor is gap-free and never
// recycles, so neither failure can occur.
class BlackboardBridge
{
public:
    using Callback = std::function<void(const juce::String& type,
                                        const nlohmann::json& payload,
                                        const juce::String& source)>;

    explicit BlackboardBridge(IntegrationEventBus& bus);

    // Publish forwards the event into the bus so listRecent/exportState keep working.
    // Returns the generated eventId.
    juce::String publish(const juce::String& source,
                         const juce::String& type,
                         nlohmann::json payload,
                         const juce::String& runId = {});

    void subscribe(const juce::String& agentId,
                   const std::vector<juce::String>& eventTypes,
                   Callback callback);

    // Cheap non-pulling probe: returns true if the bus has published any event
    // past our cursor since the last poll(). Lets the pump sleep idly (M3).
    bool hasNewEvents() const;

    // Drain new events since the last poll and fan out to matching subscribers.
    void poll();

    // H3: curated, agent-only recent events (newest-first), with payloads replaced
    // by a safe summary. Filters the raw bus down to events whose source/type look
    // agent-originated, so an MCP consumer can observe agent activity without seeing
    // non-agent bus traffic (action-ledger artifacts, permission decisions, etc.).
    nlohmann::json recentAgentEvents(int limit = 32) const;

private:
    IntegrationEventBus& bus_;
    std::mutex subscribersMutex_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Callback>>> subscribers_;
    uint64_t lastSeenSequence_ = 0;   // monotonic cursor into IntegrationEventBus sequences
};

} // namespace more_phi::agents
