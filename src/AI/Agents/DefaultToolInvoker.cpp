// src/AI/Agents/DefaultToolInvoker.cpp
#include "AI/Agents/DefaultToolInvoker.h"
#include "AI/Agents/AgentToolError.h"

#include <juce_core/juce_core.h>

#include <chrono>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }
} // namespace

DefaultToolInvoker::DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                                       int maxCallsPerAgentPerSecond,
                                       int dispatchTimeoutMs)
    : dispatch_(std::move(dispatch))
    , capability_(std::move(capability))
    , rateLimit_(maxCallsPerAgentPerSecond)
    , dispatchTimeoutMs_(dispatchTimeoutMs)
{
}

DefaultToolInvoker::~DefaultToolInvoker()
{
    // Drain in-flight (timed-out but still running) dispatches. Each std::future
    // produced by std::async blocks in its destructor until its task finishes,
    // so destroying the vector joins every background dispatch BEFORE the
    // member-declaration-order rule destroys dispatch_/capability_ (and their
    // by-ref captures, e.g. *MorePhiProcessor). This is the only correct place
    // to join — orphaning these would be a use-after-free. (AUDIT 3.1)
    std::vector<std::future<juce::String>> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(inFlightFutures_);
    }
    // Destroyed here → each future's destructor blocks until its task completes.
    // No lock held while blocking (avoids holding mutex_ across a long join).
    drained.clear();
}

bool DefaultToolInvoker::consumeRateSlotLocked(const juce::String& agentId)
{
    if (rateLimit_ <= 0)
        return true;
    const auto t = nowMs();
    // M-1 FIX: Evict expired buckets every kEvictionIntervalMs (10s) instead of
    // scanning all buckets on every invocation. For a typical 7-agent system the
    // map is tiny, but under adversarial conditions (thousands of spoofed agent ids
    // hitting the dispatch) the per-call scan would be O(n) in bucket count.
    if ((t - lastEvictionMs_) >= kEvictionIntervalMs)
    {
        for (auto it = buckets_.begin(); it != buckets_.end(); )
        {
            if (it->second.windowStartMs != 0 && (t - it->second.windowStartMs) >= 1000)
                it = buckets_.erase(it);
            else
                ++it;
        }
        lastEvictionMs_ = t;
    }
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
    //
    // AUDIT 3.1 (2026-06-30): when dispatchTimeoutMs_ > 0, run dispatch_ on a
    // std::async worker and bound it with wait_for(). Without this, a single
    // blocking tool (hosted_plugin.scan, a hung run_self_test, IPC) monopolized
    // a scheduler worker indefinitely — only the chat path's 180s loop wall-clock
    // would eventually trip, surfacing a misleading "loop timed out" that blamed
    // the loop rather than the tool. A timed-out dispatch is NOT cancelled (C++
    // std::async has no cancel) — it keeps running in the background and is
    // retained in inFlightFutures_ so the destructor joins it before the captured
    // callables die. dispatchTimeoutMs_ == 0 keeps the legacy synchronous path.
    juce::String raw;
    try
    {
        if (dispatchTimeoutMs_ <= 0)
        {
            raw = dispatch_(toolName, params);
        }
        else
        {
            // Copy toolName/params by value into the async task: toolName is a
            // juce::String (refcounted, cheap to copy); params is moved into the
            // lambda so the async thread owns its own argument snapshot. dispatch_
            // itself is a member (captured by ref) — safe because the destructor
            // joins all in-flight futures before dispatch_ is destroyed.
            auto fut = std::async(std::launch::async,
                [this, toolName, paramsCopy = params]() {
                    return dispatch_(toolName, paramsCopy);
                });

            using namespace std::chrono;
            if (fut.wait_for(milliseconds(dispatchTimeoutMs_)) == std::future_status::ready)
            {
                raw = fut.get();   // rethrows any dispatch exception → caught below
            }
            else
            {
                // Timed out. Retain the still-running future so the destructor
                // joins it; reap any previously-timed-out futures that have since
                // finished to keep the vector bounded.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto it = inFlightFutures_.begin(); it != inFlightFutures_.end(); )
                    {
                        if (it->wait_for(milliseconds(0)) == std::future_status::ready)
                        {
                            try { (void) it->get(); } catch (...) {}
                            it = inFlightFutures_.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                    inFlightFutures_.push_back(std::move(fut));
                }
                return agentToolError("tool_timeout",
                    "Tool " + toolName + " did not complete within "
                    + juce::String(dispatchTimeoutMs_) + "ms budget");
            }
        }
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
