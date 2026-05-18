#include "PluginProfileDB.h"
#include "AI/SemanticPluginProfile.h"
#include "Host/IPluginHostManager.h"
#include "Host/ParameterBridge.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace more_phi {

using json = nlohmann::json;

namespace {

std::string toStdString(const juce::String& value)
{
    return value.toStdString();
}

bool containsAny(const juce::String& lowerName, std::initializer_list<const char*> needles)
{
    for (const auto* needle : needles)
        if (lowerName.contains(needle))
            return true;
    return false;
}

juce::String getAutomationSafety(const ParameterBridge::ParameterDescriptor& descriptor)
{
    const auto lower = descriptor.name.toLowerCase();
    if (containsAny(lower, {"bypass", "mute", "solo", "panic", "reset"}))
        return "locked";

    if (containsAny(lower, {"assistant", "learn", "relearn", "analyse", "analyze"}))
        return "locked";

    if (containsAny(lower, {"quality", "oversampling", "mode"}))
        return descriptor.discrete ? "review" : "safe";

    if (containsAny(lower, {"output", "ceiling", "gain", "trim", "level"}))
        return "review";

    return "safe";
}

juce::String getParameterType(const ParameterBridge::ParameterDescriptor& descriptor)
{
    if (descriptor.boolean)
        return "binary";
    if (descriptor.discrete)
        return "discrete";
    return "continuous";
}

juce::String sanitizeProfileId(const juce::String& text)
{
    juce::String out;
    for (int i = 0; i < text.length(); ++i)
    {
        const auto c = text[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out += juce::String::charToString(c);
        else if (c == ' ' || c == '.' || c == '/')
            out += "_";
    }
    return out.trimCharactersAtStart("_").trimCharactersAtEnd("_");
}

} // namespace

juce::String PluginProfileDB::detectPluginKind(const juce::String& pluginName)
{
    const auto lower = pluginName.toLowerCase();
    if (lower.contains("ozone 12") || lower.contains("ozone12"))
        return "ozone_12";
    if (lower.contains("ozone 11") || lower.contains("ozone11"))
        return "ozone_11";
    if (lower.contains("neutron 5") || lower.contains("neutron5"))
        return "neutron_5";
    if (lower.contains("ozone"))
        return "ozone_generic";
    if (lower.contains("neutron"))
        return "neutron_generic";
    return "generic_vst3";
}

juce::String PluginProfileDB::makeProfileId(const juce::String& pluginName, int parameterCount)
{
    const auto kind = detectPluginKind(pluginName);
    const auto hash = juce::String::toHexString(pluginName.hashCode());
    return sanitizeProfileId(kind + "_" + hash + "_" + juce::String(parameterCount)).toLowerCase();
}

juce::String PluginProfileDB::buildAuditJson(IPluginHostManager& host, const ParameterBridge& bridge)
{
    const auto descriptors = bridge.getParameterDescriptors();

    const auto buildResult = [&descriptors](const juce::String& pluginName,
                                            bool acceptsMidi,
                                            bool producesMidi,
                                            int latencySamples) -> juce::String
    {
        const auto semanticControls = SemanticPluginProfile::classify(pluginName, descriptors);
        const auto detectedType = detectPluginKind(pluginName);
        const auto profileId = makeProfileId(pluginName, static_cast<int>(descriptors.size()));

        json parameters = json::array();
        int lockedCount = 0;
        int reviewCount = 0;

        for (const auto& descriptor : descriptors)
        {
            const auto safety = getAutomationSafety(descriptor);
            if (safety == "locked")
                ++lockedCount;
            else if (safety == "review")
                ++reviewCount;

            parameters.push_back({
                {"index", descriptor.index},
                {"stableId", toStdString(descriptor.stableId)},
                {"name", toStdString(descriptor.name)},
                {"type", toStdString(getParameterType(descriptor))},
                {"value", descriptor.value},
                {"displayValue", toStdString(descriptor.displayValue)},
                {"label", toStdString(descriptor.label)},
                {"discrete", descriptor.discrete},
                {"boolean", descriptor.boolean},
                {"numSteps", descriptor.numSteps},
                {"defaultValue", descriptor.defaultValue},
                {"automationSafety", toStdString(safety)}
            });
        }

        json result{
            {"success", true},
            {"schema_version", 1},
            {"semantic_schema_version", 1},
            {"profile_id", toStdString(profileId)},
            {"detected_type", toStdString(detectedType)},
            {"plugin", {
                {"name", toStdString(pluginName)},
                {"kind", toStdString(detectedType)},
                {"parameter_count", static_cast<int>(descriptors.size())},
                {"accepts_midi", acceptsMidi},
                {"produces_midi", producesMidi},
                {"latency_samples", latencySamples}
            }},
            {"safety", {
                {"locked_count", lockedCount},
                {"review_count", reviewCount},
                {"safe_count", static_cast<int>(descriptors.size()) - lockedCount - reviewCount}
            }},
            {"semantic_controls", SemanticPluginProfile::controlsToJson(semanticControls, descriptors)},
            {"semantic_control_count", static_cast<int>(semanticControls.size())},
            {"parameters", parameters}
        };

        return juce::String(result.dump());
    };

    auto* plugin = host.getPlugin();
    if (plugin == nullptr)
    {
        if (!descriptors.empty())
            return buildResult("test_hosted_plugin", false, false, 0);

        return juce::String(json{{"success", false}, {"error", "no_hosted_plugin"}}.dump());
    }

    return buildResult(plugin->getName(),
                       plugin->acceptsMidi(),
                       plugin->producesMidi(),
                       plugin->getLatencySamples());
}

juce::File PluginProfileDB::getProfileDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("More-Phi")
        .getChildFile("plugin_profiles");
}

juce::File PluginProfileDB::getProfileFile(const juce::String& profileId)
{
    return getProfileDirectory().getChildFile(sanitizeProfileId(profileId).toLowerCase() + ".json");
}

} // namespace more_phi
