// src/AI/Agents/Tooling/DefaultToolInvoker.h
#pragma once

#include "AI/Agents/AgentContext.h"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace more_phi::agents {

// Default IToolInvoker: wraps a dispatch callback (in production: MCPToolHandler::handle)
// and enforces per-agent capability scope + rate budget + attribution (Decision D1).
class DefaultToolInvoker : public IToolInvoker
{
public:
    using DispatchFn     = std::function<juce::String(const juce::String& method,
                                                       const nlohmann::json& params)>;
    using CapabilityFn   = std::function<std::vector<juce::String>(const juce::String& agentId)>;

    // maxCallsPerAgentPerSecond == 0 means unlimited.
    DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                       int maxCallsPerAgentPerSecond = 0);
    ~DefaultToolInvoker() override = default;

    nlohmann::json invoke(const juce::String& toolName,
                          const nlohmann::json& params,
                          const juce::String& agentId) override;

private:
    bool consumeRateSlotLocked(const juce::String& agentId);

    DispatchFn   dispatch_;
    CapabilityFn capability_;
    int          rateLimit_;

    struct RateBucket
    {
        juce::int64 windowStartMs = 0;
        int count = 0;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> buckets_;
    // M-1 FIX: Throttle bucket eviction to once every 10s instead of scanning
    // all buckets on every rate check. Avoids O(n) scan per tool invoke.
    juce::int64 lastEvictionMs_ = 0;
    static constexpr juce::int64 kEvictionIntervalMs = 10000;
};

} // namespace more_phi::agents
