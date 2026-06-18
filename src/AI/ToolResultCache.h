/*
 * More-Phi — AI/ToolResultCache.h
 * LRU cache for read-only/high-frequency MCP tool results.
 * Reduces latency for repeated calls such as get_plugin_info, list_parameters,
 * and analysis.get_summary while respecting plugin-generation invalidation.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <list>
#include <mutex>
#include <unordered_map>
#include <string>

namespace more_phi {

class ToolResultCache
{
public:
    explicit ToolResultCache(size_t maxEntries = 64);

    /** Look up a cached result. Returns std::nullopt if missing, expired,
     *  or generation token mismatch.
     */
    std::optional<nlohmann::json> get(const juce::String& toolName,
                                      const juce::var& params,
                                      uint64_t generationToken);

    /** Store a result. If an entry for the same key exists it is moved to
     *  the front (most-recently-used) and its TTL is refreshed.
     */
    void put(const juce::String& toolName,
             const juce::var& params,
             uint64_t generationToken,
             const nlohmann::json& result,
             std::chrono::seconds ttl = std::chrono::seconds(30));

    /** Invalidate all entries. Call when the hosted plugin changes or a
     *  full snapshot recall occurs.
     */
    void invalidateAll();

    /** Remove expired entries and enforce the maximum size limit. */
    void prune();

    struct Stats
    {
        size_t size = 0;
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
    };
    Stats getStats() const;

private:
    struct Entry
    {
        std::string key;
        nlohmann::json result;
        uint64_t generationToken = 0;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::string makeKey(const juce::String& toolName,
                        const juce::var& params,
                        uint64_t generationToken) const;

    size_t maxEntries_;
    mutable std::mutex mutex_;
    std::list<Entry> lru_;
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
    Stats stats_;
};

} // namespace more_phi
