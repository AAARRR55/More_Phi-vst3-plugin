/* More-Phi — Preset/PresetLibrary.cpp
 * Full-featured V2 preset library: JSON file storage, in-memory index,
 * full-text search, tagging, ratings, import/export.
 * MESSAGE THREAD ONLY. */
#include "PresetLibrary.h"
#include "PresetSerializerV2.h"
#include <algorithm>
#include <cctype>
#include <random>
#include <sstream>
#include <iomanip>

namespace more_phi {

// ── Constructor / Destructor ─────────────────────────────────────────────────

PresetLibrary::PresetLibrary() = default;
PresetLibrary::~PresetLibrary() = default;

// ── Initialisation ───────────────────────────────────────────────────────────

void PresetLibrary::initialize(const juce::File& storageDir)
{
    storageDir_ = storageDir;

    if (!storageDir_.exists())
        storageDir_.createDirectory();

    rebuildIndex();
}

// ── Static UUID v4 generation ─────────────────────────────────────────────────

std::string PresetLibrary::generateUUID()
{
    // Use juce::Uuid if available — it wraps OS-level UUID generation.
    juce::Uuid uuid;
    return uuid.toDashedString().toStdString();
}

// ── File helpers ──────────────────────────────────────────────────────────────

juce::File PresetLibrary::getPresetFile(const std::string& presetId) const
{
    // Sanitise the ID so it is safe to use as a filename on all platforms.
    // UUID v4 strings only contain [0-9a-f-], so this is mostly a safety measure.
    juce::String safeName = juce::String(presetId).replaceCharacter('/', '_')
                                                   .replaceCharacter('\\', '_')
                                                   .replaceCharacter(':', '_');
    return storageDir_.getChildFile(safeName + ".json");
}

bool PresetLibrary::writePresetFile(const PresetEntry& preset) const
{
    if (!preset.isValid()) return false;

    auto file = getPresetFile(preset.id);
    file.getParentDirectory().createDirectory();

    // Use the serializer to produce the canonical JSON representation.
    std::string json = PresetSerializerV2::serialize(preset);
    return file.replaceWithText(juce::String(json));
}

bool PresetLibrary::readPresetFile(const std::string& presetId,
                                   PresetEntry& outPreset) const
{
    auto file = getPresetFile(presetId);
    if (!file.existsAsFile()) return false;

    std::string content = file.loadFileAsString().toStdString();
    return PresetSerializerV2::deserialize(content, outPreset);
}

// ── Index management ──────────────────────────────────────────────────────────

std::string PresetLibrary::toLower(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s)
        result += static_cast<char>(std::tolower(c));
    return result;
}

void PresetLibrary::rebuildIndex()
{
    index_.clear();

    auto files = storageDir_.findChildFiles(juce::File::findFiles, false, "*.json");
    for (auto& f : files)
    {
        std::string content = f.loadFileAsString().toStdString();
        PresetEntry preset;
        if (PresetSerializerV2::deserialize(content, preset) && preset.isValid())
            addToIndex(preset);
    }
}

void PresetLibrary::addToIndex(const PresetEntry& preset)
{
    IndexEntry entry;
    entry.id               = preset.id;
    entry.nameLower        = toLower(preset.name);
    entry.authorLower      = toLower(preset.author);
    entry.descriptionLower = toLower(preset.description);
    entry.tags             = preset.tags;
    entry.pluginName       = preset.hostedPlugin.name;
    entry.rating           = preset.rating;
    entry.modified         = preset.modifiedTimestamp;
    index_.push_back(std::move(entry));
}

void PresetLibrary::removeFromIndex(const std::string& presetId)
{
    index_.erase(std::remove_if(index_.begin(), index_.end(),
        [&](const IndexEntry& e) { return e.id == presetId; }),
        index_.end());
}

PresetLibrary::IndexEntry* PresetLibrary::findInIndex(const std::string& presetId)
{
    for (auto& e : index_)
        if (e.id == presetId) return &e;
    return nullptr;
}

const PresetLibrary::IndexEntry* PresetLibrary::findInIndex(
    const std::string& presetId) const
{
    for (const auto& e : index_)
        if (e.id == presetId) return &e;
    return nullptr;
}

// ── Search helpers ────────────────────────────────────────────────────────────

bool PresetLibrary::matchesFilters(const IndexEntry& entry,
                                    const PresetSearchQuery& query) const
{
    // Rating filter
    if (entry.rating < query.minRating)
        return false;

    // Plugin filter (case-insensitive)
    if (!query.pluginFilter.empty())
    {
        std::string filterLower = toLower(query.pluginFilter);
        std::string pluginLower = toLower(entry.pluginName);
        if (pluginLower.find(filterLower) == std::string::npos)
            return false;
    }

    // Required tags — all must be present (case-insensitive)
    for (const auto& reqTag : query.requiredTags)
    {
        std::string reqLower = toLower(reqTag);
        bool found = false;
        for (const auto& entryTag : entry.tags)
        {
            if (toLower(entryTag) == reqLower)
            {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

float PresetLibrary::scoreMatch(const IndexEntry& entry,
                                 const PresetSearchQuery& query) const
{
    if (query.textQuery.empty())
    {
        // No text query — use rating as a tie-breaker.
        return static_cast<float>(entry.rating);
    }

    std::string queryLower = toLower(query.textQuery);
    float score = 0.0f;

    // Exact name match scores highest.
    if (entry.nameLower == queryLower)
        score += 100.0f;
    else if (entry.nameLower.find(queryLower) == 0)
        score += 60.0f;   // Prefix match in name
    else if (entry.nameLower.find(queryLower) != std::string::npos)
        score += 40.0f;   // Substring in name

    // Author match
    if (entry.authorLower.find(queryLower) != std::string::npos)
        score += 20.0f;

    // Description match
    if (entry.descriptionLower.find(queryLower) != std::string::npos)
        score += 10.0f;

    // Tag match
    for (const auto& tag : entry.tags)
    {
        if (toLower(tag).find(queryLower) != std::string::npos)
        {
            score += 15.0f;
            break;
        }
    }

    // Boost by rating so equally-relevant presets with higher ratings surface first.
    score += static_cast<float>(entry.rating) * 2.0f;

    return score;
}

// ── Browse & Search ───────────────────────────────────────────────────────────

std::vector<PresetEntry> PresetLibrary::search(const PresetSearchQuery& query) const
{
    // Build a scored list of matching index entries.
    struct ScoredId
    {
        std::string id;
        float       score;
        int64_t     modified;
        std::string nameLower;
        int         rating;
    };

    std::vector<ScoredId> matches;
    matches.reserve(index_.size());

    for (const auto& entry : index_)
    {
        if (!matchesFilters(entry, query)) continue;

        // If there is a text query, require at least some relevance.
        float score = scoreMatch(entry, query);
        if (!query.textQuery.empty() && score < 1.0f) continue;

        matches.push_back({entry.id, score, entry.modified, entry.nameLower, entry.rating});
    }

    // Sort according to the requested order.
    switch (query.sortBy)
    {
        case PresetSearchQuery::SortBy::Name:
            std::sort(matches.begin(), matches.end(),
                [](const ScoredId& a, const ScoredId& b) {
                    return a.nameLower < b.nameLower;
                });
            break;

        case PresetSearchQuery::SortBy::DateNewest:
            std::sort(matches.begin(), matches.end(),
                [](const ScoredId& a, const ScoredId& b) {
                    return a.modified > b.modified;
                });
            break;

        case PresetSearchQuery::SortBy::DateOldest:
            std::sort(matches.begin(), matches.end(),
                [](const ScoredId& a, const ScoredId& b) {
                    return a.modified < b.modified;
                });
            break;

        case PresetSearchQuery::SortBy::Rating:
            std::sort(matches.begin(), matches.end(),
                [](const ScoredId& a, const ScoredId& b) {
                    return a.rating > b.rating;
                });
            break;

        case PresetSearchQuery::SortBy::Relevance:
        default:
            std::sort(matches.begin(), matches.end(),
                [](const ScoredId& a, const ScoredId& b) {
                    return a.score > b.score;
                });
            break;
    }

    // Apply pagination.
    int offset = std::max(0, query.offset);
    int limit  = std::max(1, query.maxResults);

    if (offset >= static_cast<int>(matches.size()))
        return {};

    auto begin = matches.begin() + offset;
    auto end   = matches.begin() +
                 std::min(static_cast<int>(matches.size()), offset + limit);

    // Load the full PresetEntry for each result.
    std::vector<PresetEntry> results;
    results.reserve(static_cast<size_t>(std::distance(begin, end)));

    for (auto it = begin; it != end; ++it)
    {
        PresetEntry p;
        if (readPresetFile(it->id, p))
            results.push_back(std::move(p));
    }

    return results;
}

std::vector<PresetEntry> PresetLibrary::getRecent(int count) const
{
    PresetSearchQuery q;
    q.sortBy    = PresetSearchQuery::SortBy::DateNewest;
    q.maxResults = std::max(1, count);
    q.offset    = 0;
    return search(q);
}

std::vector<PresetEntry> PresetLibrary::getFavorites() const
{
    PresetSearchQuery q;
    q.minRating  = 4;
    q.sortBy     = PresetSearchQuery::SortBy::Rating;
    q.maxResults = 200;
    return search(q);
}

// ── CRUD ──────────────────────────────────────────────────────────────────────

bool PresetLibrary::save(const PresetEntry& preset)
{
    if (!preset.isValid()) return false;
    if (findInIndex(preset.id) != nullptr) return false;  // Already exists — use update().

    if (!writePresetFile(preset)) return false;

    addToIndex(preset);
    return true;
}

bool PresetLibrary::load(const std::string& presetId, PresetEntry& outPreset) const
{
    if (findInIndex(presetId) == nullptr) return false;
    return readPresetFile(presetId, outPreset);
}

bool PresetLibrary::update(const PresetEntry& preset)
{
    if (!preset.isValid()) return false;
    if (findInIndex(preset.id) == nullptr) return false;  // Does not exist — use save().

    if (!writePresetFile(preset)) return false;

    removeFromIndex(preset.id);
    addToIndex(preset);
    return true;
}

bool PresetLibrary::remove(const std::string& presetId)
{
    if (findInIndex(presetId) == nullptr) return false;

    auto file = getPresetFile(presetId);
    if (file.existsAsFile())
        file.deleteFile();

    removeFromIndex(presetId);
    return true;
}

bool PresetLibrary::exists(const std::string& presetId) const
{
    return findInIndex(presetId) != nullptr;
}

// ── Metadata queries ──────────────────────────────────────────────────────────

std::vector<std::string> PresetLibrary::getAllTags() const
{
    std::vector<std::string> tags;
    for (const auto& entry : index_)
        for (const auto& tag : entry.tags)
            tags.push_back(tag);

    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::vector<std::string> PresetLibrary::getAllPluginNames() const
{
    std::vector<std::string> names;
    for (const auto& entry : index_)
        if (!entry.pluginName.empty())
            names.push_back(entry.pluginName);

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

int PresetLibrary::getPresetCount() const
{
    return static_cast<int>(index_.size());
}

// ── Import / Export ───────────────────────────────────────────────────────────

bool PresetLibrary::importFromFile(const juce::File& jsonFile)
{
    if (!jsonFile.existsAsFile()) return false;

    std::string content = jsonFile.loadFileAsString().toStdString();
    PresetEntry preset;
    if (!PresetSerializerV2::deserialize(content, preset)) return false;
    if (!preset.isValid()) return false;

    // If a preset with the same ID already exists, update it rather than
    // duplicating it — useful for re-importing an exported preset.
    if (exists(preset.id))
        return update(preset);

    return save(preset);
}

bool PresetLibrary::exportToFile(const std::string& presetId,
                                  const juce::File& outFile) const
{
    PresetEntry preset;
    if (!load(presetId, preset)) return false;

    std::string json = PresetSerializerV2::serialize(preset);
    return outFile.replaceWithText(juce::String(json));
}

bool PresetLibrary::importFromDirectory(const juce::File& dir, int& importedCount)
{
    importedCount = 0;
    if (!dir.isDirectory()) return false;

    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.json");
    for (auto& f : files)
    {
        if (importFromFile(f))
            ++importedCount;
    }

    return importedCount > 0;
}

} // namespace more_phi
