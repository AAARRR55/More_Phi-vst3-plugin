/*
 * More-Phi — Preset/IPresetLibrary.h
 * Abstract interface for the V2 preset library.
 *
 * Thread safety contract
 * ----------------------
 * All methods in this interface are message-thread only. The library
 * performs disk I/O and (optionally) network I/O; neither is safe on the
 * audio thread. Do NOT call any method from processBlock() or any other
 * real-time context.
 *
 * Preset identity
 * ---------------
 * Each preset is identified by a UUID string stored in PresetEntry::id.
 * UUIDs are assigned by the library on first save and are stable across
 * sessions. Callers that need to hold a reference to a preset across saves
 * should store the id, not a pointer to a PresetEntry.
 *
 * Search and filtering
 * --------------------
 * search() is the primary discovery mechanism. Implementations may use
 * a full-text index, SQLite FTS, or simple substring matching depending
 * on the backend. The PresetSearchQuery::sortBy field controls the order
 * of results; Relevance ordering is backend-defined.
 *
 * Cloud sync
 * ----------
 * syncToCloud() and syncFromCloud() are optional capabilities. If the
 * library has no cloud backend configured, isCloudEnabled() returns false
 * and the sync methods return false without error.
 */
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace more_phi {

// ---------------------------------------------------------------------------
// PresetEntry — full preset record
// ---------------------------------------------------------------------------

/**
 * Complete descriptor for a single preset in the library.
 *
 * jsonData holds the serialised preset content (snapshot bank, morph
 * settings, modulation routes, etc.) as a JSON string. The schema is
 * defined by PresetSerializer and is versioned via morphSnapVersion.
 *
 * Timestamps are Unix epoch milliseconds (int64_t). A value of 0 indicates
 * that the timestamp has not been recorded.
 */
struct PresetEntry
{
    std::string id;                   // UUID (assigned by library on save)
    std::string name;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
    int         rating = 0;           // [0, 5]; 0 = unrated

    std::string hostedPluginName;     // e.g. "Serum", "Vital", "" = any
    std::string morphSnapVersion;     // e.g. "3.3.0"

    std::string jsonData;             // Serialised preset content

    int64_t createdTimestamp  = 0;    // Unix epoch ms
    int64_t modifiedTimestamp = 0;    // Unix epoch ms
};

// ---------------------------------------------------------------------------
// PresetSearchQuery — search and filter parameters
// ---------------------------------------------------------------------------

/**
 * Parameters for a preset library search.
 *
 * textQuery is matched against preset name, author, description, and tags
 * using implementation-defined matching (substring, FTS, fuzzy).
 *
 * requiredTags: every listed tag must be present on a result entry (AND
 * semantics). An empty vector imposes no tag constraint.
 *
 * pluginFilter: when non-empty, only presets whose hostedPluginName matches
 * (case-insensitive substring) are returned.
 *
 * minRating: only presets with rating >= minRating are returned.
 *
 * maxResults caps the result list to avoid unbounded memory allocation.
 */
struct PresetSearchQuery
{
    enum class SortBy
    {
        Name,
        Date,
        Rating,
        Relevance   // Implementation-defined; typically TF-IDF or BM25
    };

    std::string              textQuery;
    std::vector<std::string> requiredTags;
    std::string              pluginFilter;       // Empty = all plugins
    int                      minRating   = 0;
    SortBy                   sortBy      = SortBy::Relevance;
    int                      maxResults  = 50;
};

// ---------------------------------------------------------------------------
// IPresetLibrary interface
// ---------------------------------------------------------------------------

/**
 * Preset library interface for the V2 MorePhi preset system.
 *
 * Implementations may back this interface with a local SQLite database,
 * a flat JSON file store, or a remote API. The interface is intentionally
 * synchronous; callers that require non-blocking behaviour should invoke
 * these methods from a background thread and marshal results back to the
 * message thread using juce::MessageManager::callAsync().
 */
class IPresetLibrary
{
public:
    virtual ~IPresetLibrary() = default;

    // -----------------------------------------------------------------------
    // Search and retrieval
    // -----------------------------------------------------------------------

    /**
     * Searches the library and returns matching preset entries.
     *
     * @param query  Search parameters (text, tags, plugin filter, sort order).
     * @return       List of matching entries, ordered by query.sortBy,
     *               capped at query.maxResults. Empty if no matches.
     *
     * Message-thread only.
     */
    virtual std::vector<PresetEntry> search(const PresetSearchQuery& query) = 0;

    /**
     * Loads a single preset by ID.
     *
     * @param presetId   UUID of the preset to load.
     * @param outPreset  Populated with the preset data on success.
     * @return           true on success, false if the ID is not found or an
     *                   I/O error occurred.
     *
     * Message-thread only.
     */
    virtual bool load(const std::string& presetId, PresetEntry& outPreset) = 0;

    // -----------------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------------

    /**
     * Saves or updates a preset in the library.
     *
     * If preset.id is empty, the library assigns a new UUID and stores it in
     * the supplied PresetEntry before returning. If preset.id matches an
     * existing record, that record is updated. modifiedTimestamp is always
     * set to the current time by the implementation.
     *
     * @param preset  Preset to save. preset.id may be modified on first save.
     * @return        true on success, false on I/O or validation error.
     *
     * Message-thread only.
     */
    virtual bool save(PresetEntry& preset) = 0;

    /**
     * Permanently removes a preset from the library.
     *
     * @param presetId  UUID of the preset to remove.
     * @return          true on success, false if not found or I/O error.
     *
     * Message-thread only.
     */
    virtual bool remove(const std::string& presetId) = 0;

    // -----------------------------------------------------------------------
    // Metadata queries
    // -----------------------------------------------------------------------

    /**
     * Returns the list of all tag strings that appear on at least one preset
     * currently in the library. Suitable for populating a tag-browser UI.
     * The order of returned tags is implementation-defined.
     *
     * Message-thread only.
     */
    virtual std::vector<std::string> getAllTags() const = 0;

    /**
     * Returns the total number of presets currently stored in the library.
     *
     * Message-thread only.
     */
    virtual int getPresetCount() const = 0;

    // -----------------------------------------------------------------------
    // Import / export
    // -----------------------------------------------------------------------

    /**
     * Imports a preset from a file on disk.
     * The file format is implementation-defined (e.g. a JSON envelope
     * matching the PresetEntry schema). On success the preset is added to
     * the library with a fresh UUID.
     *
     * @param file  Source file. Must exist and be readable.
     * @return      true on success, false on parse or I/O error.
     *
     * Message-thread only.
     */
    virtual bool importFromFile(const juce::File& file) = 0;

    /**
     * Exports a preset from the library to a file on disk.
     * The parent directory of file must exist; the file is created or
     * overwritten. The file format is implementation-defined.
     *
     * @param presetId  UUID of the preset to export.
     * @param file      Destination file path.
     * @return          true on success, false if the preset is not found or
     *                  an I/O error occurred.
     *
     * Message-thread only.
     */
    virtual bool exportToFile(const std::string& presetId, const juce::File& file) = 0;

    // -----------------------------------------------------------------------
    // Cloud sync (optional capability)
    // -----------------------------------------------------------------------

    /**
     * Pushes all locally modified presets to the configured cloud backend.
     * Returns false immediately (without error) when isCloudEnabled() is false.
     *
     * @return  true if the sync succeeded, false on network or auth error.
     *
     * Message-thread only. May block; call from a background thread if latency
     * is a concern.
     */
    virtual bool syncToCloud() = 0;

    /**
     * Fetches remote changes from the configured cloud backend and merges them
     * into the local library. Conflict resolution policy is implementation-defined
     * (e.g. last-write-wins based on modifiedTimestamp).
     * Returns false immediately (without error) when isCloudEnabled() is false.
     *
     * @return  true if the sync succeeded, false on network or auth error.
     *
     * Message-thread only. May block; call from a background thread if latency
     * is a concern.
     */
    virtual bool syncFromCloud() = 0;

    /**
     * Returns true if a cloud backend has been configured and credentials are
     * available. When false, syncToCloud() and syncFromCloud() are no-ops that
     * return false.
     *
     * Message-thread only.
     */
    virtual bool isCloudEnabled() const = 0;
};

} // namespace more_phi
