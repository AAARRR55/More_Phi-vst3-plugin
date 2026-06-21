/*
 * More-Phi — AI/Orchestrator/SecurityValidator.cpp
 * MCP message sanitization, auth validation, and rate limiting.
 */
#include "AI/Orchestrator/SecurityValidator.h"
#include <juce_core/juce_core.h>

namespace more_phi::orchestrator {

//==============================================================================
SecurityValidator::SecurityValidator(const EcosystemConfig& config)
    : config_(config)
{
}

SecurityValidator::~SecurityValidator() = default;

//==============================================================================
bool SecurityValidator::validateRequestJson(const nlohmann::json& j, juce::String& outError)
{
    // 1. Approximate size check (post-parse; caller should also gate raw bytes)
    const std::size_t approxSize = j.dump().size();
    if (approxSize > static_cast<std::size_t>(config_.security.maxJsonSize))
    {
        outError = "Request JSON exceeds max size (" + juce::String(approxSize)
                 + " > " + juce::String(config_.security.maxJsonSize) + ")";
        return false;
    }

    // 2. Depth check
    if (!checkJsonDepth(j, 1, config_.security.maxJsonDepth, outError))
        return false;

    // 3. Top-level field whitelist (JSON-RPC 2.0 + MCP extensions)
    if (!j.is_object())
    {
        outError = "Request JSON must be an object at the top level";
        return false;
    }

    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const juce::String key = juce::String(it.key());
        if (!isAllowedTopLevelField(key))
        {
            outError = "Disallowed top-level field: '" + key + "'";
            return false;
        }
    }

    // 4. Method whitelist
    if (j.contains("method"))
    {
        const auto& methodVal = j.at("method");
        if (!methodVal.is_string())
        {
            outError = "Field 'method' must be a string";
            return false;
        }
        if (!validateMethod(juce::String(methodVal.get<std::string>())))
        {
            outError = "Method not in allowed list";
            return false;
        }
    }

    return true;
}

//==============================================================================
bool SecurityValidator::validateAuthToken(const juce::String& token,
                                            const juce::String& expectedToken) const
{
    const int lenA = token.length();
    const int lenB = expectedToken.length();

    if (lenA != lenB)
        return false;

    if (lenA == 0)
        return false;

    int diff = 0;
    for (int i = 0; i < lenA; ++i)
    {
        // XOR each character; accumulate into diff so the compiler cannot
        // short-circuit the loop on the first mismatch.
        diff |= (static_cast<int>(token[i]) ^ static_cast<int>(expectedToken[i]));
    }

    return diff == 0;
}

//==============================================================================
bool SecurityValidator::checkRateLimit(const juce::String& clientId)
{
    const juce::int64 nowMs = juce::Time::currentTimeMillis();
    const juce::int64 windowStartMs = nowMs - 60000; // 60-second sliding window

    const std::string key = clientId.toStdString();

    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    auto it = rateLimitMap_.find(key);
    if (it == rateLimitMap_.end())
    {
        RateLimitEntry entry;
        entry.timestamps.push_back(nowMs);
        rateLimitMap_[key] = std::move(entry);
        return true;
    }

    RateLimitEntry& entry = it->second;
    cleanupRateLimitEntry(entry, windowStartMs);

    if (static_cast<int>(entry.timestamps.size()) >= config_.security.rateLimitPerMinute)
    {
        logSecurityEvent("RATE_LIMIT_EXCEEDED", clientId,
                         "Requests in window: " + juce::String(entry.timestamps.size()));
        return false;
    }

    entry.timestamps.push_back(nowMs);
    return true;
}

//==============================================================================
bool SecurityValidator::sanitizeParams(nlohmann::json& params, juce::String& outError)
{
    if (!params.is_object())
    {
        // Non-object params are allowed (e.g., arrays, primitives) but we don't recurse into them.
        return true;
    }

    for (auto it = params.begin(); it != params.end();)
    {
        const juce::String key = juce::String(it.key());

        // Remove keys that are too long (potential injection / DoS)
        if (key.length() > 64)
        {
            logSecurityEvent("SANITIZE_KEY_REMOVED", "internal", "Key too long: " + key);
            it = params.erase(it);
            continue;
        }

        if (!sanitizeValue(it.value(), 1, outError))
            return false;

        ++it;
    }

    return true;
}

//==============================================================================
juce::String SecurityValidator::getClientId(juce::StreamingSocket* socket) const
{
    if (socket == nullptr)
        return "null_socket";

    // JUCE provides the connected host name.
    juce::String host = socket->getHostName();
    if (host.isEmpty())
    {
        // Fallback: pointer-based identifier so every connection is unique
        // even if the hostname is unavailable.
        host = "unknown_" + juce::String(reinterpret_cast<std::uintptr_t>(socket));
    }
    return host;
}

//==============================================================================
void SecurityValidator::logSecurityEvent(const juce::String& eventType,
                                          const juce::String& clientId,
                                          const juce::String& details) const
{
    const juce::String msg = "[SECURITY] " + eventType
                           + " | client=" + clientId
                           + " | " + details;
    juce::Logger::writeToLog(msg);
    totalEventsLogged_.fetch_add(1, std::memory_order_relaxed);
}

//==============================================================================
// Private helpers
//==============================================================================
bool SecurityValidator::checkJsonDepth(const nlohmann::json& j, int currentDepth,
                                       int maxDepth, juce::String& outError) const
{
    if (currentDepth > maxDepth)
    {
        outError = "JSON depth exceeds maximum (" + juce::String(maxDepth) + ")";
        return false;
    }

    if (j.is_object() || j.is_array())
    {
        for (const auto& item : j)
        {
            if (!checkJsonDepth(item, currentDepth + 1, maxDepth, outError))
                return false;
        }
    }
    return true;
}

bool SecurityValidator::validateMethod(const juce::String& method) const
{
    for (const auto& allowed : config_.security.allowedMethods)
    {
        if (allowed == method)
            return true;
    }
    return false;
}

bool SecurityValidator::isAllowedTopLevelField(const juce::String& field) const
{
    static const std::vector<std::string> kAllowed = {
        "jsonrpc", "method", "params", "id", "result", "error"
    };

    const std::string f = field.toStdString();
    for (const auto& a : kAllowed)
    {
        if (a == f)
            return true;
    }
    return false;
}

void SecurityValidator::cleanupRateLimitEntry(RateLimitEntry& entry, juce::int64 windowStartMs)
{
    // Throttle cleanup: only scan if at least 1 second has passed since last sweep.
    if (entry.lastCleanupMs != 0 && (juce::Time::currentTimeMillis() - entry.lastCleanupMs) < 1000)
        return;

    while (!entry.timestamps.empty() && entry.timestamps.front() < windowStartMs)
        entry.timestamps.pop_front();

    entry.lastCleanupMs = juce::Time::currentTimeMillis();
}

bool SecurityValidator::sanitizeValue(nlohmann::json& value, int depth,
                                      juce::String& outError) const
{
    if (depth > config_.security.maxJsonDepth)
    {
        outError = "Param nesting exceeds max depth during sanitization";
        return false;
    }

    if (value.is_string())
    {
        std::string s = value.get<std::string>();
        if (s.length() > 4096)
        {
            logSecurityEvent("SANITIZE_STRING_TRUNCATED", "internal",
                             "Length " + juce::String(s.length()));
            s.resize(4096);
            value = s;
        }
        return true;
    }

    if (value.is_number())
    {
        const double d = value.get<double>();
        if (std::isnan(d) || std::isinf(d))
        {
            outError = "Numeric parameter contains NaN or Infinity";
            return false;
        }
        if (d < -1e6 || d > 1e6)
        {
            logSecurityEvent("SANITIZE_NUMERIC_CLAMPED", "internal",
                             juce::String(d));
            value = juce::jlimit(-1e6, 1e6, d);
        }
        return true;
    }

    if (value.is_object())
    {
        for (auto it = value.begin(); it != value.end();)
        {
            const juce::String key = juce::String(it.key());
            if (key.length() > 64)
            {
                logSecurityEvent("SANITIZE_KEY_REMOVED", "internal", "Nested key too long: " + key);
                it = value.erase(it);
                continue;
            }
            if (!sanitizeValue(it.value(), depth + 1, outError))
                return false;
            ++it;
        }
        return true;
    }

    if (value.is_array())
    {
        for (auto& item : value)
        {
            if (!sanitizeValue(item, depth + 1, outError))
                return false;
        }
        return true;
    }

    // null, boolean — allowed as-is
    return true;
}

} // namespace more_phi::orchestrator
