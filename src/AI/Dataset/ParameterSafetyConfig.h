/*
 * MorphSnap — AI/Dataset/ParameterSafetyConfig.h
 * Parameter safety configuration for safe DSP parameter randomization.
 *
 * The "Parameter Trap" Problem:
 *   Blindly randomizing plugin parameters causes:
 *   - Silent outputs (Mute, Bypass, Solo activated)
 *   - Zero outputs (Phase Invert + extreme filtering)
 *   - Duplicate files (parameters with no audible effect)
 *
 * This system provides:
 *   - Parameter classification (Safe DSP vs Dangerous Binary)
 *   - Plugin-specific parameter profiles (FabFilter, etc.)
 *   - Safe randomization with guardrails
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace morphsnap {

/** Parameter category for safety classification */
enum class ParameterCategory
{
    SafeContinuous,     ///< Safe DSP: Frequency, Gain, Q, Mix, etc.
    SafeDiscrete,       ///< Safe discrete: Filter Type, Slope, etc.
    DangerousBinary,    ///< Mute, Solo, Bypass, Phase Invert - can silence output
    DangerousSystem,    ///< Preset load, UI toggles - should never automate
    Unknown             ///< Unclassified - treat with caution
};

/** Convert ParameterCategory to string */
inline juce::String parameterCategoryToString(ParameterCategory cat)
{
    switch (cat)
    {
        case ParameterCategory::SafeContinuous:   return "SafeContinuous";
        case ParameterCategory::SafeDiscrete:     return "SafeDiscrete";
        case ParameterCategory::DangerousBinary:  return "DangerousBinary";
        case ParameterCategory::DangerousSystem:  return "DangerousSystem";
        case ParameterCategory::Unknown:          return "Unknown";
    }
    return "Unknown";
}

/** Safety rules for a single parameter */
struct ParameterSafetyRule
{
    int parameterIndex = -1;
    juce::String parameterName;
    ParameterCategory category = ParameterCategory::Unknown;

    // For SafeContinuous parameters
    float minValue = 0.0f;          ///< Minimum safe value (normalized 0-1)
    float maxValue = 1.0f;          ///< Maximum safe value (normalized 0-1)
    float defaultValue = 0.5f;      ///< Default/fallback value
    bool randomizeEnabled = true;   ///< Whether to include in randomization

    // For SafeDiscrete parameters
    std::vector<float> validValues; ///< Valid discrete values (for filter types, etc.)

    // For DangerousBinary parameters
    float lockedValue = 0.0f;       ///< Value to lock (usually 0 = off)

    /** Convert to JSON */
    nlohmann::json toJson() const
    {
        nlohmann::json j;
        j["index"] = parameterIndex;
        j["name"] = parameterName.toStdString();
        j["category"] = parameterCategoryToString(category).toStdString();
        j["minValue"] = minValue;
        j["maxValue"] = maxValue;
        j["defaultValue"] = defaultValue;
        j["randomizeEnabled"] = randomizeEnabled;

        if (!validValues.empty())
            j["validValues"] = validValues;

        if (category == ParameterCategory::DangerousBinary)
            j["lockedValue"] = lockedValue;

        return j;
    }

    /** Create from JSON */
    static ParameterSafetyRule fromJson(const nlohmann::json& j)
    {
        ParameterSafetyRule rule;
        if (j.contains("index")) rule.parameterIndex = j["index"].get<int>();
        if (j.contains("name")) rule.parameterName = juce::String(j["name"].get<std::string>());
        if (j.contains("minValue")) rule.minValue = j["minValue"].get<float>();
        if (j.contains("maxValue")) rule.maxValue = j["maxValue"].get<float>();
        if (j.contains("defaultValue")) rule.defaultValue = j["defaultValue"].get<float>();
        if (j.contains("randomizeEnabled")) rule.randomizeEnabled = j["randomizeEnabled"].get<bool>();
        if (j.contains("lockedValue")) rule.lockedValue = j["lockedValue"].get<float>();
        if (j.contains("validValues")) rule.validValues = j["validValues"].get<std::vector<float>>();

        if (j.contains("category"))
        {
            auto catStr = j["category"].get<std::string>();
            if (catStr == "SafeContinuous") rule.category = ParameterCategory::SafeContinuous;
            else if (catStr == "SafeDiscrete") rule.category = ParameterCategory::SafeDiscrete;
            else if (catStr == "DangerousBinary") rule.category = ParameterCategory::DangerousBinary;
            else if (catStr == "DangerousSystem") rule.category = ParameterCategory::DangerousSystem;
            else rule.category = ParameterCategory::Unknown;
        }

        return rule;
    }
};

/**
 * Configuration for safe parameter randomization.
 * Contains rules for each parameter and methods to apply them.
 */
class ParameterSafetyConfig
{
public:
    ParameterSafetyConfig() = default;

    // ── Rule Management ─────────────────────────────────────────────────────

    /** Add a safety rule for a parameter */
    void addRule(const ParameterSafetyRule& rule);

    /** Add a rule by index */
    void addRule(int index, ParameterCategory category,
                 float minVal = 0.0f, float maxVal = 1.0f,
                 float defaultVal = 0.5f);

    /** Get rule for a parameter index */
    const ParameterSafetyRule* getRule(int index) const;

    /** Get rule for a parameter by name (fuzzy match) */
    const ParameterSafetyRule* getRuleByName(const juce::String& name) const;

    /** Check if parameter is safe to randomize */
    bool isSafeToRandomize(int index) const;

    /** Get all rules */
    const std::vector<ParameterSafetyRule>& getAllRules() const { return rules_; }

    /** Clear all rules */
    void clear() { rules_.clear(); indexMap_.clear(); }

    /** Get number of rules */
    int getRuleCount() const { return static_cast<int>(rules_.size()); }

    // ── Parameter Value Sanitization ─────────────────────────────────────────

    /**
     * Sanitize a parameter value according to safety rules.
     * - SafeContinuous: clamped to [minValue, maxValue]
     * - SafeDiscrete: snapped to nearest valid value
     * - DangerousBinary: forced to lockedValue
     * - DangerousSystem: forced to defaultValue
     * - Unknown: passed through with warning
     */
    float sanitizeValue(int index, float value) const;

    /**
     * Sanitize an entire parameter vector.
     * Modifies values in place according to safety rules.
     */
    void sanitizeParameterVector(std::vector<float>& params) const;

    /**
     * Generate a safe random value for a parameter.
     * Uses the rules to determine valid range/type.
     */
    float generateSafeRandomValue(int index, juce::Random& rng) const;

    /**
     * Generate a complete safe parameter vector.
     * Only randomizes parameters marked as safe.
     */
    std::vector<float> generateSafeParameterVector(int totalParameters,
                                                    juce::Random& rng,
                                                    bool onlyRandomizeSafe = true) const;

    // ── Plugin-Specific Profiles ─────────────────────────────────────────────

    /** Load FabFilter Pro-Q 3/4 safety profile */
    static ParameterSafetyConfig createFabFilterProQProfile(int numParameters = 700);

    /** Load FabFilter Pro-L 2 safety profile */
    static ParameterSafetyConfig createFabFilterProLProfile(int numParameters = 100);

    /** Load FabFilter Volcano 3 safety profile */
    static ParameterSafetyConfig createFabFilterVolcanoProfile(int numParameters = 200);

    /** Load generic EQ plugin safety profile */
    static ParameterSafetyConfig createGenericEQProfile(int numParameters);

    /** Load generic dynamics plugin safety profile */
    static ParameterSafetyConfig createGenericDynamicsProfile(int numParameters);

    /** Auto-detect plugin type from name and create appropriate profile */
    static ParameterSafetyConfig createAutoProfile(const juce::String& pluginName,
                                                   int numParameters);

    // ── Serialization ────────────────────────────────────────────────────────

    /** Export to JSON */
    nlohmann::json toJson() const;

    /** Import from JSON */
    static ParameterSafetyConfig fromJson(const nlohmann::json& j);

    /** Save to file */
    bool saveToFile(const juce::File& file) const;

    /** Load from file */
    static ParameterSafetyConfig loadFromFile(const juce::File& file);

    // ── Statistics ───────────────────────────────────────────────────────────

    /** Get count of parameters by category */
    std::unordered_map<ParameterCategory, int> getCategoryCounts() const;

    /** Get list of safe parameter indices */
    std::vector<int> getSafeParameterIndices() const;

    /** Get list of dangerous parameter indices */
    std::vector<int> getDangerousParameterIndices() const;

private:
    /** Check if a parameter name matches a dangerous pattern */
    static bool isDangerousParameterName(const juce::String& name);

    /** Check if a parameter name matches a safe DSP pattern */
    static bool isSafeDSPParameterName(const juce::String& name);

    /** Fuzzy match parameter name */
    static bool parameterNameMatches(const juce::String& name, const juce::String& pattern);

    std::vector<ParameterSafetyRule> rules_;
    std::unordered_map<int, size_t> indexMap_; ///< Maps parameter index to rules_ position
};

} // namespace morphsnap
