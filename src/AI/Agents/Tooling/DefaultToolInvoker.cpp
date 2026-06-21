// src/AI/Agents/Tooling/DefaultToolInvoker.cpp
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Tooling/AgentToolError.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }
} // namespace

DefaultToolInvoker::DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                                       int maxCallsPerAgentPerSecond)
    : dispatch_(std::move(dispatch))
    , capability_(std::move(capability))
    , rateLimit_(maxCallsPerAgentPerSecond)
{
}

bool DefaultToolInvoker::consumeRateSlotLocked(const juce::String& agentId)
{
    if (rateLimit_ <= 0)
        return true;
    const auto t = nowMs();
    auto& bucket = buckets_[agentId.toStdString()];
    if (bucket.windowStartMs == 0 || (t - bucket.windowStartMs) >= 1000)
    {
        bucket.windowStartMs = t;
        bucket.count = 0;
    }
    if (bucket.count >= rateLimit_)
        return false;
    ++bucket.count;
    return true;
}

nlohmann::json DefaultToolInvoker::invoke(const juce::String& toolName,
                                          const nlohmann::json& params,
                                          const juce::String& agentId)
{
    // 1. Capability scope check (D1a). Empty allowed list = fail closed.
    const auto allowed = capability_(agentId);
    bool permitted = false;
    for (const auto& a : allowed)
        if (a == toolName) { permitted = true; break; }
    if (! permitted)
        return agentToolError("capability_denied",
            "Agent " + agentId + " is not permitted to call " + toolName);

    // 2. Per-agent rate budget (D1b).
    bool rateOk;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rateOk = consumeRateSlotLocked(agentId);
    }
    if (! rateOk)
        return agentToolError("rate_limited",
            "Agent " + agentId + " exceeded its tool-call rate budget");

    // 3. Dispatch through the chokepoint (D1: MCPToolHandler::handle in production).
    juce::String raw;
    try
    {
        raw = dispatch_(toolName, params);
    }
    catch (const std::exception& e)
    {
        return agentToolError("dispatch_exception", juce::String(e.what()));
    }
    catch (...)
    {
        return agentToolError("dispatch_exception", "unknown dispatch exception");
    }

    // 4. Parse the JSON-string result.
    try
    {
        return nlohmann::json::parse(raw.toStdString());
    }
    catch (...)
    {
        return nlohmann::json::object({ { "raw", raw.toStdString() } });
    }
}

} // namespace more_phi::agents
