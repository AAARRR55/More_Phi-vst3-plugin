// src/AI/Agents/DefaultToolInvoker.h
#pragma once

#include "AI/Agents/AgentContext.h"

#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>
#include <vector>

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
    // dispatchTimeoutMs == 0 means no per-tool timeout (legacy synchronous
    // behavior). > 0 runs dispatch_ on a std::async worker and returns a
    // tool_timeout error if it does not complete within the budget. A timed-out
    // dispatch keeps running in the background and is joined at shutdown (see
    // inFlightFutures_ below) — it is NEVER orphaned, because the production
    // dispatch_ captures *this/refs by reference and outliving the owner would
    // be a use-after-free. (AUDIT 3.1, 2026-06-30)
    DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                       int maxCallsPerAgentPerSecond = 0,
                       int dispatchTimeoutMs = 0);
    // Defined out-of-line: must drain inFlightFutures_ (each std::async future
    // blocks in its destructor until the async task finishes) so no background
    // dispatch outlives the dispatch_/capability_ callables or their captures.
    ~DefaultToolInvoker() override;

    nlohmann::json invoke(const juce::String& toolName,
                          const nlohmann::json& params,
                          const juce::String& agentId) override;

private:
    bool consumeRateSlotLocked(const juce::String& agentId);

    // Member declaration ORDER is load-bearing for shutdown safety:
    //   - dispatch_ / capability_ capture references (e.g. *MorePhiProcessor) in
    //     production. A timed-out dispatch may still be executing on its async
    //     thread when destruction begins.
    //   - std::future from std::async BLOCKS in its destructor until the task
    //     finishes. So inFlightFutures_ must be declared AFTER dispatch_/
    //     capability_, ensuring the vector (and its blocking future dtors) is
    //     destroyed FIRST — draining every in-flight dispatch BEFORE the captured
    //     callables (and their by-ref captures) are torn down.
    DispatchFn   dispatch_;
    CapabilityFn capability_;
    int          rateLimit_;
    int          dispatchTimeoutMs_ = 0;

    std::mutex mutex_;
    // Timed-out-but-still-running dispatches, kept so the destructor can join
    // them. Guarded by mutex_. Pruned (ready futures reaped) on each invoke().
    std::vector<std::future<juce::String>> inFlightFutures_;

    struct RateBucket
    {
        juce::int64 windowStartMs = 0;
        int count = 0;
    };
    std::unordered_map<std::string, RateBucket> buckets_;
    // M-1 FIX: Throttle bucket eviction to once every 10s instead of scanning
    // all buckets on every rate check. Avoids O(n) scan per tool invoke.
    juce::int64 lastEvictionMs_ = 0;
    static constexpr juce::int64 kEvictionIntervalMs = 10000;
};

} // namespace more_phi::agents
