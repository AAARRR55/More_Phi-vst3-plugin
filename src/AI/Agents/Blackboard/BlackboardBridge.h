// src/AI/Agents/Blackboard/BlackboardBridge.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

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

    // Drain new events since the last poll and fan out to matching subscribers.
    void poll();

private:
    IntegrationEventBus& bus_;
    std::mutex subscribersMutex_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Callback>>> subscribers_;
    int lastSeenCount_ = 0;   // how many events we have already processed within the window
};

} // namespace more_phi::agents
