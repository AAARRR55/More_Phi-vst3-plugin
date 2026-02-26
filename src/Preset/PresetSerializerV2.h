/* MorphSnap — Preset/PresetSerializerV2.h
 * V2 preset serializer — reads/writes the V2 JSON format using nlohmann/json.
 * Kept separate from the V1 PresetSerializer to preserve backwards compatibility.
 * MESSAGE THREAD ONLY. */
#pragma once

#include "PresetEntry.h"
#include <nlohmann/json.hpp>
#include <string>

// Forward declaration — avoid pulling in heavy JUCE headers here.
namespace juce { class XmlElement; }

namespace morphsnap {

class PresetSerializerV2
{
public:
    // ── Schema version ──────────────────────────────────────────────────────
    static constexpr const char* SCHEMA_VERSION = "2.0";

    // ── High-level string API ────────────────────────────────────────────────

    /** Serialise a PresetEntry to a JSON string (pretty-printed). */
    static std::string serialize(const PresetEntry& preset);

    /** Deserialise a JSON string into outPreset.
     *  Returns false and leaves outPreset unchanged if the string is invalid. */
    static bool deserialize(const std::string& json, PresetEntry& outPreset);

    // ── nlohmann::json API ───────────────────────────────────────────────────

    /** Convert a PresetEntry to an nlohmann::json object. */
    static nlohmann::json toJson(const PresetEntry& preset);

    /** Populate outPreset from an nlohmann::json object.
     *  Returns false if required fields are missing or malformed. */
    static bool fromJson(const nlohmann::json& j, PresetEntry& outPreset);

    // ── Validation ──────────────────────────────────────────────────────────

    /** Validate a JSON object against the V2 preset schema.
     *  Populates errorMessage with the first problem found. */
    static bool validate(const nlohmann::json& j, std::string& errorMessage);

    // ── Migration ───────────────────────────────────────────────────────────

    /** Migrate a V1 XML preset (produced by the original PresetSerializer) to a
     *  V2 PresetEntry.  The resulting entry will have a freshly generated UUID and
     *  its jsonData field populated.
     *  Returns false if v1Xml does not look like a V1 preset. */
    static bool migrateFromV1(const juce::XmlElement& v1Xml, PresetEntry& outPreset);

private:
    // Helpers
    static int64_t parseIso8601(const std::string& iso);
    static std::string toIso8601(int64_t unixSeconds);
};

} // namespace morphsnap
