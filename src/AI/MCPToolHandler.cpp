/*
 * MorphSnap — AI/MCPToolHandler.cpp
 * Instance-aware MCP tool dispatch.
 *
 * All JSON responses are constructed with nlohmann::json so that special
 * characters in plugin parameter names (e.g. quotes, backslashes) are
 * correctly escaped without manual handling.
 */
#include "MCPToolHandler.h"
#include "InstanceIdentity.h"
#include "InstanceRegistry.h"
#include "Plugin/PluginProcessor.h"
#include "MCPToolsExtended.h"
#include <nlohmann/json.hpp>
#include <chrono>

namespace morphsnap {

using json = nlohmann::json;

// Serialize a nlohmann::json object to juce::String
static juce::String toJString(const json& j)
{
    return juce::String(j.dump());
}

// Extract a parameter index from a request that may use either "id" or "index" as key.
// Returns -1 if neither key is present.
static int extractParamId(const juce::var& params) noexcept
{
    if (params.hasProperty("id"))    return static_cast<int>(params.getProperty("id",    -1));
    if (params.hasProperty("index")) return static_cast<int>(params.getProperty("index", -1));
    return -1;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorphSnapProcessor& p,
                                     const InstanceIdentity& identity)
{
    if (method == "get_plugin_info")      return getPluginInfo(p);
    if (method == "list_parameters")      return listParameters(params, p);
    if (method == "get_parameter")        return getParameter(params, p);
    if (method == "set_parameter")        return setParameter(params, p);
    if (method == "set_parameters_batch") return setParametersBatch(params, p);
    if (method == "capture_snapshot")     return captureSnapshot(params, p);
    if (method == "recall_snapshot")      return recallSnapshot(params, p);
    if (method == "set_morph_position")   return setMorphPosition(params, p);
    if (method == "get_morph_state")      return getMorphState(p);

    // Extended AI Tools
    if (method == "analyze_parameters")             return MCPToolsExtended::analyzeParameters(params, p, p.getParameterClassifier());
    if (method == "expose_parameters")              return MCPToolsExtended::exposeParameters(params, p, p.getParameterClassifier());
    if (method == "get_token_estimate")             return MCPToolsExtended::getTokenEstimate(params, p.getTokenOptimizer());
    if (method == "set_parameters_optimized")       return MCPToolsExtended::setParametersOptimized(params, p, p.getTokenOptimizer());
    if (method == "get_morph_compatibility")        return MCPToolsExtended::getMorphCompatibility(params, p, p.getParameterClassifier());
    if (method == "suggest_intermediate_snapshots") return MCPToolsExtended::suggestIntermediateSnapshots(params, p, p.getParameterClassifier());
    if (method == "get_parameter_categories")       return MCPToolsExtended::getParameterCategories(params, p, p.getParameterClassifier());
    if (method == "learn_from_adjustment")          return MCPToolsExtended::learnFromAdjustment(params, p.getParameterClassifier());
    if (method == "get_learn_mode_status")          return MCPToolsExtended::getLearnModeStatus(params, p.getParameterClassifier());
    if (method == "set_learn_mode_config")          return MCPToolsExtended::setLearnModeConfig(params, p.getParameterClassifier());
    if (method == "reset_learning_data")            return MCPToolsExtended::resetLearningData(params, p.getParameterClassifier());
    if (method == "get_discrete_parameters")        return MCPToolsExtended::getDiscreteParameters(params, p, p.getParameterClassifier());
    if (method == "suggest_morph_settings")         return MCPToolsExtended::suggestMorphSettings(params, p, p.getParameterClassifier());
    if (method == "get_usage_stats")                return MCPToolsExtended::getUsageStats(params, p.getTokenOptimizer());
    if (method == "set_token_budget")               return MCPToolsExtended::setTokenBudget(params, p.getTokenOptimizer());
    if (method == "explain_parameter")              return MCPToolsExtended::explainParameter(params, p, p.getParameterClassifier());
    if (method == "find_related_parameters")        return MCPToolsExtended::findRelatedParameters(params, p, p.getParameterClassifier());
    if (method == "generate_dataset")               return MCPToolsExtended::generateDataset(params, p);

    // Multi-instance tools
    if (method == "get_instance_info")    return getInstanceInfo(identity);
    if (method == "list_instances")       return listInstances();

    return toJString(json{{"error","unknown_method"}});
}

// ── Tool implementations ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getPluginInfo(MorphSnapProcessor& p)
{
    json result = {
        {"name",    "MorphSnap"},
        {"type",    "effect"},
        {"version", "3.3.0"}
    };

    auto* plugin = p.getHostManager().getPlugin();
    if (plugin)
    {
        result["hostedPlugin"] = {
            {"name",       plugin->getName().toStdString()},
            {"type",       plugin->acceptsMidi() ? "instrument" : "effect"},
            {"paramCount", static_cast<int>(plugin->getParameters().size())}
        };
    }
    else
    {
        result["hostedPlugin"] = nullptr;
    }

    return toJString(result);
}

juce::String MCPToolHandler::listParameters(const juce::var& /*params*/, MorphSnapProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const int count = bridge.getParameterCount();

    json arr = json::array();
    for (int i = 0; i < count; ++i)
    {
        arr.push_back({
            {"id",    i},
            {"name",  bridge.getParameterName(i).toStdString()},
            {"value", bridge.getParameterNormalized(i)}
        });
    }
    return toJString(arr);
}

juce::String MCPToolHandler::getParameter(const juce::var& params, MorphSnapProcessor& p)
{
    const int id = extractParamId(params);
    auto& bridge = p.getParameterBridge();
    if (id < 0 || id >= bridge.getParameterCount())
        return toJString(json{{"error","invalid_param_id"}});

    return toJString(json{
        {"id",    id},
        {"name",  bridge.getParameterName(id).toStdString()},
        {"value", bridge.getParameterNormalized(id)}
    });
}

juce::String MCPToolHandler::setParameter(const juce::var& params, MorphSnapProcessor& p)
{
    const int id = extractParamId(params);
    const float value = static_cast<float>(params.getProperty("value", 0.0));

    auto& bridge = p.getParameterBridge();
    if (id < 0 || id >= bridge.getParameterCount())
        return toJString(json{{"success",false},{"error","invalid_param_id"}});

    // CRITICAL (Finding 2): Route through command queue for thread safety.
    // This serializes MCP thread → audio thread, preventing data race with
    // hosted plugin's setParameter() which is not thread-safe.
    // Trade-off: Changes only apply when audio is playing, but this is
    // better than crashing due to concurrent plugin API calls.
    p.enqueueParameterSet(id, value);

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(1);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "set_parameter";
    optimizer.recordUsage(usage);

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::setParametersBatch(const juce::var& params, MorphSnapProcessor& p)
{
    const juce::var batchPayload = params.hasProperty("params")
        ? params.getProperty("params", juce::var())
        : params.getProperty("parameters", juce::var());
    auto* list = batchPayload.getArray();
    if (!list)
        return toJString(json{{"success",false},{"error","missing params/parameters array"}});

    auto& bridge = p.getParameterBridge();
    const int maxParamCount = bridge.getParameterCount();
    int applied = 0;

    // CRITICAL (Finding 2): Route all parameter changes through the command queue
    // for thread safety. This prevents concurrent plugin API calls from MCP thread
    // and audio thread.
    for (const auto& item : *list)
    {
        const int id      = item.getProperty("id",    -1);
        const float value = static_cast<float>(item.getProperty("value", 0.0));
        if (id >= 0 && id < maxParamCount)
        {
            p.enqueueParameterSet(id, value);
            ++applied;
        }
    }

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(applied);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "set_parameters_batch";
    optimizer.recordUsage(usage);

    return toJString(json{
        {"success", true},
        {"applied", applied}
    });
}

juce::String MCPToolHandler::captureSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    p.getSnapshotBank().capture(slot, p.getParameterBridge());
    return toJString(json{{"success",true},{"slot",slot}});
}

juce::String MCPToolHandler::recallSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    // Get snapshot values and route through command queue for thread safety.
    // CRITICAL (Finding 2): Must use enqueueParameterState() instead of direct
    // applyParameterState() to prevent concurrent plugin API calls from
    // MCP thread and audio thread (undefined behavior).
    std::vector<float> values;
    if (!p.getSnapshotBank().getSlotValuesCopy(slot, values))
        return toJString(json{{"success",false},{"error","empty_slot"}});

    const int queued = p.enqueueParameterState(values);
    if (queued == 0)
        return toJString(json{{"success",false},{"error","queue_full"}});

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(queued);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "recall_snapshot";
    optimizer.recordUsage(usage);

    return toJString(json{{"success",true},{"slot",slot},{"queued",queued}});
}

juce::String MCPToolHandler::setMorphPosition(const juce::var& params, MorphSnapProcessor& p)
{
    bool sourceExplicitlySet = false;

    if (params.hasProperty("source"))
    {
        auto source = params.getProperty("source", "").toString().trim().toLowerCase();
        if (source == "xy" || source == "xypad")
        {
            p.setMorphSource(0);
            sourceExplicitlySet = true;
        }
        else if (source == "fader")
        {
            p.setMorphSource(1);
            sourceExplicitlySet = true;
        }
        else
        {
            return toJString(json{{"success",false},{"error","invalid_source"}});
        }
    }

    if (params.hasProperty("x"))
    {
        p.setMorphX(juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(params.getProperty("x", 0.5))));
        if (!sourceExplicitlySet)
            p.setMorphSource(0);
    }

    if (params.hasProperty("y"))
    {
        p.setMorphY(juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(params.getProperty("y", 0.5))));
        if (!sourceExplicitlySet)
            p.setMorphSource(0);
    }

    if (params.hasProperty("fader"))
    {
        p.setFaderPos(juce::jlimit(0.0f, 1.0f,
                      static_cast<float>(params.getProperty("fader", 0.0))));
        p.setMorphSource(1);
    }

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::getMorphState(MorphSnapProcessor& p)
{
    return toJString(json{
        {"x",      p.getMorphX()},
        {"y",      p.getMorphY()},
        {"fader",  p.getFaderPos()},
        {"source", p.getMorphSource()}
    });
}

// ── Multi-instance tools ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getInstanceInfo(const InstanceIdentity& id)
{
    return toJString(json{
        {"instanceId",  id.instanceId.toStdString()},
        {"morphCode",   id.morphCode.toStdString()},
        {"port",        id.port},
        {"bearerToken", id.bearerToken.toStdString()},
        {"createdAt",   id.createdAt}
    });
}

juce::String MCPToolHandler::listInstances()
{
    auto instances = InstanceRegistry::getInstance().getAllInstances();

    json arr = json::array();
    for (const auto& inst : instances)
    {
        arr.push_back({
            {"instanceId", inst.instanceId.toStdString()},
            {"morphCode",  inst.morphCode.toStdString()},
            {"port",       inst.port},
            {"createdAt",  inst.createdAt}
            // bearerToken intentionally omitted (redacted)
        });
    }
    return toJString(arr);
}

} // namespace morphsnap
