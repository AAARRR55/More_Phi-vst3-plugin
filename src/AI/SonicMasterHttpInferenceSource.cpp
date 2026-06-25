/*
 * More-Phi — AI/SonicMasterHttpInferenceSource.cpp
 */
#include "AI/SonicMasterHttpInferenceSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

namespace more_phi {

using json = nlohmann::json;

// ── Pure helpers ─────────────────────────────────────────────────────────────

void buildInferRequestBody(const float* left,
                           const float* right,
                           std::size_t n,
                           float* outInterleaved) noexcept
{
    if (left == nullptr || right == nullptr || outInterleaved == nullptr) return;
    for (std::size_t i = 0; i < n; ++i)
    {
        outInterleaved[2 * i + 0] = left[i];
        outInterleaved[2 * i + 1] = right[i];
    }
}

bool parseInferResponse(std::string_view jsonBody,
                        float* outDecision,
                        std::size_t outCapacity) noexcept
{
    if (outDecision == nullptr || outCapacity == 0) return false;
    const std::size_t want = std::min(outCapacity, kSonicMasterDecisionWidth);
    // P2.1: use the project's existing nlohmann/json (already linked to MorePhi)
    // instead of the hand-rolled strtod parser — json.org-adherent, locale-safe,
    // and handles all edge cases (scientific notation, NaN, inf, accidental
    // whitespace inside the array).
    try
    {
        const auto parsed = json::parse(jsonBody);
        const auto it = parsed.find("decision");
        if (it == parsed.end() || !it->is_array())
            return false;
        if (it->size() < want)
            return false;
        for (std::size_t i = 0; i < want; ++i)
            outDecision[i] = (*it)[i].get<float>();
        return true;
    }
    catch (const json::parse_error&)
    {
        return false;
    }
}

// ── HTTP source ──────────────────────────────────────────────────────────────

void SonicMasterHttpInferenceSource::setEndpoint(std::string baseUrl, int port)
{
    baseUrl_ = std::move(baseUrl);
    port_ = port;
}

void SonicMasterHttpInferenceSource::invalidateAvailability() const noexcept
{
    probeFresh_.store(false, std::memory_order_release);
    lastProbeMs_.store(0, std::memory_order_release);
}

bool SonicMasterHttpInferenceSource::isAvailable() const noexcept
{
    // ponytail: pure cache read. MUST NOT do blocking I/O — this is called from
    // the editor timer on the message thread, and a blocking /health probe to a
    // dead server stalls the whole UI for ~2s every time the 5s cache expires.
    // The probe is refreshed by refreshProbe() on a background thread.
    return cachedAvailable_;
}

void SonicMasterHttpInferenceSource::refreshProbe() noexcept
{
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch().count() / 1000000; // ms
    const int64_t last = lastProbeMs_.load(std::memory_order_acquire);
    if (probeFresh_.load(std::memory_order_acquire) && (now - last) < 5000)
        return;

    // Probe /health. Blocking — call from a background thread only.
    bool ok = false;
    try
    {
        juce::URL url(baseUrl_ + ":" + std::to_string(port_) + "/health");
        const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                              .withConnectionTimeoutMs(800);
        if (auto stream = url.createInputStream(opts))
        {
            stream->readEntireStreamAsString();
            ok = true;
        }
    }
    catch (...) { ok = false; }

    cachedAvailable_.store(ok, std::memory_order_release);
    probeFresh_.store(true, std::memory_order_release);
    lastProbeMs_.store(now, std::memory_order_release);
}

bool SonicMasterHttpInferenceSource::infer(const float* stereoInterleaved,
                                           float* outDecision,
                                           std::size_t outCapacity) noexcept
{
    if (stereoInterleaved == nullptr || outDecision == nullptr
        || outCapacity < kSonicMasterDecisionWidth)
        return false;

    // FIX-1.2: hierarchical timeouts — connection 2s, read 5s. The old
    // single 10s timeout meant a slow connect and a hanging read shared the
    // same budget, stalling the analysis loop for the full 10s on either.

    // A3 FIX (UAF): the std::async thread below previously captured url/body/opts
    // by REFERENCE. On the 5s-timeout path (wait_for != ready), infer() returns
    // and destroys those locals while the detached async thread keeps running and
    // dereferences them (url.withPOSTData(body)) — a classic dangling-reference
    // use-after-free triggered whenever the server accepts the connection but
    // never responds within 5s. Build the request objects INSIDE the async thread
    // so they live exactly as long as the thread does. The locals here are now
    // plain values copied in (cheap: baseUrl_/port_/targetLufs_ are small scalars).
    const std::string baseUrlCopy = baseUrl_;
    const int portCopy = port_;
    const float targetLufsCopy = targetLufs_.load(std::memory_order_relaxed);
    // Copy the request body into a heap-owned block the async thread owns.
    auto bodyHolder = std::make_shared<juce::MemoryBlock>(
        stereoInterleaved, 2 * kSonicMasterSegmentFrames * sizeof(float));

    // Use an async read with a 5-second deadline so a hanging server (accepts
    // POST but never responds) stalls the analysis loop for at most ~5s,
    // not the old 10s (which was the connect timeout only, plus unbounded read).
    auto future = std::async(std::launch::async, [baseUrlCopy, portCopy, targetLufsCopy, bodyHolder]() -> std::string {
        try
        {
            juce::URL url(baseUrlCopy + ":" + std::to_string(portCopy)
                          + "/infer?target_lufs=" + juce::String(targetLufsCopy, 3).toStdString());
            const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                  .withConnectionTimeoutMs(2000);
            auto stream = url.withPOSTData(*bodyHolder).createInputStream(opts);
            if (stream == nullptr)
                return {};
            return stream->readEntireStreamAsString().toStdString();
        }
        catch (...)
        {
            return {};
        }
    });

    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        return false;   // A3: safe now — bodyHolder + locals are owned by the async thread, not this frame

    std::string resp;
    try { resp = future.get(); } catch (...) { return false; }
    if (resp.empty())
        return false;

    return parseInferResponse(resp, outDecision, outCapacity);
}

} // namespace more_phi
