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
    /** Invalidation scope. Each cache entry belongs to exactly one scope so a
     *  write can evict only the scope(s) it actually dirties instead of the
     *  whole cache (spec §6.2). Ordering matters: the broader scopes are used
     *  by invalidateAll() only; scoped invalidation targets a single tag. */
    enum class Scope : uint8_t
    {
        Parameters,   ///< list_parameters, get_parameter, hosted_plugin.parameters, more_phi.parameters, diagnose_parameter_pipeline
        Analysis,     ///< analysis.* (meters/spectrum/stereo field)
        Morph,        ///< get_morph_state
        Profile,      ///< plugin_profile.describe_semantics / semantic_map
        Control,      ///< automation.history/get_transaction, permission.*, workflow.list, memory.*, context.*, events.*
        Instance,     ///< get_instance_info, list_instances
        PluginInfo     ///< get_plugin_info, hosted_plugin.info
    };

    explicit ToolResultCache(size_t maxEntries = 64);

    /** Look up a cached result. Returns std::nullopt if missing, expired,
     *  generation token mismatch, or (when @p instanceId is non-empty) the
     *  cached entry belongs to a different plugin instance.
     *
     *  B1 FIX (2026-06-19 audit): the cache is a process-wide singleton shared
     *  by all plugin instances. Without instanceId in the key, instance A could
     *  read instance B's cached get_plugin_info (which embeds instanceId/port/
     *  morphCode) on a (toolName, params, generationToken) collision. Namespacing
     *  the key by instanceId makes the shared cache instance-safe.
     */
    std::optional<nlohmann::json> get(const juce::String& toolName,
                                      const juce::var& params,
                                      uint64_t generationToken,
                                      const juce::String& instanceId = {});

    /** Store a result. If an entry for the same key exists it is moved to
     *  the front (most-recently-used) and its TTL is refreshed.
     *
     *  @p instanceId namespaces the entry by plugin instance (see get() docs).
     */
    void put(const juce::String& toolName,
             const juce::var& params,
             uint64_t generationToken,
             const nlohmann::json& result,
             const juce::String& instanceId = {},
             std::chrono::seconds ttl = std::chrono::seconds(30));

    /** Invalidate all entries. Call when the hosted plugin changes or a
     *  full snapshot recall occurs.
     */
    void invalidateAll();

    /** Invalidate only entries whose scope is in @p scopes. Used by the
     *  verified-write path so a parameter write evicts parameter-describing
     *  reads without flushing analysis meters or the semantic profile
     *  (spec §6.2). No-op if @p scopes is empty. Returns the count evicted.
     */
    size_t invalidateScopes(const std::vector<Scope>& scopes);

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

    /** Classify a tool name to its invalidation scope (spec §6.2).
     *  Unknown tools fall back to Scope::Parameters so any unlisted read
     *  is still evicted by a parameter write (safe by default). */
    static Scope scopeForTool(const juce::String& toolName);

    /** Return the age of the most-recently-matched cache entry (seconds since
     *  insertion). Returns -1.0 if no entry is found or the cache is empty.
     *  AUDIT-FIX (P11, 2026-06-29): lets cache-hit responses report staleness
     *  so clients can decide whether a cached decision is still fresh enough.
     */
    double getEntryAgeSeconds(const juce::String& toolName,
                              const juce::var& params,
                              uint64_t generationToken,
                              const juce::String& instanceId = {});

private:
    struct Entry
    {
        std::string key;
        nlohmann::json result;
        uint64_t generationToken = 0;
        std::chrono::steady_clock::time_point insertedAt;
        std::chrono::steady_clock::time_point expiresAt;
        Scope scope = Scope::Parameters;
    };

    std::string makeKey(const juce::String& toolName,
                        const juce::var& params,
                        uint64_t generationToken,
                        const juce::String& instanceId) const;

    size_t maxEntries_;
    mutable std::mutex mutex_;
    std::list<Entry> lru_;
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
    Stats stats_;
};

} // namespace more_phi
