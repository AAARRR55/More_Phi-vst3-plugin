/*
 * MorphSnap — Unit Tests: Preset Library (V2)
 *
 * Catch2 v3 test suite for V2 preset management subsystems.
 *
 * Coverage:
 *   - PresetEntry validation (id, name requirements)
 *   - PresetSerializerV2: JSON round-trip, schema version, error handling
 *   - PresetLibrary CRUD: save, load, update, remove, exists, count
 *   - PresetLibrary search: text, tags, plugin filter, rating, sorting, limits
 *   - PresetLibrary import/export: JSON files, directory scan
 *   - UUID generation: uniqueness and format
 *
 * All tests are self-contained and use in-memory or temporary storage.
 * File I/O tests use JUCE's temporary file utilities.
 *
 * The PresetLibrary and PresetSerializerV2 implementations below model
 * the expected V2 production API. Once the production code exists, the
 * test bodies should compile and pass against it unchanged.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../Mocks/MockV2Interfaces.h"

// Use nlohmann/json for serialization tests (already a project dependency)
#include <nlohmann/json.hpp>
#include <juce_core/juce_core.h>

#include <vector>
#include <string>
#include <algorithm>
#include <set>
#include <map>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>

using Catch::Approx;
using namespace morphsnap::test;
using json = nlohmann::json;

// =============================================================================
//  In-memory PresetSerializerV2 — self-contained implementation
// =============================================================================
namespace {

static constexpr const char* SCHEMA_VERSION = "2.0";

/**
 * Serialize a PresetEntry to a JSON string.
 * Schema version is always "2.0".
 */
std::string serializePreset(const PresetEntry& entry)
{
    json j;
    j["schema"]          = SCHEMA_VERSION;
    j["id"]              = entry.id;
    j["name"]            = entry.name;
    j["author"]          = entry.author;
    j["category"]        = entry.category;
    j["tags"]            = entry.tags;
    j["hostedPluginName"]= entry.hostedPluginName;
    j["hostedPluginId"]  = entry.hostedPluginId;
    j["rating"]          = entry.rating;
    j["createdAt"]       = entry.createdAt;
    j["updatedAt"]       = entry.updatedAt;
    return j.dump();
}

/**
 * Deserialize a PresetEntry from a JSON string.
 * Returns false if:
 *   - JSON is malformed
 *   - Required fields (id, name) are missing
 *   - Schema version is not "2.0"
 */
bool deserializePreset(const std::string& jsonStr, PresetEntry& out)
{
    try
    {
        json j = json::parse(jsonStr);

        // Check schema version
        if (!j.contains("schema") || j["schema"] != SCHEMA_VERSION)
            return false;

        // Required fields
        if (!j.contains("id") || !j.contains("name"))
            return false;

        out.id               = j.at("id").get<std::string>();
        out.name             = j.at("name").get<std::string>();
        out.author           = j.value("author", "");
        out.category         = j.value("category", "");
        out.tags             = j.value("tags", std::vector<std::string>{});
        out.hostedPluginName = j.value("hostedPluginName", "");
        out.hostedPluginId   = j.value("hostedPluginId", "");
        out.rating           = j.value("rating", 0.0f);
        out.createdAt        = j.value("createdAt", int64_t{0});
        out.updatedAt        = j.value("updatedAt", int64_t{0});

        // Validate required non-empty fields
        if (out.id.empty() || out.name.empty())
            return false;

        return true;
    }
    catch (...)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// UUID v4 generator (RFC 4122 compliant, uses JUCE's Random for entropy)
// ---------------------------------------------------------------------------

std::string generateUUID()
{
    // Use JUCE's random for platform-independent entropy in tests
    juce::Random rng;
    rng.setSeedRandomly();

    // Generate 16 random bytes
    std::array<uint8_t, 16> bytes;
    for (auto& b : bytes)
        b = static_cast<uint8_t>(rng.nextInt(256));

    // Set version 4 (random) bits: byte[6] = 0100xxxx
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    // Set variant bits: byte[8] = 10xxxxxx
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    // Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss.width(2); oss.fill('0');
        oss << static_cast<int>(bytes[static_cast<size_t>(i)]);
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// In-memory PresetLibrary
// ---------------------------------------------------------------------------

/**
 * In-memory preset library implementing the expected V2 production API.
 * Uses JUCE's File for optional disk persistence.
 *
 * The library can optionally persist presets to a directory on disk.
 * When a directory is not set, all operations are purely in-memory.
 */
class PresetLibrary
{
public:
    /** Set the storage directory for disk persistence. Pass File() to disable. */
    void setDirectory(const juce::File& dir)
    {
        directory_ = dir;
        if (dir.isDirectory())
            dir.createDirectory();
    }

    /**
     * Save a preset. If one with the same id already exists, it is replaced.
     * Returns true on success.
     */
    bool save(const PresetEntry& entry)
    {
        if (!entry.isValid()) return false;

        auto copy       = entry;
        if (copy.updatedAt == 0) copy.updatedAt = now();
        if (copy.createdAt == 0) copy.createdAt = copy.updatedAt;

        store_[entry.id] = copy;

        if (directory_.isDirectory())
        {
            juce::File file = directory_.getChildFile(entry.id + ".json");
            std::string jsonStr = serializePreset(copy);
            copy.filePath = file.getFullPathName().toStdString();
            file.replaceWithText(juce::String(jsonStr));
        }
        return true;
    }

    /**
     * Load a preset by id. Returns false if not found.
     */
    bool load(const std::string& id, PresetEntry& out) const
    {
        auto it = store_.find(id);
        if (it == store_.end()) return false;
        out = it->second;
        return true;
    }

    /**
     * Update a preset's fields. Only the preset with matching id is modified.
     * Returns false if the id does not exist.
     */
    bool update(const PresetEntry& updated)
    {
        if (store_.find(updated.id) == store_.end()) return false;
        return save(updated);
    }

    /**
     * Remove a preset by id. Also deletes any associated file.
     * Returns false if the id was not found.
     */
    bool remove(const std::string& id)
    {
        auto it = store_.find(id);
        if (it == store_.end()) return false;

        if (directory_.isDirectory())
        {
            juce::File file = directory_.getChildFile(id + ".json");
            if (file.existsAsFile()) file.deleteFile();
        }

        store_.erase(it);
        return true;
    }

    bool exists(const std::string& id) const
    {
        return store_.count(id) > 0;
    }

    int getPresetCount() const
    {
        return static_cast<int>(store_.size());
    }

    /**
     * Search presets with filtering and sorting.
     */
    std::vector<PresetEntry> search(const PresetSearchQuery& query) const
    {
        std::vector<PresetEntry> results;

        for (const auto& [id, entry] : store_)
        {
            // Text filter (case-insensitive match on name or author)
            if (!query.textQuery.empty())
            {
                std::string lowerQuery = query.textQuery;
                std::string lowerName  = entry.name;
                std::string lowerAuthor= entry.author;
                std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
                std::transform(lowerName.begin(),  lowerName.end(),  lowerName.begin(),  ::tolower);
                std::transform(lowerAuthor.begin(),lowerAuthor.end(),lowerAuthor.begin(),::tolower);

                bool nameMatch   = lowerName.find(lowerQuery) != std::string::npos;
                bool authorMatch = lowerAuthor.find(lowerQuery) != std::string::npos;
                if (!nameMatch && !authorMatch) continue;
            }

            // Tag filter — all specified tags must be present
            if (!query.tags.empty())
            {
                bool allTagsFound = true;
                for (const auto& tag : query.tags)
                {
                    bool found = std::find(entry.tags.begin(), entry.tags.end(), tag)
                                 != entry.tags.end();
                    if (!found) { allTagsFound = false; break; }
                }
                if (!allTagsFound) continue;
            }

            // Plugin filter
            if (!query.pluginFilter.empty() && entry.hostedPluginName != query.pluginFilter)
                continue;

            // Rating filter
            if (entry.rating < query.minRating) continue;

            results.push_back(entry);
        }

        // Sort
        if (query.sortBy == PresetSearchQuery::SortBy::Name)
        {
            std::sort(results.begin(), results.end(),
                [](const PresetEntry& a, const PresetEntry& b) {
                    return a.name < b.name;
                });
        }
        else if (query.sortBy == PresetSearchQuery::SortBy::DateNewest)
        {
            std::sort(results.begin(), results.end(),
                [](const PresetEntry& a, const PresetEntry& b) {
                    return a.updatedAt > b.updatedAt;
                });
        }

        // Limit results
        if (query.maxResults > 0 && static_cast<int>(results.size()) > query.maxResults)
            results.resize(static_cast<size_t>(query.maxResults));

        return results;
    }

    /**
     * Export all presets to a JSON file.
     */
    bool exportToFile(const juce::File& file) const
    {
        json arr = json::array();
        for (const auto& [id, entry] : store_)
            arr.push_back(json::parse(serializePreset(entry)));

        json root;
        root["schema"]  = SCHEMA_VERSION;
        root["presets"] = arr;

        return file.replaceWithText(juce::String(root.dump(2)));
    }

    /**
     * Import presets from a JSON file (as exported by exportToFile).
     * Returns the number of presets imported successfully.
     */
    int importFromFile(const juce::File& file)
    {
        if (!file.existsAsFile()) return 0;

        std::string content = file.loadFileAsString().toStdString();
        int count = 0;

        try
        {
            json root = json::parse(content);
            if (!root.contains("presets")) return 0;

            for (const auto& j : root["presets"])
            {
                PresetEntry entry;
                if (deserializePreset(j.dump(), entry))
                {
                    store_[entry.id] = entry;
                    ++count;
                }
            }
        }
        catch (...) { return 0; }

        return count;
    }

    /**
     * Import all .json files from a directory.
     * Returns the number of presets imported successfully.
     */
    int importFromDirectory(const juce::File& dir)
    {
        if (!dir.isDirectory()) return 0;

        int count = 0;
        juce::Array<juce::File> files;
        dir.findChildFiles(files, juce::File::findFiles, false, "*.json");

        for (const auto& f : files)
        {
            std::string content = f.loadFileAsString().toStdString();
            PresetEntry entry;
            if (deserializePreset(content, entry))
            {
                store_[entry.id] = entry;
                ++count;
            }
        }
        return count;
    }

    void clearAll() { store_.clear(); }

private:
    std::map<std::string, PresetEntry> store_;
    juce::File directory_;

    static int64_t now()
    {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};

// Helper: create a minimal valid PresetEntry with a unique id
PresetEntry makeEntry(const std::string& name,
                      const std::string& author  = "TestAuthor",
                      float rating               = 3.0f)
{
    PresetEntry e;
    e.id     = generateUUID();
    e.name   = name;
    e.author = author;
    e.rating = rating;
    return e;
}

} // namespace

// =============================================================================
//  PresetEntry Validation
// =============================================================================

TEST_CASE("PresetEntry validation: empty id is invalid", "[preset]")
{
    PresetEntry entry;
    entry.id   = "";
    entry.name = "Valid Name";
    REQUIRE_FALSE(entry.isValid());
}

TEST_CASE("PresetEntry validation: empty name is invalid", "[preset]")
{
    PresetEntry entry;
    entry.id   = "some-uuid-here";
    entry.name = "";
    REQUIRE_FALSE(entry.isValid());
}

TEST_CASE("PresetEntry validation: valid entry passes validation", "[preset]")
{
    PresetEntry entry;
    entry.id   = "550e8400-e29b-41d4-a716-446655440000";
    entry.name = "My Preset";
    REQUIRE(entry.isValid());
}

TEST_CASE("PresetEntry validation: both id and name required", "[preset]")
{
    PresetEntry both;
    both.id   = "abc-123";
    both.name = "Name";
    REQUIRE(both.isValid());

    PresetEntry neitherOne;
    neitherOne.id   = "";
    neitherOne.name = "";
    REQUIRE_FALSE(neitherOne.isValid());
}

// =============================================================================
//  PresetSerializerV2 Round-trip
// =============================================================================

TEST_CASE("PresetSerializerV2 round-trip: serialize then deserialize preserves all fields", "[preset][serialization]")
{
    PresetEntry original;
    original.id              = "test-uuid-1234";
    original.name            = "Test Preset";
    original.author          = "Unit Tester";
    original.category        = "Ambient";
    original.tags            = {"warm", "pad", "evolving"};
    original.hostedPluginName= "Serum";
    original.hostedPluginId  = "VST3:xxxx-yyyy";
    original.rating          = 4.5f;
    original.createdAt       = 1700000000;
    original.updatedAt       = 1700001000;

    std::string jsonStr = serializePreset(original);
    REQUIRE_FALSE(jsonStr.empty());

    PresetEntry restored;
    REQUIRE(deserializePreset(jsonStr, restored));

    REQUIRE(restored.id              == original.id);
    REQUIRE(restored.name            == original.name);
    REQUIRE(restored.author          == original.author);
    REQUIRE(restored.category        == original.category);
    REQUIRE(restored.tags            == original.tags);
    REQUIRE(restored.hostedPluginName== original.hostedPluginName);
    REQUIRE(restored.hostedPluginId  == original.hostedPluginId);
    REQUIRE(restored.rating          == Approx(original.rating).margin(1e-4f));
    REQUIRE(restored.createdAt       == original.createdAt);
    REQUIRE(restored.updatedAt       == original.updatedAt);
}

TEST_CASE("PresetSerializerV2 round-trip: tags are preserved", "[preset][serialization]")
{
    PresetEntry entry;
    entry.id   = "tag-test-001";
    entry.name = "Tag Test";
    entry.tags = {"cinematic", "bass", "dark", "electronic"};

    std::string jsonStr = serializePreset(entry);
    PresetEntry restored;
    REQUIRE(deserializePreset(jsonStr, restored));

    REQUIRE(restored.tags.size() == 4);
    REQUIRE(restored.tags[0] == "cinematic");
    REQUIRE(restored.tags[1] == "bass");
    REQUIRE(restored.tags[2] == "dark");
    REQUIRE(restored.tags[3] == "electronic");
}

TEST_CASE("PresetSerializerV2 round-trip: hosted plugin info is preserved", "[preset][serialization]")
{
    PresetEntry entry;
    entry.id              = "plugin-test-002";
    entry.name            = "Plugin Test";
    entry.hostedPluginName= "Massive X";
    entry.hostedPluginId  = "VST3:{12345678-1234-1234-1234-123456789ABC}";

    std::string jsonStr = serializePreset(entry);
    PresetEntry restored;
    REQUIRE(deserializePreset(jsonStr, restored));

    REQUIRE(restored.hostedPluginName == "Massive X");
    REQUIRE(restored.hostedPluginId   == "VST3:{12345678-1234-1234-1234-123456789ABC}");
}

TEST_CASE("PresetSerializerV2 round-trip: invalid JSON returns false", "[preset][serialization]")
{
    PresetEntry out;
    REQUIRE_FALSE(deserializePreset("not valid json{{}", out));
    REQUIRE_FALSE(deserializePreset("",                  out));
    REQUIRE_FALSE(deserializePreset("null",              out));
    REQUIRE_FALSE(deserializePreset("{\"key\": 42}",     out));
}

TEST_CASE("PresetSerializerV2 round-trip: missing required fields returns false", "[preset][serialization]")
{
    // JSON without 'id'
    json noId;
    noId["schema"] = SCHEMA_VERSION;
    noId["name"]   = "Missing ID";
    PresetEntry out;
    REQUIRE_FALSE(deserializePreset(noId.dump(), out));

    // JSON without 'name'
    json noName;
    noName["schema"] = SCHEMA_VERSION;
    noName["id"]     = "some-id";
    REQUIRE_FALSE(deserializePreset(noName.dump(), out));
}

TEST_CASE("PresetSerializerV2 round-trip: schema version is 2.0", "[preset][serialization]")
{
    PresetEntry entry;
    entry.id   = "schema-test";
    entry.name = "Schema Check";

    std::string jsonStr = serializePreset(entry);
    json j = json::parse(jsonStr);

    REQUIRE(j.contains("schema"));
    REQUIRE(j["schema"].get<std::string>() == "2.0");
}

TEST_CASE("PresetSerializerV2 round-trip: wrong schema version fails deserialization", "[preset][serialization]")
{
    json j;
    j["schema"] = "1.0";
    j["id"]     = "test-id";
    j["name"]   = "Test";

    PresetEntry out;
    REQUIRE_FALSE(deserializePreset(j.dump(), out));
}

// =============================================================================
//  PresetLibrary CRUD
// =============================================================================

TEST_CASE("PresetLibrary CRUD: save creates entry in library", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("Test Preset");

    REQUIRE(lib.save(entry));
    REQUIRE(lib.exists(entry.id));
    REQUIRE(lib.getPresetCount() == 1);
}

TEST_CASE("PresetLibrary CRUD: load retrieves saved preset", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("Pad Drone");
    entry.author = "Alex";

    REQUIRE(lib.save(entry));

    PresetEntry loaded;
    REQUIRE(lib.load(entry.id, loaded));
    REQUIRE(loaded.name   == "Pad Drone");
    REQUIRE(loaded.author == "Alex");
}

TEST_CASE("PresetLibrary CRUD: update modifies existing preset", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("Original Name");
    lib.save(entry);

    entry.name   = "Updated Name";
    entry.rating = 5.0f;
    REQUIRE(lib.update(entry));

    PresetEntry loaded;
    REQUIRE(lib.load(entry.id, loaded));
    REQUIRE(loaded.name   == "Updated Name");
    REQUIRE(loaded.rating == Approx(5.0f));
}

TEST_CASE("PresetLibrary CRUD: remove deletes preset from library", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("To Delete");
    lib.save(entry);

    REQUIRE(lib.exists(entry.id));
    REQUIRE(lib.remove(entry.id));
    REQUIRE_FALSE(lib.exists(entry.id));
    REQUIRE(lib.getPresetCount() == 0);
}

TEST_CASE("PresetLibrary CRUD: exists returns true for saved preset", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("Exists Test");
    lib.save(entry);

    REQUIRE(lib.exists(entry.id));
}

TEST_CASE("PresetLibrary CRUD: exists returns false for unknown id", "[preset][library]")
{
    PresetLibrary lib;
    REQUIRE_FALSE(lib.exists("this-id-does-not-exist"));
}

TEST_CASE("PresetLibrary CRUD: getPresetCount tracks library size", "[preset][library]")
{
    PresetLibrary lib;
    REQUIRE(lib.getPresetCount() == 0);

    for (int i = 0; i < 5; ++i)
        lib.save(makeEntry("Preset " + std::to_string(i)));

    REQUIRE(lib.getPresetCount() == 5);

    lib.remove(lib.search({}).front().id);
    REQUIRE(lib.getPresetCount() == 4);
}

TEST_CASE("PresetLibrary CRUD: save invalid entry fails", "[preset][library]")
{
    PresetLibrary lib;

    PresetEntry invalid;  // empty id and name
    REQUIRE_FALSE(lib.save(invalid));
    REQUIRE(lib.getPresetCount() == 0);
}

TEST_CASE("PresetLibrary CRUD: update non-existent preset fails", "[preset][library]")
{
    PresetLibrary lib;
    auto entry = makeEntry("Ghost");
    // Not saved first
    REQUIRE_FALSE(lib.update(entry));
}

// =============================================================================
//  PresetLibrary Search
// =============================================================================

TEST_CASE("PresetLibrary search: text query matches name", "[preset][search]")
{
    PresetLibrary lib;
    lib.save(makeEntry("Deep Bass Wobble"));
    lib.save(makeEntry("Bright Lead"));
    lib.save(makeEntry("Dark Ambient Pad"));

    PresetSearchQuery q;
    q.textQuery = "bass";
    auto results = lib.search(q);

    REQUIRE(results.size() == 1);
    REQUIRE(results[0].name == "Deep Bass Wobble");
}

TEST_CASE("PresetLibrary search: text query matches author", "[preset][search]")
{
    PresetLibrary lib;
    auto e1 = makeEntry("Preset A", "Alice");
    auto e2 = makeEntry("Preset B", "Bob");
    lib.save(e1);
    lib.save(e2);

    PresetSearchQuery q;
    q.textQuery = "alice";
    auto results = lib.search(q);

    REQUIRE(results.size() == 1);
    REQUIRE(results[0].author == "Alice");
}

TEST_CASE("PresetLibrary search: text query is case-insensitive", "[preset][search]")
{
    PresetLibrary lib;
    lib.save(makeEntry("Funky Groove"));

    PresetSearchQuery q;
    q.textQuery = "FUNKY";
    auto results = lib.search(q);

    REQUIRE(results.size() == 1);

    q.textQuery = "funky";
    results = lib.search(q);
    REQUIRE(results.size() == 1);

    q.textQuery = "FuNkY";
    results = lib.search(q);
    REQUIRE(results.size() == 1);
}

TEST_CASE("PresetLibrary search: tag filter requires all specified tags", "[preset][search]")
{
    PresetLibrary lib;

    auto e1 = makeEntry("P1");
    e1.tags = {"warm", "pad"};

    auto e2 = makeEntry("P2");
    e2.tags = {"warm", "lead", "bright"};

    auto e3 = makeEntry("P3");
    e3.tags = {"dark", "atmospheric"};

    lib.save(e1);
    lib.save(e2);
    lib.save(e3);

    // Only "warm" tag — should match P1 and P2
    PresetSearchQuery q;
    q.tags = {"warm"};
    auto results = lib.search(q);
    REQUIRE(results.size() == 2);

    // Both "warm" AND "pad" — should match only P1
    q.tags = {"warm", "pad"};
    results = lib.search(q);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].name == "P1");
}

TEST_CASE("PresetLibrary search: plugin filter restricts results", "[preset][search]")
{
    PresetLibrary lib;

    auto e1 = makeEntry("Serum Preset");
    e1.hostedPluginName = "Serum";

    auto e2 = makeEntry("Massive Preset");
    e2.hostedPluginName = "Massive X";

    auto e3 = makeEntry("Another Serum");
    e3.hostedPluginName = "Serum";

    lib.save(e1);
    lib.save(e2);
    lib.save(e3);

    PresetSearchQuery q;
    q.pluginFilter = "Serum";
    auto results = lib.search(q);

    REQUIRE(results.size() == 2);
    for (const auto& r : results)
        REQUIRE(r.hostedPluginName == "Serum");
}

TEST_CASE("PresetLibrary search: min rating filters low-rated presets", "[preset][search]")
{
    PresetLibrary lib;

    auto e1 = makeEntry("Low Rated",   "A", 1.0f);
    auto e2 = makeEntry("Mid Rated",   "A", 3.0f);
    auto e3 = makeEntry("High Rated",  "A", 5.0f);

    lib.save(e1);
    lib.save(e2);
    lib.save(e3);

    PresetSearchQuery q;
    q.minRating = 3.0f;
    auto results = lib.search(q);

    REQUIRE(results.size() == 2);
    for (const auto& r : results)
        REQUIRE(r.rating >= 3.0f);
}

TEST_CASE("PresetLibrary search: sort by name alphabetizes results", "[preset][search]")
{
    PresetLibrary lib;
    lib.save(makeEntry("Zebra"));
    lib.save(makeEntry("Alpha"));
    lib.save(makeEntry("Mango"));

    PresetSearchQuery q;
    q.sortBy = PresetSearchQuery::SortBy::Name;
    auto results = lib.search(q);

    REQUIRE(results.size() == 3);
    REQUIRE(results[0].name == "Alpha");
    REQUIRE(results[1].name == "Mango");
    REQUIRE(results[2].name == "Zebra");
}

TEST_CASE("PresetLibrary search: sort by date newest puts recent first", "[preset][search]")
{
    PresetLibrary lib;

    auto e1 = makeEntry("Old");
    e1.updatedAt = 1000;

    auto e2 = makeEntry("Newer");
    e2.updatedAt = 2000;

    auto e3 = makeEntry("Newest");
    e3.updatedAt = 3000;

    lib.save(e1);
    lib.save(e2);
    lib.save(e3);

    PresetSearchQuery q;
    q.sortBy = PresetSearchQuery::SortBy::DateNewest;
    auto results = lib.search(q);

    REQUIRE(results.size() == 3);
    REQUIRE(results[0].name == "Newest");
    REQUIRE(results[1].name == "Newer");
    REQUIRE(results[2].name == "Old");
}

TEST_CASE("PresetLibrary search: maxResults limits output", "[preset][search]")
{
    PresetLibrary lib;
    for (int i = 0; i < 20; ++i)
        lib.save(makeEntry("Preset " + std::to_string(i)));

    PresetSearchQuery q;
    q.maxResults = 5;
    auto results = lib.search(q);

    REQUIRE(results.size() == 5);
}

TEST_CASE("PresetLibrary search: empty query returns all presets", "[preset][search]")
{
    PresetLibrary lib;
    for (int i = 0; i < 7; ++i)
        lib.save(makeEntry("P" + std::to_string(i)));

    PresetSearchQuery q;  // All fields at defaults — no filter
    auto results = lib.search(q);

    REQUIRE(results.size() == 7);
}

// =============================================================================
//  PresetLibrary Import/Export (with JUCE temp files)
// =============================================================================

TEST_CASE("PresetLibrary import/export: export creates valid JSON file", "[preset][io]")
{
    PresetLibrary lib;
    auto e1 = makeEntry("Export Test 1");
    auto e2 = makeEntry("Export Test 2");
    lib.save(e1);
    lib.save(e2);

    juce::File tmpFile = juce::File::createTempFile("morphsnap_export_test");
    REQUIRE(lib.exportToFile(tmpFile));

    // Verify file exists and contains valid JSON
    REQUIRE(tmpFile.existsAsFile());
    std::string content = tmpFile.loadFileAsString().toStdString();
    REQUIRE_FALSE(content.empty());

    json j;
    REQUIRE_NOTHROW(j = json::parse(content));
    REQUIRE(j.contains("presets"));
    REQUIRE(j.contains("schema"));
    REQUIRE(j["presets"].is_array());
    REQUIRE(j["presets"].size() == 2);

    tmpFile.deleteFile();
}

TEST_CASE("PresetLibrary import/export: import reads exported file", "[preset][io]")
{
    PresetLibrary lib;
    auto e1 = makeEntry("Export Import A");
    auto e2 = makeEntry("Export Import B");
    e1.tags   = {"test", "export"};
    e2.author = "Exporter";
    lib.save(e1);
    lib.save(e2);

    juce::File tmpFile = juce::File::createTempFile("morphsnap_ei_test");
    REQUIRE(lib.exportToFile(tmpFile));

    // Import into a fresh library
    PresetLibrary lib2;
    int count = lib2.importFromFile(tmpFile);
    REQUIRE(count == 2);
    REQUIRE(lib2.getPresetCount() == 2);

    // Verify one of the presets
    PresetEntry loaded;
    REQUIRE(lib2.load(e1.id, loaded));
    REQUIRE(loaded.name == "Export Import A");
    REQUIRE(loaded.tags == e1.tags);

    tmpFile.deleteFile();
}

TEST_CASE("PresetLibrary import/export: import from directory finds all JSON files", "[preset][io]")
{
    // Create a temp directory with individual preset JSON files
    juce::File tmpDir = juce::File::createTempFile("morphsnap_dir_test");
    tmpDir.deleteFile();  // remove the temp file
    tmpDir.createDirectory();

    // Write individual preset JSON files
    for (int i = 0; i < 3; ++i)
    {
        auto entry = makeEntry("Dir Preset " + std::to_string(i));
        std::string jsonStr = serializePreset(entry);
        juce::File f = tmpDir.getChildFile(entry.id + ".json");
        f.replaceWithText(juce::String(jsonStr));
    }

    // Import from directory
    PresetLibrary lib;
    int count = lib.importFromDirectory(tmpDir);
    REQUIRE(count == 3);
    REQUIRE(lib.getPresetCount() == 3);

    // Cleanup
    tmpDir.deleteRecursively();
}

TEST_CASE("PresetLibrary import/export: import from non-existent directory returns 0", "[preset][io]")
{
    PresetLibrary lib;
    juce::File nonExistent("/this/path/does/not/exist/hopefully");
    int count = lib.importFromDirectory(nonExistent);
    REQUIRE(count == 0);
}

// =============================================================================
//  UUID Generation
// =============================================================================

TEST_CASE("UUID generation: generated UUIDs are unique", "[preset][uuid]")
{
    const int N = 100;
    std::set<std::string> uuids;

    for (int i = 0; i < N; ++i)
    {
        std::string uuid = generateUUID();
        REQUIRE_FALSE(uuid.empty());
        // Insert should succeed (no duplicate)
        REQUIRE(uuids.insert(uuid).second);
    }

    REQUIRE(static_cast<int>(uuids.size()) == N);
}

TEST_CASE("UUID generation: UUID format is valid v4", "[preset][uuid]")
{
    // UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // where y is 8, 9, a, or b
    const std::regex uuidRegex(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");

    for (int i = 0; i < 20; ++i)
    {
        std::string uuid = generateUUID();
        INFO("UUID: " << uuid);
        REQUIRE(std::regex_match(uuid, uuidRegex));
    }
}

TEST_CASE("UUID generation: UUID has correct character count", "[preset][uuid]")
{
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx = 8+1+4+1+4+1+4+1+12 = 36 chars
    std::string uuid = generateUUID();
    REQUIRE(uuid.length() == 36);
}

TEST_CASE("UUID generation: UUID has dashes at correct positions", "[preset][uuid]")
{
    std::string uuid = generateUUID();
    REQUIRE(uuid[8]  == '-');
    REQUIRE(uuid[13] == '-');
    REQUIRE(uuid[18] == '-');
    REQUIRE(uuid[23] == '-');
}
