/* More-Phi — Preset/PresetEntry.h
 * Data structures for a single V2 preset entry and search queries.
 * Pure data — no JUCE dependencies, no audio thread constraints. */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace more_phi {

/** Information about the plugin whose parameters are captured in this preset. */
struct HostedPluginInfo
{
    std::string name;
    std::string manufacturer;
    std::string format;       // "VST3" or "AU"
    std::string uid;

    bool isEmpty() const { return name.empty(); }
};

/** A single V2 preset entry as held in the in-memory library index and on disk. */
struct PresetEntry
{
    std::string id;                     // UUID v4, primary key
    std::string name;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
    int rating = 0;                     // 0-5 stars

    HostedPluginInfo hostedPlugin;
    std::string morphSnapVersion;       // e.g. "4.0.0"

    // Full serialised JSON content — kept verbatim so the library can round-trip
    // without re-serialising every time a caller only needs the metadata.
    std::string jsonData;

    int64_t createdTimestamp  = 0;      // Unix seconds UTC
    int64_t modifiedTimestamp = 0;

    bool isValid() const { return !id.empty() && !name.empty(); }
};

/** Query parameters for PresetLibrary::search(). */
struct PresetSearchQuery
{
    // Free-text searched across name, author and description (case-insensitive).
    std::string textQuery;

    // All tags in this list must be present on a matching preset.
    std::vector<std::string> requiredTags;

    // If non-empty, only presets whose hostedPlugin.name matches are returned.
    std::string pluginFilter;

    // Only return presets with rating >= minRating.
    int minRating = 0;

    enum class SortBy { Name, DateNewest, DateOldest, Rating, Relevance };
    SortBy sortBy = SortBy::Relevance;

    int maxResults = 50;
    int offset     = 0;   // Pagination offset into the full result set.
};

} // namespace more_phi
