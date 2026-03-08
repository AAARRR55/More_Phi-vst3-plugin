/*
 * MorphSnap — AI/Dataset/ParameterSafetyConfig.cpp
 * Implementation of parameter safety configuration for safe DSP randomization.
 */
#include "ParameterSafetyConfig.h"
#include <algorithm>
#include <fstream>

namespace morphsnap {

// ============================================================================
// Rule Management
// ============================================================================

void ParameterSafetyConfig::addRule(const ParameterSafetyRule& rule)
{
    if (rule.parameterIndex < 0)
        return;

    // Check if rule already exists for this index
    auto it = indexMap_.find(rule.parameterIndex);
    if (it != indexMap_.end())
    {
        // Update existing rule
        rules_[it->second] = rule;
    }
    else
    {
        // Add new rule
        indexMap_[rule.parameterIndex] = rules_.size();
        rules_.push_back(rule);
    }
}

void ParameterSafetyConfig::addRule(int index, ParameterCategory category,
                                     float minVal, float maxVal, float defaultVal)
{
    ParameterSafetyRule rule;
    rule.parameterIndex = index;
    rule.category = category;
    rule.minValue = minVal;
    rule.maxValue = maxVal;
    rule.defaultValue = defaultVal;
    rule.randomizeEnabled = (category == ParameterCategory::SafeContinuous ||
                             category == ParameterCategory::SafeDiscrete);

    if (category == ParameterCategory::DangerousBinary)
    {
        rule.lockedValue = 0.0f; // Default: turn off
    }

    addRule(rule);
}

const ParameterSafetyRule* ParameterSafetyConfig::getRule(int index) const
{
    auto it = indexMap_.find(index);
    if (it != indexMap_.end())
        return &rules_[it->second];
    return nullptr;
}

const ParameterSafetyRule* ParameterSafetyConfig::getRuleByName(const juce::String& name) const
{
    for (const auto& rule : rules_)
    {
        if (parameterNameMatches(rule.parameterName, name))
            return &rule;
    }
    return nullptr;
}

bool ParameterSafetyConfig::isSafeToRandomize(int index) const
{
    const auto* rule = getRule(index);
    if (!rule)
        return false; // Unknown = not safe

    return rule->randomizeEnabled &&
           (rule->category == ParameterCategory::SafeContinuous ||
            rule->category == ParameterCategory::SafeDiscrete);
}

// ============================================================================
// Parameter Value Sanitization
// ============================================================================

float ParameterSafetyConfig::sanitizeValue(int index, float value) const
{
    const auto* rule = getRule(index);

    if (!rule)
    {
        // Unknown parameter - pass through but could log warning
        return juce::jlimit(0.0f, 1.0f, value);
    }

    switch (rule->category)
    {
        case ParameterCategory::SafeContinuous:
            // Clamp to safe range
            return juce::jlimit(rule->minValue, rule->maxValue, value);

        case ParameterCategory::SafeDiscrete:
            // Snap to nearest valid value
            if (!rule->validValues.empty())
            {
                float nearest = rule->validValues[0];
                float minDist = std::abs(value - nearest);
                for (float validVal : rule->validValues)
                {
                    float dist = std::abs(value - validVal);
                    if (dist < minDist)
                    {
                        minDist = dist;
                        nearest = validVal;
                    }
                }
                return nearest;
            }
            return value;

        case ParameterCategory::DangerousBinary:
            // Force to locked value (usually 0 = off)
            return rule->lockedValue;

        case ParameterCategory::DangerousSystem:
            // Force to default - don't touch system parameters
            return rule->defaultValue;

        case ParameterCategory::Unknown:
        default:
            // Pass through with clamp
            return juce::jlimit(0.0f, 1.0f, value);
    }
}

void ParameterSafetyConfig::sanitizeParameterVector(std::vector<float>& params) const
{
    for (size_t i = 0; i < params.size(); ++i)
    {
        params[i] = sanitizeValue(static_cast<int>(i), params[i]);
    }
}

float ParameterSafetyConfig::generateSafeRandomValue(int index, juce::Random& rng) const
{
    const auto* rule = getRule(index);

    if (!rule || !rule->randomizeEnabled)
    {
        // No rule or not safe to randomize - use default
        return rule ? rule->defaultValue : 0.5f;
    }

    switch (rule->category)
    {
        case ParameterCategory::SafeContinuous:
        {
            // Random value in safe range
            return rule->minValue + rng.nextFloat() * (rule->maxValue - rule->minValue);
        }

        case ParameterCategory::SafeDiscrete:
        {
            // Random selection from valid values
            if (!rule->validValues.empty())
            {
                int idx = rng.nextInt(static_cast<int>(rule->validValues.size()));
                return rule->validValues[idx];
            }
            return rule->defaultValue;
        }

        default:
            return rule->defaultValue;
    }
}

std::vector<float> ParameterSafetyConfig::generateSafeParameterVector(
    int totalParameters, juce::Random& rng, bool onlyRandomizeSafe) const
{
    std::vector<float> params(totalParameters, 0.5f);

    for (int i = 0; i < totalParameters; ++i)
    {
        const auto* rule = getRule(i);

        if (!rule)
        {
            // No rule for this parameter
            params[i] = onlyRandomizeSafe ? 0.5f : rng.nextFloat();
            continue;
        }

        if (rule->randomizeEnabled && isSafeToRandomize(i))
        {
            params[i] = generateSafeRandomValue(i, rng);
        }
        else
        {
            // Use safe default or locked value
            params[i] = rule->category == ParameterCategory::DangerousBinary
                ? rule->lockedValue
                : rule->defaultValue;
        }
    }

    return params;
}

// ============================================================================
// Plugin-Specific Profiles
// ============================================================================

ParameterSafetyConfig ParameterSafetyConfig::createFabFilterProQProfile(int numParameters)
{
    ParameterSafetyConfig config;

    // FabFilter Pro-Q 3/4 has ~700 parameters organized in bands
    // Band parameters follow a pattern: BandN_ParamName
    //
    // SAFE DSP PARAMETERS (to randomize):
    //   - BandN_Frequency (logarithmic: 10Hz - 30kHz)
    //   - BandN_Gain (linear: -30dB to +30dB, normalized)
    //   - BandN_Q (logarithmic: 0.025 to 40.0)
    //   - BandN_Slope (discrete: 6/12/24/48 dB/oct)
    //   - Band Type (discrete: Bell/Cut/Shelf/etc.)
    //
    // DANGEROUS BINARY PARAMETERS (to lock at 0):
    //   - BandN_Enable (1 = bypass this band!)
    //   - BandN_Solo (1 = only hear this band)
    //   - BandN_Mute (1 = silence this band)
    //   - Phase Invert
    //   - Global Bypass
    //   - Channel Mode (L/R/M/S)

    // Global dangerous parameters
    config.addRule({0, "Bypass", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});
    config.addRule({1, "Phase Invert", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});
    config.addRule({2, "Stereo Placement", ParameterCategory::DangerousBinary, 0, 1, 0.5f, true, {}, 0.5f});

    // Band parameters (Pro-Q typically has 24 bands, each with ~25 parameters)
    // Parameters per band: Enable, Freq, Gain, Q, Type, Slope, Solo, Mute, Stereo, etc.
    const int paramsPerBand = 25;
    const int numBands = 24;

    for (int band = 0; band < numBands; ++band)
    {
        int baseIndex = 10 + band * paramsPerBand; // Offset by global params

        // Band Enable - MUST stay at 1 (on) or we get silent bands
        config.addRule({baseIndex + 0,
                       "Band" + juce::String(band) + "_Enable",
                       ParameterCategory::DangerousBinary,
                       0, 1, 1.0f, true, {}, 1.0f}); // Lock to ON

        // Frequency - SAFE to randomize (logarithmic mapping)
        config.addRule({baseIndex + 1,
                       "Band" + juce::String(band) + "_Frequency",
                       ParameterCategory::SafeContinuous,
                       0.1f, 0.9f, 0.5f, true}); // Avoid extremes

        // Gain - SAFE to randomize
        config.addRule({baseIndex + 2,
                       "Band" + juce::String(band) + "_Gain",
                       ParameterCategory::SafeContinuous,
                       0.2f, 0.8f, 0.5f, true}); // Avoid extreme boosts/cuts

        // Q - SAFE to randomize (filter resonance)
        config.addRule({baseIndex + 3,
                       "Band" + juce::String(band) + "_Q",
                       ParameterCategory::SafeContinuous,
                       0.1f, 0.9f, 0.3f, true});

        // Filter Type - SAFE discrete
        config.addRule({baseIndex + 4,
                       "Band" + juce::String(band) + "_Type",
                       ParameterCategory::SafeDiscrete,
                       0, 1, 0, true,
                       {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f}}); // Bell, Cut, Shelf, etc.

        // Slope - SAFE discrete
        config.addRule({baseIndex + 5,
                       "Band" + juce::String(band) + "_Slope",
                       ParameterCategory::SafeDiscrete,
                       0, 1, 0.25f, true,
                       {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}}); // 6/12/24/48 dB/oct

        // Solo - DANGEROUS (mutes all other bands!)
        config.addRule({baseIndex + 6,
                       "Band" + juce::String(band) + "_Solo",
                       ParameterCategory::DangerousBinary,
                       0, 1, 0, true, {}, 0.0f}); // Lock to OFF

        // Mute - DANGEROUS
        config.addRule({baseIndex + 7,
                       "Band" + juce::String(band) + "_Mute",
                       ParameterCategory::DangerousBinary,
                       0, 1, 0, true, {}, 0.0f}); // Lock to OFF

        // Stereo placement - could be safe but risky
        config.addRule({baseIndex + 8,
                       "Band" + juce::String(band) + "_Stereo",
                       ParameterCategory::SafeContinuous,
                       0.0f, 1.0f, 0.5f, true});

        // Remaining band parameters - treat as unknown/safe by default
        for (int p = 9; p < paramsPerBand && baseIndex + p < numParameters; ++p)
        {
            config.addRule({baseIndex + p,
                           "Band" + juce::String(band) + "_Param" + juce::String(p),
                           ParameterCategory::SafeContinuous,
                           0.1f, 0.9f, 0.5f, true});
        }
    }

    // Spectrum/Analyzer settings (not audio, but don't affect sound)
    for (int i = numBands * paramsPerBand + 10; i < numParameters; ++i)
    {
        config.addRule({i, "UI_Param" + juce::String(i),
                       ParameterCategory::DangerousSystem,
                       0, 1, 0.5f, false, {}, 0.5f});
    }

    return config;
}

ParameterSafetyConfig ParameterSafetyConfig::createFabFilterProLProfile(int numParameters)
{
    ParameterSafetyConfig config;

    // FabFilter Pro-L 2 is a limiter
    // SAFE: Threshold, Attack, Release, Lookahead, Output Gain
    // DANGEROUS: Bypass, Mute, Link/Unlink channels

    // Bypass
    config.addRule({0, "Bypass", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});

    // Threshold - SAFE (main control)
    config.addRule({1, "Threshold", ParameterCategory::SafeContinuous, 0.0f, 1.0f, 0.5f, true});

    // Attack - SAFE
    config.addRule({2, "Attack", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.3f, true});

    // Release - SAFE
    config.addRule({3, "Release", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.4f, true});

    // Lookahead - SAFE discrete
    config.addRule({4, "Lookahead", ParameterCategory::SafeDiscrete, 0, 1, 0.5f, true,
                   {0.0f, 0.33f, 0.66f, 1.0f}}); // 0ms, 3ms, 5ms, 10ms

    // Output Gain - SAFE (but keep reasonable)
    config.addRule({5, "Output Gain", ParameterCategory::SafeContinuous, 0.3f, 0.7f, 0.5f, true});

    // Channel Link - SAFE
    config.addRule({6, "Channel Link", ParameterCategory::SafeContinuous, 0.0f, 1.0f, 0.5f, true});

    // Style - SAFE discrete
    config.addRule({7, "Style", ParameterCategory::SafeDiscrete, 0, 1, 0.5f, true,
                   {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}});

    // Oversampling - SAFE but affects CPU
    config.addRule({8, "Oversampling", ParameterCategory::SafeDiscrete, 0, 1, 0.25f, true,
                   {0.0f, 0.33f, 0.66f, 1.0f}}); // 1x, 2x, 4x, 8x

    // Remaining parameters - treat as safe with caution
    for (int i = 9; i < numParameters; ++i)
    {
        config.addRule({i, "Param" + juce::String(i),
                       ParameterCategory::SafeContinuous,
                       0.2f, 0.8f, 0.5f, true});
    }

    return config;
}

ParameterSafetyConfig ParameterSafetyConfig::createFabFilterVolcanoProfile(int numParameters)
{
    ParameterSafetyConfig config;

    // FabFilter Volcano 3 is a filter plugin
    // SAFE: Frequency, Resonance, Filter Type, Mix, Drive
    // DANGEROUS: Bypass, Mute, Solo

    // Bypass
    config.addRule({0, "Bypass", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});

    // Filter 1
    config.addRule({1, "Filter1_Frequency", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.5f, true});
    config.addRule({2, "Filter1_Resonance", ParameterCategory::SafeContinuous, 0.0f, 0.8f, 0.3f, true});
    config.addRule({3, "Filter1_Type", ParameterCategory::SafeDiscrete, 0, 1, 0, true,
                   {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}});
    config.addRule({4, "Filter1_Panning", ParameterCategory::SafeContinuous, 0.0f, 1.0f, 0.5f, true});

    // Filter 2
    config.addRule({5, "Filter2_Frequency", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.5f, true});
    config.addRule({6, "Filter2_Resonance", ParameterCategory::SafeContinuous, 0.0f, 0.8f, 0.3f, true});
    config.addRule({7, "Filter2_Type", ParameterCategory::SafeDiscrete, 0, 1, 0, true,
                   {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}});
    config.addRule({8, "Filter2_Panning", ParameterCategory::SafeContinuous, 0.0f, 1.0f, 0.5f, true});

    // Global
    config.addRule({9, "Mix", ParameterCategory::SafeContinuous, 0.0f, 1.0f, 1.0f, true});
    config.addRule({10, "Drive", ParameterCategory::SafeContinuous, 0.0f, 0.7f, 0.0f, true});
    config.addRule({11, "Output", ParameterCategory::SafeContinuous, 0.3f, 0.7f, 0.5f, true});

    // Remaining parameters
    for (int i = 12; i < numParameters; ++i)
    {
        config.addRule({i, "Param" + juce::String(i),
                       ParameterCategory::SafeContinuous,
                       0.2f, 0.8f, 0.5f, true});
    }

    return config;
}

ParameterSafetyConfig ParameterSafetyConfig::createGenericEQProfile(int numParameters)
{
    ParameterSafetyConfig config;

    // Generic EQ profile - assume first few are dangerous, rest are bands
    config.addRule({0, "Bypass", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});
    config.addRule({1, "Phase", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});
    config.addRule({2, "Output", ParameterCategory::SafeContinuous, 0.3f, 0.7f, 0.5f, true});

    // Assume remaining are band parameters (Frequency, Gain, Q patterns)
    for (int i = 3; i < numParameters; ++i)
    {
        config.addRule({i, "Param" + juce::String(i),
                       ParameterCategory::SafeContinuous,
                       0.1f, 0.9f, 0.5f, true});
    }

    return config;
}

ParameterSafetyConfig ParameterSafetyConfig::createGenericDynamicsProfile(int numParameters)
{
    ParameterSafetyConfig config;

    // Generic dynamics profile (compressor/limiter)
    config.addRule({0, "Bypass", ParameterCategory::DangerousBinary, 0, 1, 0, true, {}, 0.0f});
    config.addRule({1, "Threshold", ParameterCategory::SafeContinuous, 0.0f, 0.8f, 0.5f, true});
    config.addRule({2, "Ratio", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.3f, true});
    config.addRule({3, "Attack", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.3f, true});
    config.addRule({4, "Release", ParameterCategory::SafeContinuous, 0.1f, 0.9f, 0.4f, true});
    config.addRule({5, "Makeup", ParameterCategory::SafeContinuous, 0.2f, 0.7f, 0.5f, true});
    config.addRule({6, "Mix", ParameterCategory::SafeContinuous, 0.5f, 1.0f, 1.0f, true});

    for (int i = 7; i < numParameters; ++i)
    {
        config.addRule({i, "Param" + juce::String(i),
                       ParameterCategory::SafeContinuous,
                       0.2f, 0.8f, 0.5f, true});
    }

    return config;
}

ParameterSafetyConfig ParameterSafetyConfig::createAutoProfile(const juce::String& pluginName,
                                                               int numParameters)
{
    auto name = pluginName.toLowerCase();

    if (name.contains("pro-q") || name.contains("proq"))
        return createFabFilterProQProfile(numParameters);

    if (name.contains("pro-l") || name.contains("prol"))
        return createFabFilterProLProfile(numParameters);

    if (name.contains("volcano"))
        return createFabFilterVolcanoProfile(numParameters);

    if (name.contains("eq") || name.contains("equalizer") || name.contains("parametric"))
        return createGenericEQProfile(numParameters);

    if (name.contains("compressor") || name.contains("limiter") ||
        name.contains("gate") || name.contains("expander"))
        return createGenericDynamicsProfile(numParameters);

    // Default: assume all safe but conservative
    ParameterSafetyConfig config;
    for (int i = 0; i < numParameters; ++i)
    {
        config.addRule({i, "Param" + juce::String(i),
                       ParameterCategory::SafeContinuous,
                       0.2f, 0.8f, 0.5f, true});
    }
    return config;
}

// ============================================================================
// Serialization
// ============================================================================

nlohmann::json ParameterSafetyConfig::toJson() const
{
    nlohmann::json j;
    j["rules"] = nlohmann::json::array();

    for (const auto& rule : rules_)
    {
        j["rules"].push_back(rule.toJson());
    }

    j["version"] = 1;
    j["totalRules"] = static_cast<int>(rules_.size());

    return j;
}

ParameterSafetyConfig ParameterSafetyConfig::fromJson(const nlohmann::json& j)
{
    ParameterSafetyConfig config;

    if (j.contains("rules") && j["rules"].is_array())
    {
        for (const auto& ruleJson : j["rules"])
        {
            config.addRule(ParameterSafetyRule::fromJson(ruleJson));
        }
    }

    return config;
}

bool ParameterSafetyConfig::saveToFile(const juce::File& file) const
{
    auto json = toJson();
    std::ofstream ofs(file.getFullPathName().toStdString());
    if (!ofs.is_open())
        return false;

    ofs << json.dump(2);
    return true;
}

ParameterSafetyConfig ParameterSafetyConfig::loadFromFile(const juce::File& file)
{
    std::ifstream ifs(file.getFullPathName().toStdString());
    if (!ifs.is_open())
        return ParameterSafetyConfig();

    try
    {
        nlohmann::json j;
        ifs >> j;
        return fromJson(j);
    }
    catch (...)
    {
        return ParameterSafetyConfig();
    }
}

// ============================================================================
// Statistics
// ============================================================================

std::unordered_map<ParameterCategory, int> ParameterSafetyConfig::getCategoryCounts() const
{
    std::unordered_map<ParameterCategory, int> counts;

    for (const auto& rule : rules_)
    {
        counts[rule.category]++;
    }

    return counts;
}

std::vector<int> ParameterSafetyConfig::getSafeParameterIndices() const
{
    std::vector<int> indices;
    indices.reserve(rules_.size());

    for (const auto& rule : rules_)
    {
        if (rule.category == ParameterCategory::SafeContinuous ||
            rule.category == ParameterCategory::SafeDiscrete)
        {
            indices.push_back(rule.parameterIndex);
        }
    }

    std::sort(indices.begin(), indices.end());
    return indices;
}

std::vector<int> ParameterSafetyConfig::getDangerousParameterIndices() const
{
    std::vector<int> indices;
    indices.reserve(rules_.size());

    for (const auto& rule : rules_)
    {
        if (rule.category == ParameterCategory::DangerousBinary ||
            rule.category == ParameterCategory::DangerousSystem)
        {
            indices.push_back(rule.parameterIndex);
        }
    }

    std::sort(indices.begin(), indices.end());
    return indices;
}

// ============================================================================
// Private Helpers
// ============================================================================

bool ParameterSafetyConfig::isDangerousParameterName(const juce::String& name)
{
    auto lower = name.toLowerCase();

    // Binary switches that can silence output
    return lower.contains("bypass") ||
           lower.contains("mute") ||
           lower.contains("solo") ||
           lower.contains("enable") ||
           lower.contains("activate") ||
           lower.contains("phase") ||
           lower.contains("invert") ||
           lower.contains("link") ||
           lower.contains("power") ||
           lower.contains("on/off");
}

bool ParameterSafetyConfig::isSafeDSPParameterName(const juce::String& name)
{
    auto lower = name.toLowerCase();

    // Safe DSP controls
    return lower.contains("freq") ||
           lower.contains("gain") ||
           lower.contains("volume") ||
           lower.contains("pan") ||
           lower.contains("q") ||
           lower.contains("resonance") ||
           lower.contains("res") ||
           lower.contains("attack") ||
           lower.contains("release") ||
           lower.contains("decay") ||
           lower.contains("sustain") ||
           lower.contains("threshold") ||
           lower.contains("ratio") ||
           lower.contains("knee") ||
           lower.contains("mix") ||
           lower.contains("wet") ||
           lower.contains("dry") ||
           lower.contains("drive") ||
           lower.contains("saturation") ||
           lower.contains("feedback") ||
           lower.contains("delay") ||
           lower.contains("reverb") ||
           lower.contains("size") ||
           lower.contains("width");
}

bool ParameterSafetyConfig::parameterNameMatches(const juce::String& name, const juce::String& pattern)
{
    return name.equalsIgnoreCase(pattern) ||
           name.containsIgnoreCase(pattern) ||
           pattern.containsIgnoreCase(name);
}

} // namespace morphsnap
