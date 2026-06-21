/*
 * More-Phi — AI/SonicMasterHttpInferenceSource.cpp
 */
#include "AI/SonicMasterHttpInferenceSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <juce_core/juce_core.h>

namespace more_phi {

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

namespace {

// Locate the "decision" array in a /infer JSON response and parse its float
// members. Hand-rolled because the response is a fixed, tiny shape and we want
// the parser to be pure (testable) and dependency-light.
bool extractDecisionArray(std::string_view body, float* out, std::size_t cap) noexcept
{
    const auto key = body.find("\"decision\"");
    if (key == std::string_view::npos) return false;
    const auto open = body.find('[', key);
    if (open == std::string_view::npos) return false;
    const auto close = body.find(']', open);
    if (close == std::string_view::npos) return false;

    std::size_t count = 0;
    std::size_t i = open + 1;
    while (i < close && count < cap)
    {
        // Skip separators / whitespace.
        while (i < close && (body[i] == ',' || body[i] == ' ' || body[i] == '\n'
                             || body[i] == '\r' || body[i] == '\t'))
            ++i;
        if (i >= close) break;

        char* endptr = nullptr;
        // std::from_chars would be ideal but MSVC float support is uneven;
        // strtod on a null-terminated scratch buffer is portable and fine here.
        const std::size_t tokLen = std::min(close - i, std::size_t { 63 });
        char buf[64];
        std::memcpy(buf, body.data() + i, tokLen);
        buf[tokLen] = '\0';
        const double v = std::strtod(buf, &endptr);
        if (endptr == buf)
            return false;  // no parse progress -> malformed
        out[count++] = static_cast<float>(v);
        i += static_cast<std::size_t>(endptr - buf);
    }
    return count == cap;
}

} // namespace

bool parseInferResponse(std::string_view jsonBody,
                        float* outDecision,
                        std::size_t outCapacity) noexcept
{
    if (outDecision == nullptr || outCapacity == 0) return false;
    // Clamp to the model's fixed decision width so a larger/malformed response
    // can't overrun the caller's buffer.
    const std::size_t want = std::min(outCapacity, kSonicMasterDecisionWidth);
    return extractDecisionArray(jsonBody, outDecision, want);
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
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch().count() / 1000000; // ms
    const int64_t last = lastProbeMs_.load(std::memory_order_acquire);
    if (probeFresh_.load(std::memory_order_acquire) && (now - last) < 5000)
        return cachedAvailable_;

    // Probe /health. Blocking but cheap (localhost, empty body). Wrapped so a
    // missing/unreachable server reports false rather than throwing.
    bool ok = false;
    try
    {
        juce::URL url(baseUrl_ + ":" + std::to_string(port_) + "/health");
        const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                              .withConnectionTimeoutMs(800);
        if (auto stream = url.createInputStream(opts))
        {
            stream->readEntireStreamAsString();  // drain; we only care it connected
            ok = true;
        }
    }
    catch (...) { ok = false; }

    cachedAvailable_ = ok;
    probeFresh_.store(true, std::memory_order_release);
    lastProbeMs_.store(now, std::memory_order_release);
    return ok;
}

bool SonicMasterHttpInferenceSource::infer(const float* stereoInterleaved,
                                           float* outDecision,
                                           std::size_t outCapacity) noexcept
{
    if (stereoInterleaved == nullptr || outDecision == nullptr
        || outCapacity < kSonicMasterDecisionWidth)
        return false;

    // The body is the raw little-endian float32 interleaved stereo window
    // (2 * kSonicMasterSegmentFrames * 4 bytes). Copy into a MemoryBlock.
    juce::MemoryBlock body(stereoInterleaved, 2 * kSonicMasterSegmentFrames * sizeof(float));

    juce::URL url(baseUrl_ + ":" + std::to_string(port_)
                  + "/infer?target_lufs=" + juce::String(targetLufs_, 3).toStdString());
    const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                          .withConnectionTimeoutMs(10000);

    std::string resp;
    try
    {
        auto stream = url.withPOSTData(body).createInputStream(opts);
        if (stream == nullptr)
            return false;
        resp = stream->readEntireStreamAsString().toStdString();
    }
    catch (...)
    {
        return false;
    }

    return parseInferResponse(resp, outDecision, outCapacity);
}

} // namespace more_phi
