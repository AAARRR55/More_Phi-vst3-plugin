#include "PluginSemanticMapper.h"

#include "PluginProfileDB.h"
#include "Core/ParameterClassifier.h"
#include "Host/IPluginHostManager.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace more_phi {

using json = nlohmann::json;

namespace {

struct SemanticRole
{
    juce::String id;
    juce::String role;
    juce::String family;
    juce::String unit;
    juce::String safeRange;
};

struct CapabilityCounts
{
    int eq = 0;
    int compressor = 0;
    int limiter = 0;
    int imager = 0;
    int saturation = 0;
};

std::string toStdString(const juce::String& value)
{
    return value.toStdString();
}

juce::String combinedSearchText(const ParameterBridge::ParameterDescriptor& descriptor)
{
    return (descriptor.name + " " + descriptor.label + " " + descriptor.displayValue).toLowerCase();
}

bool containsAny(const juce::String& text, std::initializer_list<const char*> needles)
{
    for (const auto* needle : needles)
        if (text.contains(needle))
            return true;
    return false;
}

bool containsWordQ(const juce::String& text)
{
    const auto s = text.toStdString();
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != 'q')
            continue;

        const bool leftOk = i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1]));
        const bool rightOk = i + 1 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 1]));
        if (leftOk && rightOk)
            return true;
    }

    return false;
}

int extractOrdinalAfterToken(const juce::String& text, const juce::String& token)
{
    const auto s = text.toStdString();
    const auto t = token.toStdString();
    size_t pos = s.find(t);

    while (pos != std::string::npos)
    {
        size_t i = pos + t.size();
        while (i < s.size() && (s[i] == ' ' || s[i] == '_' || s[i] == '-' || s[i] == '#'))
            ++i;

        int value = 0;
        bool hasDigit = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        {
            value = value * 10 + (s[i] - '0');
            hasDigit = true;
            ++i;
        }

        if (hasDigit)
            return value;

        pos = s.find(t, pos + 1);
    }

    return 0;
}

int extractBandNumber(const juce::String& text)
{
    if (const int band = extractOrdinalAfterToken(text, "band"); band > 0)
        return band;
    if (const int node = extractOrdinalAfterToken(text, "node"); node > 0)
        return node;
    return 0;
}

juce::String sanitizeSemanticId(const juce::String& input)
{
    const auto lower = input.toLowerCase();
    juce::String out;
    bool previousUnderscore = false;

    for (int i = 0; i < lower.length(); ++i)
    {
        const auto c = lower[i];
        const bool isAsciiAlphaNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (isAsciiAlphaNum)
        {
            out += juce::String::charToString(c);
            previousUnderscore = false;
        }
        else if (!previousUnderscore)
        {
            out += "_";
            previousUnderscore = true;
        }
    }

    out = out.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    return out.isNotEmpty() ? out : "parameter";
}

juce::String bandId(int band, const char* suffix)
{
    if (band > 0)
        return juce::String("band_") + juce::String(band) + "_" + suffix;
    return juce::String("eq_") + suffix;
}

juce::String widthBandPrefix(const juce::String& text)
{
    if (containsAny(text, {"sub", "bass"}))
        return "sub";
    if (containsAny(text, {"low"}))
        return "low";
    if (containsAny(text, {"mid"}))
        return "mid";
    if (containsAny(text, {"high", "air", "treble"}))
        return "high";
    return {};
}

SemanticRole classifyRole(const ParameterBridge::ParameterDescriptor& descriptor)
{
    const auto text = combinedSearchText(descriptor);
    const int band = extractBandNumber(text);
    const bool eqContext = containsAny(text, {"eq", "equalizer", "filter", "band ", "node "});
    const bool dynamicsContext = containsAny(text, {"compress", "compressor", "dynamics"});
    const bool limiterContext = containsAny(text, {"limit", "limiter", "maximizer", "maximise", "maximize", "true peak"});
    const bool imagerContext = containsAny(text, {"imager", "stereo", "width", "spread", "side", "mid/side", "m/s"});

    if (eqContext && containsAny(text, {"freq", "frequency", "cutoff", "hz"}))
        return {bandId(band, "frequency"), "eq_frequency", "eq", "Hz", "20 Hz to 20000 Hz"};

    if (eqContext && (containsWordQ(text) || containsAny(text, {"bandwidth", "resonance", "reson"})))
        return {bandId(band, "q"), "eq_q", "eq", "Q", "0.3 to 8.0"};

    if (eqContext && containsAny(text, {"gain", "db", "level"}))
        return {bandId(band, "gain"), "eq_gain", "eq", "dB", "-6 dB to +3 dB"};

    if (eqContext && containsAny(text, {"type", "shape", "slope"}))
        return {bandId(band, "filter_type"), "eq_filter_type", "eq", {}, {}};

    if (eqContext && containsAny(text, {"enable", "enabled", "active", "on/off"}))
        return {bandId(band, "enabled"), "eq_band_enabled", "eq", {}, {}};

    if (dynamicsContext && containsAny(text, {"threshold", "thresh"}))
        return {"compressor_threshold", "compressor_threshold", "compressor", "dB", {}};

    if (dynamicsContext && containsAny(text, {"ratio"}))
        return {"compressor_ratio", "compressor_ratio", "compressor", "ratio", "max 4:1 without confirmation"};

    if (dynamicsContext && containsAny(text, {"attack"}))
        return {"compressor_attack", "compressor_attack", "compressor", "ms", "5 ms or slower without confirmation"};

    if (dynamicsContext && containsAny(text, {"release"}))
        return {"compressor_release", "compressor_release", "compressor", "ms", {}};

    if (dynamicsContext && containsAny(text, {"knee"}))
        return {"compressor_knee", "compressor_knee", "compressor", "dB", {}};

    if (dynamicsContext && containsAny(text, {"makeup", "make up"}))
        return {"compressor_makeup_gain", "compressor_makeup_gain", "compressor", "dB", {}};

    if (limiterContext && containsAny(text, {"ceiling", "true peak"}))
        return {"limiter_ceiling", "limiter_ceiling", "limiter", "dBTP", "-2.0 dBTP to -0.8 dBTP"};

    if (limiterContext && containsAny(text, {"input", "drive", "gain"}))
        return {"limiter_input_gain", "limiter_input_gain", "limiter", "dB", "small steps only"};

    if (limiterContext && containsAny(text, {"output", "level"}))
        return {"limiter_output_level", "limiter_output_level", "limiter", "dB", {}};

    if (limiterContext && containsAny(text, {"release"}))
        return {"limiter_release", "limiter_release", "limiter", "ms", {}};

    if (imagerContext && containsAny(text, {"crossover", "cross over"}))
        return {"imager_crossover", "imager_crossover", "stereo_imager", "Hz", {}};

    if (imagerContext && containsAny(text, {"width", "spread"}))
    {
        const auto prefix = widthBandPrefix(text);
        const auto id = prefix.isNotEmpty() ? juce::String("imager_") + prefix + "_width" : "imager_width";
        const auto safeRange = (prefix == "sub" || prefix == "low") ? "max 1.0" : "avoid correlation below 0.1";
        return {id, "imager_width", "stereo_imager", "normalized", safeRange};
    }

    if (containsAny(text, {"saturation", "distortion", "tube", "tape", "exciter", "harmonic", "warmth"}))
    {
        if (containsAny(text, {"drive", "amount", "saturation", "exciter", "harmonic"}))
            return {"saturation_drive", "saturation_drive", "saturation", "normalized", "subtle amounts only"};
        if (containsAny(text, {"mix", "blend"}))
            return {"saturation_mix", "saturation_mix", "saturation", "percent", {}};
        return {"saturation_tone", "saturation_tone", "saturation", "normalized", {}};
    }

    if (containsAny(text, {"mix", "blend", "dry wet", "dry/wet"}))
        return {"mix", "mix", "generic", "percent", {}};

    return {juce::String("control_") + sanitizeSemanticId(descriptor.name), "generic_control", "generic", {}, {}};
}

juce::String safetyFor(const ParameterBridge::ParameterDescriptor& descriptor, const SemanticRole& role)
{
    const auto text = combinedSearchText(descriptor);

    if (containsAny(text, {"bypass", "mute", "solo", "panic", "reset", "random", "randomize",
                           "preset", "license", "account", "latency"}))
        return "blocked";

    if ((text.contains("midi") && text.contains("learn"))
        || containsAny(text, {"assistant", "analyse", "analyze", "global power", "power"}))
        return "blocked";

    if (containsAny(text, {"quality", "oversampling", "mode", "enable", "enabled", "active", "on/off"}))
        return "caution";

    if (role.role.startsWith("limiter_") || role.role == "imager_width" || role.role == "saturation_drive")
        return "caution";

    if (containsAny(text, {"output", "trim", "ceiling"}))
        return "caution";

    return "safe";
}

juce::String parameterTypeToString(ParameterType type)
{
    switch (type)
    {
        case ParameterType::Continuous:  return "continuous";
        case ParameterType::Discrete:    return "discrete";
        case ParameterType::Binary:      return "binary";
        case ParameterType::Frequency:   return "frequency";
        case ParameterType::Decibel:     return "decibel";
        case ParameterType::Percentage:  return "percentage";
        case ParameterType::Enumeration: return "enumeration";
        case ParameterType::Unknown:
        case ParameterType::Count:
        default:                         return "unknown";
    }
}

void countCapability(const SemanticRole& role, CapabilityCounts& counts)
{
    if (role.family == "eq")
        ++counts.eq;
    else if (role.family == "compressor")
        ++counts.compressor;
    else if (role.family == "limiter")
        ++counts.limiter;
    else if (role.family == "stereo_imager")
        ++counts.imager;
    else if (role.family == "saturation")
        ++counts.saturation;
}

json supportsFromCounts(const CapabilityCounts& counts)
{
    json supports = json::array();
    if (counts.eq > 0)
        supports.push_back("eq");
    if (counts.compressor > 0)
        supports.push_back("compressor");
    if (counts.limiter > 0)
        supports.push_back("limiter");
    if (counts.imager > 0)
        supports.push_back("stereo_imager");
    if (counts.saturation > 0)
        supports.push_back("saturation");
    if (supports.empty())
        supports.push_back("generic_parameter_control");
    return supports;
}

int supportedFamilyCount(const CapabilityCounts& counts)
{
    return (counts.eq > 0 ? 1 : 0)
        + (counts.compressor > 0 ? 1 : 0)
        + (counts.limiter > 0 ? 1 : 0)
        + (counts.imager > 0 ? 1 : 0)
        + (counts.saturation > 0 ? 1 : 0);
}

juce::String detectedType(const CapabilityCounts& counts, const juce::String& pluginKind)
{
    const int families = supportedFamilyCount(counts);
    if (families >= 3)
        return "mastering_suite";

    if ((pluginKind.contains("ozone") || pluginKind.contains("neutron")) && families >= 2)
        return "mastering_suite";

    if (counts.eq > 0 && families == 1)
        return "eq";
    if (counts.compressor > 0 && families == 1)
        return "compressor";
    if (counts.limiter > 0 && families == 1)
        return "limiter";
    if (counts.imager > 0 && families == 1)
        return "stereo_imager";
    if (counts.saturation > 0 && families == 1)
        return "saturation";

    return "generic";
}

float confidenceFor(const CapabilityCounts& counts, int controlCount, const juce::String& type)
{
    if (type == "mastering_suite")
        return 0.92f;

    const int families = supportedFamilyCount(counts);
    if (families == 0)
        return controlCount > 0 ? 0.35f : 0.0f;

    return juce::jlimit(0.0f, 1.0f, 0.70f + std::min(0.20f, static_cast<float>(controlCount) * 0.02f));
}

json rawParameterJson(const ParameterBridge::ParameterDescriptor& descriptor,
                      const ParameterMetadata& metadata)
{
    return {
        {"index", descriptor.index},
        {"stableId", toStdString(descriptor.stableId)},
        {"name", toStdString(descriptor.name)},
        {"value", descriptor.value},
        {"displayValue", toStdString(descriptor.displayValue)},
        {"label", toStdString(descriptor.label)},
        {"discrete", descriptor.discrete},
        {"boolean", descriptor.boolean},
        {"numSteps", descriptor.numSteps},
        {"defaultValue", descriptor.defaultValue},
        {"classifier_type", toStdString(parameterTypeToString(metadata.type))},
        {"classifier_category", std::string(metadata.category)}
    };
}

json controlJson(const ParameterBridge::ParameterDescriptor& descriptor,
                 const ParameterMetadata& metadata,
                 const SemanticRole& role,
                 const juce::String& safety)
{
    json item{
        {"semantic_id", toStdString(role.id)},
        {"param_id", descriptor.index},
        {"index", descriptor.index},
        {"stableId", toStdString(descriptor.stableId)},
        {"name", toStdString(descriptor.name)},
        {"role", toStdString(role.role)},
        {"family", toStdString(role.family)},
        {"safety", toStdString(safety)},
        {"current_value", descriptor.value},
        {"display_value", toStdString(descriptor.displayValue)},
        {"label", toStdString(descriptor.label)},
        {"discrete", descriptor.discrete},
        {"boolean", descriptor.boolean},
        {"num_steps", descriptor.numSteps},
        {"classifier_type", toStdString(parameterTypeToString(metadata.type))}
    };

    if (role.unit.isNotEmpty())
        item["unit"] = toStdString(role.unit);
    if (role.safeRange.isNotEmpty())
        item["safe_range"] = toStdString(role.safeRange);

    return item;
}

} // namespace

juce::String PluginSemanticMapper::buildSemanticMapJson(IPluginHostManager& host,
                                                        const ParameterBridge& bridge,
                                                        const ParameterClassifier& classifier,
                                                        const Options& options)
{
    const auto descriptors = bridge.getParameterDescriptors();
    if (descriptors.empty())
        return juce::String(json{{"success", false}, {"error", "no_hosted_parameters"}}.dump());

    const int maxControls = juce::jlimit(0, static_cast<int>(descriptors.size()), options.maxControls);
    auto* plugin = host.getPlugin();
    const juce::String pluginName = plugin != nullptr ? plugin->getName() : "current";
    const juce::String pluginKind = plugin != nullptr ? PluginProfileDB::detectPluginKind(pluginName) : "generic_vst3";
    const juce::String profileId = plugin != nullptr
        ? PluginProfileDB::makeProfileId(pluginName, static_cast<int>(descriptors.size()))
        : juce::String("current_") + juce::String(static_cast<int>(descriptors.size())) + "_parameters";

    json safeControls = json::array();
    json cautionControls = json::array();
    json blockedControls = json::array();
    json rawParameters = json::array();
    CapabilityCounts counts;
    int returned = 0;

    for (const auto& descriptor : descriptors)
    {
        if (returned >= maxControls)
            break;

        const auto metadata = classifier.getMetadata(descriptor.index);
        const auto role = classifyRole(descriptor);
        const auto safety = safetyFor(descriptor, role);
        auto control = controlJson(descriptor, metadata, role, safety);

        if (safety == "blocked")
            blockedControls.push_back(std::move(control));
        else if (safety == "caution")
            cautionControls.push_back(std::move(control));
        else
            safeControls.push_back(std::move(control));

        countCapability(role, counts);

        if (options.includeRawParameters)
            rawParameters.push_back(rawParameterJson(descriptor, metadata));

        ++returned;
    }

    const auto type = detectedType(counts, pluginKind);
    json result{
        {"success", true},
        {"schema_version", 1},
        {"plugin_id", "current"},
        {"profile_id", toStdString(profileId)},
        {"plugin", plugin != nullptr ? json{
            {"name", toStdString(pluginName)},
            {"kind", toStdString(pluginKind)},
            {"parameter_count", static_cast<int>(descriptors.size())},
            {"accepts_midi", plugin->acceptsMidi()},
            {"produces_midi", plugin->producesMidi()},
            {"latency_samples", plugin->getLatencySamples()}
        } : json(nullptr)},
        {"detected_type", toStdString(type)},
        {"confidence", confidenceFor(counts, returned, type)},
        {"supports", supportsFromCounts(counts)},
        {"safe_controls", safeControls},
        {"caution_controls", cautionControls},
        {"blocked_controls", blockedControls},
        {"returned_controls", returned},
        {"total_parameters", static_cast<int>(descriptors.size())}
    };

    if (options.includeRawParameters)
        result["raw_parameters"] = rawParameters;

    return juce::String(result.dump());
}

} // namespace more_phi
