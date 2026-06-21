// src/AI/Agents/Blackboard/BlackboardBridge.cpp
#include "AI/Agents/Blackboard/BlackboardBridge.h"

#include "AI/AutomationControlPlane.h"

namespace more_phi::agents {

BlackboardBridge::BlackboardBridge(IntegrationEventBus& bus) : bus_(bus) {}

juce::String BlackboardBridge::publish(const juce::String& source,
                                       const juce::String& type,
                                       nlohmann::json payload,
                                       const juce::String& runId)
{
    more_phi::IntegrationEvent ev;
    ev.eventId = more_phi::makeAutomationId("evt");
    ev.source  = source;
    ev.type    = type;
    ev.workflowRunId = runId;
    ev.payload = std::move(payload);
    ev.timestamp = juce::Time::getCurrentTime();
    bus_.publish(std::move(ev));
    return {};
}

void BlackboardBridge::subscribe(const juce::String& agentId,
                                 const std::vector<juce::String>& eventTypes,
                                 Callback callback)
{
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    for (const auto& t : eventTypes)
        subscribers_[t.toStdString()].push_back({ agentId.toStdString(), std::move(callback) });
}

void BlackboardBridge::poll()
{
    // listRecent returns newest-first; we process oldest-unseen to newest so ordering
    // is causal. lastSeenCount_ tracks how many we've consumed within this window.
    const int window = 512;
    auto recent = bus_.listRecent(window);
    if (! recent.is_array())
        return;
    const int totalSeen = static_cast<int>(recent.size());
    int startIdx = lastSeenCount_;
    if (startIdx > totalSeen)
        startIdx = totalSeen;

    // recent[0] is newest; recent[totalSeen-1] is oldest. Iterate tail->head skipping
    // the `startIdx` we already processed.
    for (int i = totalSeen - 1 - startIdx; i >= 0; --i)
    {
        const auto& ev = recent[i];
        if (! ev.is_object())
            continue;
        const juce::String type   = juce::String(ev.value("type",   std::string{}));
        const juce::String source = juce::String(ev.value("source", std::string{}));
        const nlohmann::json payload = ev.value("payload", nlohmann::json::object());

        std::vector<std::pair<std::string, Callback>> matches;
        {
            std::lock_guard<std::mutex> lock(subscribersMutex_);
            auto it = subscribers_.find(type.toStdString());
            if (it != subscribers_.end())
                matches = it->second;  // copy under lock
        }
        for (const auto& [agentId, cb] : matches)
        {
            try { cb(type, payload, source); }
            catch (...) { /* a subscriber fault must not break the pump */ }
        }
    }
    lastSeenCount_ = totalSeen;
}

} // namespace more_phi::agents
