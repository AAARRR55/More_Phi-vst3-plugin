/* More-Phi — Preset/PresetSerializerV2.cpp
 * JSON serialization/deserialization for V2 presets using nlohmann/json.
 * MESSAGE THREAD ONLY. */
#include "PresetSerializerV2.h"
#include "PresetLibrary.h"  // generateUUID
#include "Core/SnapshotBank.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <stdexcept>

namespace more_phi {

// ── Internal helpers ─────────────────────────────────────────────────────────

int64_t PresetSerializerV2::parseIso8601(const std::string& iso)
{
    if (iso.empty()) return 0;

    std::tm tm{};
    std::istringstream ss(iso);
    // Accept "2026-02-26T12:00:00Z" format
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;

#if defined(_WIN32)
    return static_cast<int64_t>(_mkgmtime(&tm));
#else
    return static_cast<int64_t>(timegm(&tm));
#endif
}

std::string PresetSerializerV2::toIso8601(int64_t unixSeconds)
{
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ── toJson ───────────────────────────────────────────────────────────────────

nlohmann::json PresetSerializerV2::toJson(const PresetEntry& preset, SnapshotBank* bank)
{
    nlohmann::json j;

    j["version"]          = SCHEMA_VERSION;
    j["id"]               = preset.id;
    j["name"]             = preset.name;
    j["author"]           = preset.author;
    j["description"]      = preset.description;
    j["tags"]             = preset.tags;
    j["rating"]           = preset.rating;
    j["morphSnapVersion"] = preset.morphSnapVersion;
    j["created"]          = toIso8601(preset.createdTimestamp);
    j["modified"]         = toIso8601(preset.modifiedTimestamp);

    // Hosted plugin info
    j["hostedPlugin"] = {
        {"name",         preset.hostedPlugin.name},
        {"manufacturer", preset.hostedPlugin.manufacturer},
        {"format",       preset.hostedPlugin.format},
        {"uid",          preset.hostedPlugin.uid}
    };

    // If the preset already carries fully serialised snapshot/morph/modulation
    // data in its jsonData field, merge those sections in verbatim so we do
    // not lose information that was loaded from disk.
    if (!preset.jsonData.empty())
    {
        try
        {
            auto existing = nlohmann::json::parse(preset.jsonData);

            for (auto& section : {"snapshots", "morphState", "modulation"})
            {
                if (existing.contains(section))
                    j[section] = existing[section];
            }
        }
        catch (const nlohmann::json::exception&)
        {
            // jsonData is malformed — omit inner sections rather than crash.
        }
    }

    // If no prior snapshot data exists and a live bank is provided, serialize it.
    if (bank != nullptr && !j.contains("snapshots"))
    {
        nlohmann::json occupiedArr = nlohmann::json::array();
        nlohmann::json snapData = nlohmann::json::object();

        if (bank->tryReadLocked([&](const std::array<ParameterState, SnapshotBank::NUM_SLOTS>& slots)
        {
            for (int s = 0; s < SnapshotBank::NUM_SLOTS; ++s)
            {
                nlohmann::json slotObj;
                slotObj["index"] = s;
                slotObj["occupied"] = slots[s].occupied;
                slotObj["paramCount"] = slots[s].parameterCount;

                if (slots[s].occupied && slots[s].parameterCount > 0)
                {
                    nlohmann::json vals = nlohmann::json::array();
                    for (int i = 0; i < slots[s].parameterCount; ++i)
                        vals.push_back(static_cast<double>(slots[s].values[i]));
                    slotObj["values"] = vals;
                    occupiedArr.push_back(s);
                }

                snapData[std::to_string(s)] = slotObj;
            }
        }))
        {
            j["snapshots"] = {
                {"count", 12},
                {"occupied", occupiedArr},
                {"data", snapData}
            };
        }
    }

    // Ensure the three sections always exist (with sensible defaults) so that
    // consumers can rely on them being present.
    if (!j.contains("snapshots"))
    {
        j["snapshots"] = {
            {"count", 12},
            {"occupied", nlohmann::json::array()},
            {"data",     nlohmann::json::object()}
        };
    }
    if (!j.contains("morphState"))
    {
        j["morphState"] = {
            {"positionX",   0.5},
            {"positionY",   0.5},
            {"faderPos",    0.0},
            {"physicsMode", "Direct"},
            {"smoothing",   0.95}
        };
    }
    if (!j.contains("modulation"))
    {
        j["modulation"] = {
            {"routes",      nlohmann::json::array()},
            {"lfoStates",   nlohmann::json::array()},
            {"macroValues", nlohmann::json::array()}
        };
    }

    return j;
}

// ── fromJson ─────────────────────────────────────────────────────────────────

bool PresetSerializerV2::fromJson(const nlohmann::json& j, PresetEntry& outPreset)
{
    std::string errMsg;
    if (!validate(j, errMsg))
        return false;

    try
    {
        PresetEntry p;

        p.id               = j.at("id").get<std::string>();
        p.name             = j.at("name").get<std::string>();
        p.author           = j.value("author", std::string{});
        p.description      = j.value("description", std::string{});
        p.morphSnapVersion = j.value("morphSnapVersion", std::string{});
        p.rating           = j.value("rating", 0);

        // Clamp rating to valid range
        p.rating = std::max(0, std::min(5, p.rating));

        if (j.contains("tags") && j["tags"].is_array())
            p.tags = j["tags"].get<std::vector<std::string>>();

        if (j.contains("created"))
            p.createdTimestamp = parseIso8601(j["created"].get<std::string>());

        if (j.contains("modified"))
            p.modifiedTimestamp = parseIso8601(j["modified"].get<std::string>());

        if (j.contains("hostedPlugin"))
        {
            const auto& hp = j["hostedPlugin"];
            p.hostedPlugin.name         = hp.value("name", std::string{});
            p.hostedPlugin.manufacturer = hp.value("manufacturer", std::string{});
            p.hostedPlugin.format       = hp.value("format", std::string{});
            p.hostedPlugin.uid          = hp.value("uid", std::string{});
        }

        // Store the full JSON as-is so callers can access snapshots/morphState.
        p.jsonData = j.dump(2);

        outPreset = std::move(p);
        return true;
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
}

// ── serialize ────────────────────────────────────────────────────────────────

std::string PresetSerializerV2::serialize(const PresetEntry& preset)
{
    return toJson(preset).dump(2);
}

// ── deserialize ──────────────────────────────────────────────────────────────

bool PresetSerializerV2::deserialize(const std::string& json, PresetEntry& outPreset)
{
    if (json.empty()) return false;

    try
    {
        auto j = nlohmann::json::parse(json);
        return fromJson(j, outPreset);
    }
    catch (const nlohmann::json::parse_error&)
    {
        return false;
    }
}

// ── validate ─────────────────────────────────────────────────────────────────

bool PresetSerializerV2::validate(const nlohmann::json& j, std::string& errorMessage)
{
    if (!j.is_object())
    {
        errorMessage = "Root element must be a JSON object";
        return false;
    }

    // version field
    if (!j.contains("version"))
    {
        errorMessage = "Missing required field: version";
        return false;
    }
    if (!j["version"].is_string())
    {
        errorMessage = "Field 'version' must be a string";
        return false;
    }

    // id field
    if (!j.contains("id"))
    {
        errorMessage = "Missing required field: id";
        return false;
    }
    if (!j["id"].is_string() || j["id"].get<std::string>().empty())
    {
        errorMessage = "Field 'id' must be a non-empty string";
        return false;
    }

    // name field
    if (!j.contains("name"))
    {
        errorMessage = "Missing required field: name";
        return false;
    }
    if (!j["name"].is_string() || j["name"].get<std::string>().empty())
    {
        errorMessage = "Field 'name' must be a non-empty string";
        return false;
    }

    // rating range check
    if (j.contains("rating"))
    {
        if (!j["rating"].is_number_integer())
        {
            errorMessage = "Field 'rating' must be an integer";
            return false;
        }
        int r = j["rating"].get<int>();
        if (r < 0 || r > 5)
        {
            errorMessage = "Field 'rating' must be in the range [0, 5]";
            return false;
        }
    }

    // tags must be an array of strings if present
    if (j.contains("tags"))
    {
        if (!j["tags"].is_array())
        {
            errorMessage = "Field 'tags' must be an array";
            return false;
        }
        for (const auto& tag : j["tags"])
        {
            if (!tag.is_string())
            {
                errorMessage = "All elements of 'tags' must be strings";
                return false;
            }
        }
    }

    // hostedPlugin structure check
    if (j.contains("hostedPlugin"))
    {
        const auto& hp = j["hostedPlugin"];
        if (!hp.is_object())
        {
            errorMessage = "Field 'hostedPlugin' must be an object";
            return false;
        }
    }

    errorMessage.clear();
    return true;
}

// ── migrateFromV1 ────────────────────────────────────────────────────────────

bool PresetSerializerV2::migrateFromV1(const juce::var& v1Json,
                                       PresetEntry& outPreset)
{
    // V1 format: JSON object produced by PresetSerializer::serialize()
    if (!v1Json.isObject()) return false;

    auto version = v1Json.getProperty("version", 0);
    if (static_cast<int>(version) != 1) return false;

    PresetEntry p;
    p.id   = PresetLibrary::generateUUID();
    p.name = v1Json.getProperty("name", "Migrated Preset").toString().toStdString();

    // Use current time for both timestamps since V1 had no timestamps.
    auto now = std::chrono::system_clock::now();
    p.createdTimestamp  = static_cast<int64_t>(
        std::chrono::system_clock::to_time_t(now));
    p.modifiedTimestamp = p.createdTimestamp;

    p.morphSnapVersion = "1.0.0";  // Unknown original version — mark as 1.x.

    // Build a minimal V2 JSON from the V1 data, preserving what we can.
    nlohmann::json j = toJson(p);   // Gives us the default skeleton.

    // Preserve the APVTS state string in the output JSON under "apvts" so it
    // can be restored verbatim when the preset is loaded.
    juce::String apvtsStr = v1Json.getProperty("apvts", {}).toString();
    if (apvtsStr.isNotEmpty())
    {
        j["apvts"] = apvtsStr.toStdString();
        p.description = "Migrated from V1 preset.";
    }

    // Parse the V1 snapshots array and convert into the V2 JSON format.
    auto* snapArr = v1Json.getProperty("snapshots", {}).getArray();
    if (snapArr != nullptr)
    {
        nlohmann::json occupiedArr = nlohmann::json::array();
        nlohmann::json snapData    = nlohmann::json::object();

        for (int s = 0; s < juce::jmin(static_cast<int>(snapArr->size()), 12); ++s)
        {
            const auto& slotVar = (*snapArr)[s];
            if (!slotVar.isObject()) continue;

            bool occupied   = slotVar.getProperty("occupied", false);
            int  paramCount = slotVar.getProperty("paramCount", 0);
            juce::String slotName = slotVar.getProperty("name", {}).toString();

            nlohmann::json slotObj;
            slotObj["index"]      = s;
            slotObj["occupied"]   = occupied;
            slotObj["paramCount"] = paramCount;
            if (slotName.isNotEmpty())
                slotObj["name"] = slotName.toStdString();

            if (occupied && paramCount > 0)
            {
                auto* valArr = slotVar.getProperty("values", {}).getArray();
                if (valArr != nullptr)
                {
                    nlohmann::json vals = nlohmann::json::array();
                    for (int i = 0; i < juce::jmin(paramCount, static_cast<int>(valArr->size())); ++i)
                        vals.push_back(static_cast<double>((*valArr)[i]));
                    slotObj["values"] = vals;
                }
                occupiedArr.push_back(s);
            }

            // FIX C4: Preserve per-slot opaque state chunk from V1 (Kontakt/wavetable synths).
            juce::String stateBase64 = slotVar.getProperty("stateChunk", {}).toString();
            if (stateBase64.isNotEmpty())
                slotObj["stateChunk"] = stateBase64.toStdString();

            snapData[std::to_string(s)] = slotObj;
        }

        j["snapshots"]["count"]    = 12;
        j["snapshots"]["occupied"] = occupiedArr;
        j["snapshots"]["data"]     = snapData;
    }

    // Migrate hosted plugin info if present (V1 stored it as an XML string).
    juce::String hostedPluginXml = v1Json.getProperty("hostedPlugin", {}).toString();
    if (hostedPluginXml.isNotEmpty())
    {
        if (auto xml = juce::parseXML(hostedPluginXml))
        {
            juce::PluginDescription desc;
            if (desc.loadFromXml(*xml))
            {
                j["hostedPlugin"] = {
                    {"name",         desc.name.toStdString()},
                    {"manufacturer", desc.manufacturerName.toStdString()},
                    {"format",       desc.pluginFormatName.toStdString()},
                    {"uid",          desc.fileOrIdentifier.toStdString()}
                };
            }
        }
    }

    // Migrate hosted plugin state if present.
    juce::String hostedPluginState = v1Json.getProperty("hostedPluginState", {}).toString();
    if (hostedPluginState.isNotEmpty())
    {
        j["hostedPluginState"] = hostedPluginState.toStdString();
    }

    // Re-build final JSON with updated description.
    j["description"] = p.description;
    p.jsonData = j.dump(2);

    outPreset = std::move(p);
    return true;
}

} // namespace more_phi
