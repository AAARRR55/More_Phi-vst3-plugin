/*
 * More-Phi — AI/ToolResultCache.cpp
 * LRU cache implementation for read-only MCP tool results.
 */
#include "ToolResultCache.h"
#include <sstream>

namespace more_phi {

static std::string juceVarToStableString(const juce::var& value)
{
    // Use JUCE's built-in JSON serialization for deterministic key generation.
    return juce::JSON::toString(value, true).toStdString();
}

ToolResultCache::ToolResultCache(size_t maxEntries)
    : maxEntries_(maxEntries)
{
}

std::string ToolResultCache::makeKey(const juce::String& toolName,
                                     const juce::var& params,
                                     uint64_t generationToken) const
{
    std::ostringstream key;
    key << toolName.toStdString() << '\0'
        << juceVarToStableString(params) << '\0'
        << generationToken;
    return key.str();
}

std::optional<nlohmann::json> ToolResultCache::get(const juce::String& toolName,
                                                   const juce::var& params,
                                                   uint64_t generationToken)
{
    const auto key = makeKey(toolName, params, generationToken);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end())
    {
        ++stats_.misses;
        return std::nullopt;
    }

    if (it->second->generationToken != generationToken ||
        it->second->expiresAt <= now)
    {
        lru_.erase(it->second);
        index_.erase(it);
        ++stats_.misses;
        return std::nullopt;
    }

    // Move to front (most-recently-used).
    lru_.splice(lru_.begin(), lru_, it->second);
    ++stats_.hits;
    return it->second->result;
}

void ToolResultCache::put(const juce::String& toolName,
                          const juce::var& params,
                          uint64_t generationToken,
                          const nlohmann::json& result,
                          std::chrono::seconds ttl)
{
    const auto key = makeKey(toolName, params, generationToken);
    const auto now = std::chrono::steady_clock::now();
    const Scope scope = scopeForTool(toolName);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end())
    {
        // Update existing entry in place and move to front.
        it->second->result = result;
        it->second->generationToken = generationToken;
        it->second->expiresAt = now + ttl;
        it->second->scope = scope;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    // Enforce size limit before inserting.
    while (lru_.size() >= maxEntries_ && !lru_.empty())
    {
        const auto& oldest = lru_.back();
        index_.erase(oldest.key);
        lru_.pop_back();
        ++stats_.evictions;
    }

    lru_.push_front(Entry{key, result, generationToken, now + ttl, scope});
    index_[key] = lru_.begin();
}

void ToolResultCache::invalidateAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
}

size_t ToolResultCache::invalidateScopes(const std::vector<Scope>& scopes)
{
    if (scopes.empty())
        return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    size_t evicted = 0;
    for (auto it = lru_.begin(); it != lru_.end();)
    {
        bool match = false;
        for (const Scope s : scopes)
        {
            if (it->scope == s) { match = true; break; }
        }
        if (match)
        {
            index_.erase(it->key);
            it = lru_.erase(it);
            ++evicted;
        }
        else
        {
            ++it;
        }
    }
    stats_.evictions += evicted;
    return evicted;
}

void ToolResultCache::prune()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = lru_.begin(); it != lru_.end();)
    {
        if (it->expiresAt <= now)
        {
            index_.erase(it->key);
            it = lru_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

ToolResultCache::Stats ToolResultCache::getStats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.size = lru_.size();
    return s;
}

ToolResultCache::Scope ToolResultCache::scopeForTool(const juce::String& toolName)
{
    // Order matters: most-specific prefixes first.
    if (toolName == "get_plugin_info" || toolName == "hosted_plugin.info")
        return Scope::PluginInfo;
    if (toolName == "list_parameters" || toolName == "hosted_plugin.parameters"
        || toolName == "get_parameter" || toolName == "more_phi.parameters"
        || toolName == "diagnose_parameter_pipeline")
        return Scope::Parameters;
    if (toolName == "get_morph_state")
        return Scope::Morph;
    if (toolName.startsWith("analysis."))
        return Scope::Analysis;
    if (toolName.startsWith("plugin_profile.") || toolName == "describe_plugin_semantic_map")
        return Scope::Profile;
    if (toolName == "get_instance_info" || toolName == "list_instances")
        return Scope::Instance;
    if (toolName.startsWith("automation.") || toolName.startsWith("permission.")
        || toolName.startsWith("workflow.") || toolName.startsWith("memory.")
        || toolName.startsWith("context.") || toolName.startsWith("events.")
        || toolName.startsWith("sync."))
        return Scope::Control;

    // Unknown read tool: safe default — a parameter write will evict it.
    return Scope::Parameters;
}

} // namespace more_phi
