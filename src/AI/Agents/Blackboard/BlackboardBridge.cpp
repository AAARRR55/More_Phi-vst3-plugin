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
    const auto eventId = ev.eventId;
    bus_.publish(std::move(ev));   // publish stamps ev.sequence
    return eventId;
}

void BlackboardBridge::subscribe(const juce::String& agentId,
                                 const std::vector<juce::String>& eventTypes,
                                 Callback callback)
{
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    for (const auto& t : eventTypes)
        subscribers_[t.toStdString()].push_back({ agentId.toStdString(), std::move(callback) });
}

bool BlackboardBridge::hasNewEvents() const
{
    return bus_.lastSequence() > lastSeenSequence_;
}

namespace {
// H3: heuristics for "did an agent publish this event?" The agent layer is the
// only publisher of these type/source namespaces; non-agent bus traffic (action
// ledger, permission kernel) uses different type prefixes. Conservative: when in
// doubt, treat as non-agent and exclude.
bool isAgentEvent(const juce::String& type, const juce::String& source)
{
    static const std::vector<std::string> typePrefixes = {
        "agents.", "analysis.", "realtime.", "conductor.",
        "optimization.", "creative.", "quality.", "memory."
    };
    const auto t = type.toStdString();
    for (const auto& p : typePrefixes)
        if (t.rfind(p, 0) == 0)
            return true;

    // Agent sources are role-prefixed ids ("conductor-1", "realtime-1", ...).
    static const std::vector<std::string> sourcePrefixes = {
        "conductor", "analysis", "optimization", "creative",
        "realtime", "quality", "memory"
    };
    const auto s = source.toLowerCase().toStdString();
    for (const auto& p : sourcePrefixes)
        if (s.rfind(p, 0) == 0)
            return true;

    return false;
}
} // namespace

nlohmann::json BlackboardBridge::recentAgentEvents(int limit) const
{
    const int safeLimit = juce::jlimit(1, 256, limit <= 0 ? 32 : limit);
    auto recent = bus_.listRecent(safeLimit * 4);   // over-fetch, then filter
    if (! recent.is_array())
        return nlohmann::json::array();

    nlohmann::json out = nlohmann::json::array();
    for (const auto& ev : recent)
    {
        if (! ev.is_object())
            continue;
        const juce::String type   = juce::String(ev.value("type",   std::string{}));
        const juce::String source = juce::String(ev.value("source", std::string{}));
        if (! isAgentEvent(type, source))
            continue;
        // Safe summary: no raw payload. Callers who need detail use the dedicated,
        // permission-gated tool paths.
        out.push_back({
            { "type",   type.toStdString() },
            { "source", source.toStdString() },
            { "runId",  juce::String(ev.value("workflowRunId", std::string{})).toStdString() },
            { "sequence", ev.value("sequence", static_cast<uint64_t>(0)) }
        });
        if (static_cast<int>(out.size()) >= safeLimit)
            break;
    }
    return out;
}

void BlackboardBridge::poll()
{
    // C1 FIX: pull only the events strictly newer than our cursor, oldest-first.
    // The bus stamps each event with a gap-free monotonic sequence, so this is
    // exact regardless of how many events the bounded ring has evicted since the
    // last poll. A dropped count cursor (the old design) would re-deliver or skip
    // events under eviction; the sequence cursor cannot.
    const int window = 512;
    auto recent = bus_.listRecentSince(lastSeenSequence_, window);
    if (! recent.is_array() || recent.empty())
        return;

    for (const auto& ev : recent)
    {
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

        // Advance the cursor past this event. We read the field defensively;
        // any event lacking a sequence (e.g. a foreign IntegrationEvent not
        // routed through the same publish() path) is still consumed but does
        // not move the cursor — it will be re-considered next poll, which is
        // safe because it has no side effects we haven't already idempotently
        // guarded against via the rate/budget limiters in the reactive agents.
        const auto seq = ev.value("sequence", static_cast<uint64_t>(0));
        if (seq > lastSeenSequence_)
            lastSeenSequence_ = seq;
    }
}

} // namespace more_phi::agents
