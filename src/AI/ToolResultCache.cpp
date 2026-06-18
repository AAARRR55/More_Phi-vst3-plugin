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

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end())
    {
        // Update existing entry in place and move to front.
        it->second->result = result;
        it->second->generationToken = generationToken;
        it->second->expiresAt = now + ttl;
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

    lru_.push_front(Entry{key, result, generationToken, now + ttl});
    index_[key] = lru_.begin();
}

void ToolResultCache::invalidateAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
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

} // namespace more_phi
