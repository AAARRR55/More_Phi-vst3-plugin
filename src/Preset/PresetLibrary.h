/* MorphSnap — Preset/PresetLibrary.h
 * Full-featured V2 preset management: JSON storage, in-memory index,
 * full-text search, tagging, ratings, import/export.
 * MESSAGE THREAD ONLY — not audio-thread safe. */
#pragma once

#include "PresetEntry.h"
#include <juce_core/juce_core.h>
#include <string>
#include <vector>
#include <cstdint>

namespace morphsnap {

class PresetLibrary
{
public:
    PresetLibrary();
    ~PresetLibrary();

    // Non-copyable, non-movable (owns mutable index state).
    PresetLibrary(const PresetLibrary&) = delete;
    PresetLibrary& operator=(const PresetLibrary&) = delete;

    // ── Initialisation ───────────────────────────────────────────────────────

    /** Prepare the library for use.  Creates storageDir if it does not exist,
     *  then scans all .json files and rebuilds the in-memory index. */
    void initialize(const juce::File& storageDir);

    // ── Browse & search ──────────────────────────────────────────────────────

    /** Full-text + tag + plugin + rating search with optional sorting/pagination. */
    std::vector<PresetEntry> search(const PresetSearchQuery& query) const;

    /** Return the most recently modified presets (up to count). */
    std::vector<PresetEntry> getRecent(int count = 20) const;

    /** Return presets with rating >= 4, sorted by rating descending. */
    std::vector<PresetEntry> getFavorites() const;

    // ── CRUD ─────────────────────────────────────────────────────────────────

    /** Write a new preset to disk and add it to the index.
     *  preset.id must be non-empty (use generateUUID() if needed).
     *  Returns false if the preset is invalid or the file write fails. */
    bool save(const PresetEntry& preset);

    /** Load the full PresetEntry (including jsonData) for the given ID.
     *  Returns false if the ID is not found. */
    bool load(const std::string& presetId, PresetEntry& outPreset) const;

    /** Overwrite an existing preset on disk and refresh the index entry.
     *  Returns false if the preset does not already exist or the write fails. */
    bool update(const PresetEntry& preset);

    /** Delete the preset JSON file and remove it from the index.
     *  Returns false if the ID is not found. */
    bool remove(const std::string& presetId);

    /** True if a preset with this ID is currently in the index. */
    bool exists(const std::string& presetId) const;

    // ── Metadata queries ─────────────────────────────────────────────────────

    /** Sorted, deduplicated list of every tag used across all presets. */
    std::vector<std::string> getAllTags() const;

    /** Sorted, deduplicated list of every hosted-plugin name in the library. */
    std::vector<std::string> getAllPluginNames() const;

    /** Total number of presets currently in the library. */
    int getPresetCount() const;

    // ── Import / export ──────────────────────────────────────────────────────

    /** Import a single preset from a .json file.
     *  Returns false if the file is missing, unreadable, or fails validation. */
    bool importFromFile(const juce::File& jsonFile);

    /** Export the preset with the given ID to outFile as a .json file.
     *  Returns false if the ID is not found or the write fails. */
    bool exportToFile(const std::string& presetId, const juce::File& outFile) const;

    /** Import all .json files in dir (non-recursive).
     *  importedCount is set to the number of successfully imported presets.
     *  Returns true if at least one preset was imported. */
    bool importFromDirectory(const juce::File& dir, int& importedCount);

    // ── Utilities ────────────────────────────────────────────────────────────

    /** Generate a random UUID v4 string suitable for use as a preset ID. */
    static std::string generateUUID();

    /** Return the directory passed to initialize(). */
    const juce::File& getStorageDir() const { return storageDir_; }

private:
    // ── Flat index entry (no jsonData — kept lean for search performance) ─────
    struct IndexEntry
    {
        std::string id;
        std::string nameLower;         // Lowercase for case-insensitive search
        std::string authorLower;
        std::string descriptionLower;
        std::vector<std::string> tags; // Original case preserved
        std::string pluginName;
        int rating   = 0;
        int64_t modified = 0;          // Unix seconds — used for recency sorting
    };

    juce::File              storageDir_;
    std::vector<IndexEntry> index_;

    // ── File helpers ─────────────────────────────────────────────────────────
    juce::File getPresetFile(const std::string& presetId) const;
    bool writePresetFile(const PresetEntry& preset) const;
    bool readPresetFile(const std::string& presetId, PresetEntry& outPreset) const;

    // ── Index management ─────────────────────────────────────────────────────
    void rebuildIndex();
    void addToIndex(const PresetEntry& preset);
    void removeFromIndex(const std::string& presetId);
    IndexEntry* findInIndex(const std::string& presetId);
    const IndexEntry* findInIndex(const std::string& presetId) const;

    // ── Search internals ─────────────────────────────────────────────────────

    /** Returns a relevance score for the entry against query (higher = better).
     *  Only call when matchesFilters() has already returned true. */
    float scoreMatch(const IndexEntry& entry, const PresetSearchQuery& query) const;

    /** Returns true when entry satisfies all filter criteria in query
     *  (tags, pluginFilter, minRating) regardless of text relevance. */
    bool matchesFilters(const IndexEntry& entry, const PresetSearchQuery& query) const;

    // Lower-case helper
    static std::string toLower(const std::string& s);
};

} // namespace morphsnap
