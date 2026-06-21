/*
 * More-Phi — AI/Orchestrator/SecurityValidator.h
 * MCP message sanitization and security enforcement.
 * Thread-safe; designed for concurrent connection handler threads.
 */
#pragma once

#include "AI/Orchestrator/EcosystemConfig.h"
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <cstdint>

namespace more_phi::orchestrator {

//==============================================================================
/**
 * @class SecurityValidator
 * @brief Validates incoming MCP JSON-RPC requests, enforces rate limits,
 *        sanitizes parameters, and logs security events.
 *
 * All public methods are thread-safe. The validator is intended to be
 * constructed once per EcosystemConfig and shared across ConnectionThreads.
 */
class SecurityValidator
{
public:
    explicit SecurityValidator(const EcosystemConfig& config);
    ~SecurityValidator();

    // Disable copy/move; validator holds mutable state.
    SecurityValidator(const SecurityValidator&) = delete;
    SecurityValidator& operator=(const SecurityValidator&) = delete;
    SecurityValidator(SecurityValidator&&) = delete;
    SecurityValidator& operator=(SecurityValidator&&) = delete;

    //==========================================================================
    /**
     * Validate a parsed JSON-RPC request.
     *
     * Checks:
     *   - JSON depth <= security.maxJsonDepth
     *   - Serialized size <= security.maxJsonSize (approximate via dump)
     *   - Top-level fields are in the JSON-RPC 2.0 whitelist
     *   - Method is in the allowedMethods list
     *
     * @param j         Parsed JSON object.
     * @param outError  Human-readable error on failure.
     * @return true if the request passes all checks.
     */
    bool validateRequestJson(const nlohmann::json& j, juce::String& outError);

    /**
     * Constant-time bearer token comparison.
     *
     * Uses bitwise XOR accumulation to avoid timing side-channels.
     * Length mismatch is an immediate rejection (leaks only length info).
     */
    bool validateAuthToken(const juce::String& token,
                           const juce::String& expectedToken) const;

    /**
     * Per-client sliding-window rate limiting.
     *
     * Tracks request timestamps per clientId. Returns false if the client
     * has exceeded security.rateLimitPerMinute within the last 60 seconds.
     */
    bool checkRateLimit(const juce::String& clientId);

    /**
     * Sanitize the "params" object of a JSON-RPC request.
     *
     * Actions:
     *   - Remove keys longer than 64 characters
     *   - Truncate string values exceeding 4096 characters
     *   - Clamp numeric values to [-1e6, 1e6]
     *   - Enforce max nesting depth
     *
     * @param params    In/out: the params object to sanitize (modified in place).
     * @param outError  Human-readable error if a critical violation is found.
     * @return true if sanitization succeeded (params may have been modified).
     */
    bool sanitizeParams(nlohmann::json& params, juce::String& outError);

    /**
     * Extract a client identifier from an active TCP socket.
     *
     * Uses the connected host name; falls back to a pointer-based identifier
     * if the host name is unavailable.
     */
    juce::String getClientId(juce::StreamingSocket* socket) const;

    //==========================================================================
    /**
     * Log a security event to the JUCE logger.
     *
     * Format: "[SECURITY] <eventType> | client=<clientId> | <details>"
     */
    void logSecurityEvent(const juce::String& eventType,
                          const juce::String& clientId,
                          const juce::String& details) const;

    /** Total number of security events logged since construction. */
    int getTotalEventsLogged() const noexcept { return totalEventsLogged_.load(); }

private:
    struct RateLimitEntry
    {
        std::deque<juce::int64> timestamps;
        juce::int64 lastCleanupMs = 0;
    };

    EcosystemConfig config_;

    mutable std::mutex rateLimitMutex_;
    std::unordered_map<std::string, RateLimitEntry> rateLimitMap_;

    mutable std::atomic<int> totalEventsLogged_{0};

    //==========================================================================
    bool checkJsonDepth(const nlohmann::json& j, int currentDepth,
                        int maxDepth, juce::String& outError) const;
    bool validateMethod(const juce::String& method) const;
    bool isAllowedTopLevelField(const juce::String& field) const;
    void cleanupRateLimitEntry(RateLimitEntry& entry, juce::int64 windowStartMs);

    bool sanitizeValue(nlohmann::json& value, int depth,
                       juce::String& outError) const;
};

} // namespace more_phi::orchestrator
