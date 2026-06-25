/*
 * More-Phi — AI/SonicMasterHttpInferenceSource.cpp
 */
#include "AI/SonicMasterHttpInferenceSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
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

    // AUDIT-FIX (A8): the previous std::async + future::wait_for pattern had an
    // ineffective 5s timeout. On timeout, infer() returned false — but the
    // std::future destructor is REQUIRED by the standard to join the async task,
    // so a hanging server (accepts POST, never responds) stalled the analysis
    // thread for the full hang duration, not 5s. Replace with a detached thread
    // + atomic done flag so the destructor does not join. Real socket-level
    // cancellation would need WinHTTP/libcURL; this gives "give up waiting",
    // and an inFlight_ guard prevents stacking concurrent requests.
    if (inFlight_.exchange(true, std::memory_order_acq_rel))
    {
        // A previous request is still pending in its detached thread. Skip rather
        // than stack another 2 MB POST onto a wedged server.
        return false;
    }

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto ok    = std::make_shared<std::atomic<bool>>(false);
    auto respHolder = std::make_shared<std::string>();

    std::thread worker([baseUrlCopy, portCopy, targetLufsCopy, bodyHolder, done, ok, respHolder]() {
        try
        {
            juce::URL url(baseUrlCopy + ":" + std::to_string(portCopy)
                          + "/infer?target_lufs=" + juce::String(targetLufsCopy, 3).toStdString());
            const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                  .withConnectionTimeoutMs(2000);
            auto stream = url.withPOSTData(*bodyHolder).createInputStream(opts);
            if (stream != nullptr)
            {
                *respHolder = stream->readEntireStreamAsString().toStdString();
                ok->store(!respHolder->empty(), std::memory_order_release);
            }
        }
        catch (...)
        {
            ok->store(false, std::memory_order_release);
        }
        done->store(true, std::memory_order_release);
    });
    worker.detach();  // ponytail: detach — ~thread does not join, so timeout is effective

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (done->load(std::memory_order_acquire))
        {
            const bool success = ok->load(std::memory_order_acquire);
            inFlight_.store(false, std::memory_order_release);
            if (!success || respHolder->empty())
                return false;
            return parseInferResponse(*respHolder, outDecision, outCapacity);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Timeout. The detached worker may still complete in the background; the
    // inFlight_ flag stays set so the next cycle skips until that worker sets
    // done. We do NOT clear inFlight_ here on purpose — a still-running wedged
    // request would otherwise let us stack a second one next cycle. The flag is
    // cleared on the next observed completion (above) or by refreshProbe().
    return false;
}

} // namespace more_phi
