#include "SemanticPluginProfile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace more_phi {

using json = nlohmann::json;

namespace {

std::string toStdString(const juce::String& value)
{
    return value.toStdString();
}

juce::String stableNameSearchText(const ParameterBridge::ParameterDescriptor& descriptor)
{
    return (descriptor.name + " " + descriptor.stableId).toLowerCase();
}

juce::String classificationSearchText(const ParameterBridge::ParameterDescriptor& descriptor)
{
    return (stableNameSearchText(descriptor) + " " + descriptor.label).toLowerCase();
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

juce::String sanitizeIdentifier(const juce::String& input)
{
    const auto lower = input.toLowerCase();
    juce::String output;
    bool previousSeparator = false;

    for (int i = 0; i < lower.length(); ++i)
    {
        const auto c = lower[i];
        const bool isAlphaNumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (isAlphaNumeric)
        {
            output += juce::String::charToString(c);
            previousSeparator = false;
        }
        else if (!previousSeparator)
        {
            output += "_";
            previousSeparator = true;
        }
    }

    output = output.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    return output.isNotEmpty() ? output : "parameter";
}

juce::String lockedKeywordFor(const juce::String& text)
{
    struct LockedToken
    {
        const char* token;
        const char* semantic;
    };

    static constexpr LockedToken tokens[] = {
        {"randomize", "randomize"},
        {"randomise", "randomize"},
        {"relearn", "relearn"},
        {"oversampling", "oversampling"},
        {"bypass", "bypass"},
        {"mute", "mute"},
        {"solo", "solo"},
        {"panic", "panic"},
        {"reset", "reset"},
        {"assistant", "assistant"},
        {"analyse", "analyze"},
        {"analyze", "analyze"},
        {"learn", "learn"},
        {"random", "random"},
        {"preset", "preset"},
        {"license", "license"},
        {"quality", "quality"},
        {"mode", "mode"}
    };

    for (const auto& token : tokens)
        if (text.contains(token.token))
            return token.semantic;

    return {};
}

int parseBandNumber(std::string_view digits) noexcept
{
    constexpr int maxReasonableBandNumber = 64;

    int value = 0;
    const auto* begin = digits.data();
    const auto* end = begin + digits.size();
    const auto result = std::from_chars(begin, end, value);

    if (result.ec != std::errc{} || result.ptr != end || value <= 0 || value > maxReasonableBandNumber)
        return 0;

    return value;
}

int extractBandNumber(const juce::String& text)
{
    const auto s = text.toStdString();
    std::smatch match;

    static const std::regex afterBandToken(R"((?:^|[^a-z0-9])(?:band|node)[\s_#-]*(\d+)(?:[^a-z0-9]|$))",
                                           std::regex::icase);
    if (std::regex_search(s, match, afterBandToken) && match.size() > 1)
        return parseBandNumber(match[1].str());

    static const std::regex beforeRoleToken(
        R"((?:^|[^a-z0-9])(\d+)[\s_#-]*(?:freq|frequency|cutoff|gain|level|db|trim|q|quality|resonance)(?:[^a-z0-9]|$))",
        std::regex::icase);
    if (std::regex_search(s, match, beforeRoleToken) && match.size() > 1)
        return parseBandNumber(match[1].str());

    return 0;
}

juce::String eqSemanticId(int band, const char* suffix)
{
    if (band > 0)
        return juce::String("eq.band_") + juce::String(band) + "." + suffix;

    return juce::String("eq.") + suffix;
}

SemanticControl makeControl(const ParameterBridge::ParameterDescriptor& descriptor,
                            juce::String semanticId,
                            juce::String role,
                            juce::String group,
                            juce::String unit,
                            juce::String safety,
                            float safeMin = 0.0f,
                            float safeMax = 1.0f,
                            float maxStepDb = 0.0f,
                            float minDeltaDb = 0.0f,
                            float maxDeltaDb = 0.0f)
{
    SemanticControl control;
    control.semanticId = std::move(semanticId);
    control.role = std::move(role);
    control.group = std::move(group);
    control.unit = std::move(unit);
    control.safety = std::move(safety);
    control.parameterIndex = descriptor.index;
    control.safeMin = safeMin;
    control.safeMax = safeMax;
    control.maxStepDb = maxStepDb;
    control.minDeltaDb = minDeltaDb;
    control.maxDeltaDb = maxDeltaDb;
    return control;
}

SemanticControl classifyOne(const ParameterBridge::ParameterDescriptor& descriptor)
{
    const auto text = classificationSearchText(descriptor);
    const int band = extractBandNumber(stableNameSearchText(descriptor));

    if (const auto lockedKeyword = lockedKeywordFor(text); lockedKeyword.isNotEmpty())
    {
        return makeControl(descriptor,
                           "locked." + lockedKeyword,
                           "locked_" + lockedKeyword,
                           "locked",
                           {},
                           "locked");
    }

    const bool hasOutputContext = containsAny(text, {"output", "global", "master"});
    const bool hasGainToken = containsAny(text, {"gain", "level", "db", "trim"});
    const bool hasMixToken = containsAny(text, {"mix", "blend", "dry wet", "dry/wet"});

    if (containsAny(text, {"ceiling", "true peak", "true-peak"}))
    {
        return makeControl(descriptor,
                           "limiter.ceiling",
                           "limiter_ceiling",
                           "limiter",
                           "dBTP",
                           "caution");
    }

    if (text.contains("input gain"))
    {
        return makeControl(descriptor,
                           "limiter.input_gain",
                           "limiter_input_gain",
                           "limiter",
                           "dB",
                           "safe",
                           0.0f,
                           1.0f,
                           3.0f,
                           -6.0f,
                           3.0f);
    }

    if (containsAny(text, {"limiter", "maximizer", "maximiser"}))
    {
        if (text.contains("release"))
            return makeControl(descriptor, "limiter.release", "limiter_release", "limiter", "ms", "caution");

        return makeControl(descriptor, "limiter.control", "limiter_control", "limiter", {}, "caution");
    }

    if (containsAny(text, {"width", "stereo", "imager"}))
    {
        return makeControl(descriptor,
                           "imager.width",
                           "imager_width",
                           "imager",
                           "normalized",
                           "caution");
    }

    if (containsAny(text, {"drive", "saturation", "exciter", "tone"}))
    {
        const auto suffix = containsAny(text, {"tone"}) ? juce::String("tone") : juce::String("drive");
        return makeControl(descriptor,
                           "saturation." + suffix,
                           "saturation_" + suffix,
                           "saturation",
                           "normalized",
                           "caution");
    }

    if (text.contains("threshold"))
        return makeControl(descriptor, "compressor.threshold", "compressor_threshold", "compressor", "dB", "safe");

    if (text.contains("ratio"))
        return makeControl(descriptor, "compressor.ratio", "compressor_ratio", "compressor", "ratio", "safe");

    if (text.contains("attack"))
        return makeControl(descriptor, "compressor.attack", "compressor_attack", "compressor", "ms", "safe");

    if (text.contains("release"))
        return makeControl(descriptor, "compressor.release", "compressor_release", "compressor", "ms", "safe");

    if (containsAny(text, {"makeup", "make up"}))
    {
        return makeControl(descriptor,
                           "compressor.makeup",
                           "compressor_makeup",
                           "compressor",
                           "dB",
                           "safe",
                           0.0f,
                           1.0f,
                           3.0f,
                           -6.0f,
                           3.0f);
    }

    if ((hasOutputContext && (hasGainToken || hasMixToken)) || text == "mix" || text == "gain")
    {
        const auto suffix = hasMixToken ? juce::String("mix") : juce::String("gain");
        return makeControl(descriptor,
                           "global." + suffix,
                           "global_" + suffix,
                           "global",
                           hasGainToken ? juce::String("dB") : juce::String("normalized"),
                           "caution");
    }

    if (containsAny(text, {"freq", "frequency", "hz", "cutoff"}))
    {
        return makeControl(descriptor,
                           eqSemanticId(band, "frequency"),
                           "eq_band_frequency",
                           "eq",
                           "Hz",
                           "safe");
    }

    if (containsWordQ(text) || text.contains("resonance"))
    {
        return makeControl(descriptor,
                           eqSemanticId(band, "q"),
                           "eq_band_q",
                           "eq",
                           "Q",
                           "safe");
    }

    if (hasGainToken)
    {
        return makeControl(descriptor,
                           eqSemanticId(band, "gain"),
                           "eq_band_gain",
                           "eq",
                           "dB",
                           "safe",
                           0.0f,
                           1.0f,
                           3.0f,
                           -6.0f,
                           3.0f);
    }

    return makeControl(descriptor,
                       "generic." + sanitizeIdentifier(descriptor.name),
                       "generic_control",
                       "generic",
                       {},
                       "safe");
}

json descriptorJson(const ParameterBridge::ParameterDescriptor& descriptor)
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
        {"defaultValue", descriptor.defaultValue}
    };
}

std::optional<double> parseFirstNumber(const juce::String& text)
{
    const auto s = text.toStdString();
    std::cmatch match;
    static const std::regex numberPattern(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+))");

    if (!std::regex_search(s.c_str(), match, numberPattern))
        return std::nullopt;

    try
    {
        return std::stod(match[0].str());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

const ParameterBridge::ParameterDescriptor* findDescriptor(const std::vector<ParameterBridge::ParameterDescriptor>& descriptors,
                                                           int parameterIndex)
{
    const auto it = std::find_if(descriptors.begin(), descriptors.end(), [parameterIndex](const auto& descriptor) {
        return descriptor.index == parameterIndex;
    });

    return it != descriptors.end() ? &(*it) : nullptr;
}

} // namespace

std::vector<SemanticControl> SemanticPluginProfile::classify(const std::vector<ParameterDescriptor>& descriptors)
{
    std::vector<SemanticControl> controls;
    controls.reserve(descriptors.size());

    std::map<std::string, int> usedSemanticIds;
    for (size_t i = 0; i < descriptors.size(); ++i)
    {
        auto control = classifyOne(descriptors[i]);
        const auto baseSemanticId = control.semanticId;
        auto candidate = baseSemanticId;

        if (usedSemanticIds[candidate.toStdString()] > 0)
        {
            const int suffixIndex = control.parameterIndex >= 0 ? control.parameterIndex : static_cast<int>(i);
            const auto stableSuffix = juce::String(".param_") + juce::String(suffixIndex);
            candidate = baseSemanticId + stableSuffix;

            int disambiguator = 2;
            while (usedSemanticIds[candidate.toStdString()] > 0)
            {
                candidate = baseSemanticId + stableSuffix + "_" + juce::String(disambiguator);
                ++disambiguator;
            }
        }

        control.semanticId = candidate;
        ++usedSemanticIds[candidate.toStdString()];
        controls.push_back(std::move(control));
    }

    return controls;
}

std::vector<SemanticControl> SemanticPluginProfile::classify(const juce::String& pluginName,
                                                             const std::vector<ParameterDescriptor>& descriptors)
{
    juce::ignoreUnused(pluginName);
    return classify(descriptors);
}

json SemanticPluginProfile::controlsToJson(const std::vector<ParameterDescriptor>& descriptors,
                                           const std::vector<SemanticControl>& controls)
{
    json items = json::array();

    for (const auto& control : controls)
    {
        const auto* descriptor = findDescriptor(descriptors, control.parameterIndex);
        if (descriptor == nullptr)
            continue;

        items.push_back({
            {"semantic_id", toStdString(control.semanticId)},
            {"role", toStdString(control.role)},
            {"group", toStdString(control.group)},
            {"parameter", descriptorJson(*descriptor)},
            {"unit", toStdString(control.unit)},
            {"safety", toStdString(control.safety)},
            {"safe_normalized_range", {
                {"min", control.safeMin},
                {"max", control.safeMax}
            }},
            {"action_limits", {
                {"max_step_db", control.maxStepDb},
                {"min_delta_db", control.minDeltaDb},
                {"max_delta_db", control.maxDeltaDb}
            }}
        });
    }

    return items;
}

json SemanticPluginProfile::controlsToJson(const std::vector<SemanticControl>& controls,
                                           const std::vector<ParameterDescriptor>& descriptors)
{
    return controlsToJson(descriptors, controls);
}

const SemanticControl* SemanticPluginProfile::findControl(const std::vector<SemanticControl>& controls,
                                                          const juce::String& semanticId)
{
    const auto it = std::find_if(controls.begin(), controls.end(), [&semanticId](const auto& control) {
        return control.semanticId == semanticId;
    });

    return it != controls.end() ? &(*it) : nullptr;
}

SemanticActionPlan SemanticPluginProfile::planSafeAction(const juce::var& params,
                                                         const std::vector<ParameterDescriptor>& descriptors,
                                                         const std::vector<SemanticControl>& controls,
                                                         const ParameterBridge* bridge)
{
    SemanticActionPlan plan;
    plan.success = false;
    plan.error = "invalid_params";
    plan.message = "plugin_profile.apply_safe_action requires an action object.";
    plan.actionJson = json::object();

    const auto actionVar = params.getProperty("action", juce::var());
    if (!actionVar.isObject())
        return plan;

    const auto actionType = actionVar.getProperty("type", "").toString();
    const auto semanticId = actionVar.getProperty("semantic_id", actionVar.getProperty("semanticId", "")).toString();
    plan.actionJson = {
        {"type", actionType.toStdString()},
        {"semantic_id", semanticId.toStdString()}
    };

    const SemanticControl* control = nullptr;
    int matchingControls = 0;
    for (const auto& candidate : controls)
    {
        if (candidate.semanticId != semanticId)
            continue;

        control = &candidate;
        ++matchingControls;
    }

    if (matchingControls == 0)
    {
        plan.error = "semantic_control_not_found";
        plan.message = "No semantic control matched the requested semantic_id.";
        return plan;
    }

    if (matchingControls > 1)
    {
        plan.error = "semantic_control_ambiguous";
        plan.message = "Multiple semantic controls matched the requested semantic_id.";
        plan.actionJson["match_count"] = matchingControls;
        return plan;
    }

    plan.actionJson["parameter_index"] = control->parameterIndex;
    plan.actionJson["role"] = control->role.toStdString();
    plan.actionJson["safety"] = control->safety.toStdString();

    if (control->safety == "locked")
    {
        plan.error = "control_locked";
        plan.message = "The requested semantic control is locked and cannot be changed safely.";
        return plan;
    }

    const bool allowCaution = static_cast<bool>(params.getProperty("allow_caution", false));
    if (control->safety == "caution" && !allowCaution)
    {
        plan.error = "caution_requires_confirmation";
        plan.message = "The requested semantic control requires allow_caution=true.";
        return plan;
    }

    auto buildSetPlan = [&plan, control](float normalizedValue)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);
        plan.success = true;
        plan.error = {};
        plan.message = {};
        plan.commands.push_back({control->parameterIndex, clamped});
        plan.actionJson["normalized_value"] = clamped;
    };

    if (actionType == "set_semantic_normalized")
    {
        if (!actionVar.hasProperty("normalized_value"))
        {
            plan.error = "invalid_params";
            plan.message = "set_semantic_normalized requires normalized_value.";
            return plan;
        }

        const float requestedValue = static_cast<float>(actionVar.getProperty("normalized_value", 0.0));
        if (!std::isfinite(requestedValue) || requestedValue < control->safeMin || requestedValue > control->safeMax)
        {
            plan.error = "value_out_of_safe_range";
            plan.message = "normalized_value is outside the semantic control safe range.";
            plan.actionJson["requested_normalized_value"] = requestedValue;
            plan.actionJson["safe_normalized_range"] = {{"min", control->safeMin}, {"max", control->safeMax}};
            return plan;
        }

        buildSetPlan(requestedValue);
        return plan;
    }

    if (actionType == "eq_gain_delta_db")
    {
        if (control->role != "eq_band_gain")
        {
            plan.error = "unsupported_action_for_control";
            plan.message = "eq_gain_delta_db can only target eq_band_gain controls.";
            return plan;
        }

        if (!actionVar.hasProperty("delta_db"))
        {
            plan.error = "invalid_params";
            plan.message = "eq_gain_delta_db requires delta_db.";
            return plan;
        }

        const float deltaDb = static_cast<float>(actionVar.getProperty("delta_db", 0.0));
        plan.actionJson["delta_db"] = deltaDb;
        if (!std::isfinite(deltaDb) || deltaDb < -6.0f || deltaDb > 3.0f)
        {
            plan.error = "delta_out_of_safe_range";
            plan.message = "delta_db is outside the allowed [-6.0, +3.0] range.";
            return plan;
        }

        const auto* descriptor = findDescriptor(descriptors, control->parameterIndex);
        if (descriptor == nullptr || bridge == nullptr)
        {
            plan.error = "unsupported_unit_conversion";
            plan.message = "Current display value is unavailable for dB conversion.";
            return plan;
        }

        const auto currentDb = parseFirstNumber(descriptor->displayValue);
        if (!currentDb)
        {
            plan.error = "unsupported_unit_conversion";
            plan.message = "Current display value could not be parsed as dB.";
            return plan;
        }

        const double targetDb = *currentDb + static_cast<double>(deltaDb);
        auto valueToDb = [bridge, control](float normalized) -> std::optional<double>
        {
            const auto text = bridge->getParameterDisplayValueAtNormalized(control->parameterIndex, normalized);
            return parseFirstNumber(text);
        };

        const auto dbAtZero = valueToDb(0.0f);
        const auto dbAtOne = valueToDb(1.0f);
        if (!dbAtZero || !dbAtOne || std::abs(*dbAtOne - *dbAtZero) < 1.0e-6)
        {
            plan.error = "unsupported_unit_conversion";
            plan.message = "Display values could not be sampled as a monotonic dB range.";
            return plan;
        }

        const bool ascending = *dbAtOne > *dbAtZero;
        float low = 0.0f;
        float high = 1.0f;
        float bestNormalized = juce::jlimit(0.0f, 1.0f, descriptor->value);
        double bestDistance = std::numeric_limits<double>::max();

        for (int i = 0; i < 28; ++i)
        {
            const float mid = (low + high) * 0.5f;
            const auto sampledDb = valueToDb(mid);
            if (!sampledDb)
            {
                plan.error = "unsupported_unit_conversion";
                plan.message = "Display value samples could not be parsed as dB.";
                plan.commands.clear();
                return plan;
            }

            const double distance = std::abs(*sampledDb - targetDb);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestNormalized = mid;
            }

            if ((ascending && *sampledDb < targetDb) || (!ascending && *sampledDb > targetDb))
                low = mid;
            else
                high = mid;
        }

        if (bestNormalized < control->safeMin || bestNormalized > control->safeMax)
        {
            plan.error = "value_out_of_safe_range";
            plan.message = "The target dB value maps outside the semantic control safe range.";
            plan.actionJson["target_db"] = targetDb;
            plan.actionJson["mapped_normalized_value"] = bestNormalized;
            plan.actionJson["safe_normalized_range"] = {{"min", control->safeMin}, {"max", control->safeMax}};
            return plan;
        }

        buildSetPlan(bestNormalized);
        plan.actionJson["current_db"] = *currentDb;
        plan.actionJson["target_db"] = targetDb;
        plan.actionJson["conversion_error_db"] = bestDistance;
        return plan;
    }

    plan.error = "unsupported_action";
    plan.message = "Unsupported semantic action type.";
    return plan;
}

} // namespace more_phi
