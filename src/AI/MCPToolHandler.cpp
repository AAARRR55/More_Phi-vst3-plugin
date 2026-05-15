/*
 * More-Phi — AI/MCPToolHandler.cpp
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
#include "MCPEQTool.h"
#include "OzoneParameterMap.h"
#include "ChainPlanExecutor.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <optional>

namespace more_phi {

using json = nlohmann::json;

// Serialize a nlohmann::json object to juce::String
static juce::String toJString(const json& j)
{
    return juce::String(j.dump());
}

// Extract a parameter index from a request that may use either "id" or "index" as key.
// B-9 FIX: Returns std::optional<int> — callers can't accidentally use -1 as a valid index.
// If maxParamCount > 0, the result is also bounds-checked against it.
static std::optional<int> extractParamId(const juce::var& params, int maxParamCount = 0) noexcept
{
    int id = -1;
    if (params.hasProperty("id"))         id = static_cast<int>(params.getProperty("id",    -1));
    else if (params.hasProperty("index")) id = static_cast<int>(params.getProperty("index", -1));
    else                                  return std::nullopt;

    if (id < 0) return std::nullopt;
    if (maxParamCount > 0 && id >= maxParamCount) return std::nullopt;
    return id;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorePhiProcessor& p,
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
    if (method == "generate_dataset_v2")            return MCPToolsExtended::generateDatasetV2(params, p);
    if (method == "generate_dataset_v3")            return MCPToolsExtended::generateDatasetV3(params, p);

    // EQ Assistant Tools (Natural Language EQ Control)
    if (method == "eq_adjust")                      return MCPEQTool::adjustEQ(params, p, p.getAIAssistant()).jsonResult;
    if (method == "eq_preview")                     return MCPEQTool::previewEQ(params, p, p.getAIAssistant()).jsonResult;
    if (method == "eq_apply")                       return MCPEQTool::applyEQ(params, p).jsonResult;
    if (method == "eq_reject")                      return MCPEQTool::rejectEQ(params, p).jsonResult;
    if (method == "eq_context")                     return MCPEQTool::getContext(params, p).jsonResult;
    if (method == "eq_reset_context")               return MCPEQTool::resetContext(params, p).jsonResult;
    if (method == "eq_validate")                    return MCPEQTool::validateEQ(params, p).jsonResult;
    if (method == "eq_suggest")                     return MCPEQTool::suggestEQ(params, p, p.getAIAssistant()).jsonResult;

    // Multi-instance tools
    if (method == "get_instance_info")    return getInstanceInfo(identity);
    if (method == "list_instances")       return listInstances();

    // Ozone mastering tools
    if (method == "get_mastering_state")  return getMasteringState(p);
    if (method == "apply_mastering_plan") return applyMasteringPlan(params, p);

    return toJString(json{{"error","unknown_method"}});
}

// ── Tool implementations ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getPluginInfo(MorePhiProcessor& p)
{
    json result = {
        {"name",    "More-Phi"},
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

juce::String MCPToolHandler::listParameters(const juce::var& /*params*/, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto descriptors = bridge.getParameterDescriptors();

    json arr = json::array();
    for (const auto& descriptor : descriptors)
    {
        arr.push_back({
            {"id",           descriptor.index},
            {"index",        descriptor.index},
            {"stableId",     descriptor.stableId.toStdString()},
            {"name",         descriptor.name.toStdString()},
            {"value",        descriptor.value},
            {"displayValue", descriptor.displayValue.toStdString()},
            {"label",        descriptor.label.toStdString()},
            {"discrete",     descriptor.discrete},
            {"boolean",      descriptor.boolean},
            {"numSteps",     descriptor.numSteps},
            {"defaultValue", descriptor.defaultValue}
        });
    }
    return toJString(arr);
}

juce::String MCPToolHandler::getParameter(const juce::var& params, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto stableId = params.getProperty("stableId",
                          params.getProperty("stable_id", "")).toString();
    const auto idOpt = extractParamId(params);
    const auto name = params.getProperty("name", "").toString();
    const auto resolution = bridge.resolveParameter(stableId, idOpt.value_or(-1), name);
    if (!resolution.success)
        return toJString(json{{"error","invalid_param_id"}});
    const auto descriptor = bridge.getParameterDescriptor(resolution.index);

    return toJString(json{
        {"id",           descriptor.index},
        {"index",        descriptor.index},
        {"stableId",     descriptor.stableId.toStdString()},
        {"name",         descriptor.name.toStdString()},
        {"value",        descriptor.value},
        {"displayValue", descriptor.displayValue.toStdString()},
        {"label",        descriptor.label.toStdString()},
        {"discrete",     descriptor.discrete},
        {"boolean",      descriptor.boolean},
        {"numSteps",     descriptor.numSteps},
        {"defaultValue", descriptor.defaultValue}
    });
}

juce::String MCPToolHandler::setParameter(const juce::var& params, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto stableId = params.getProperty("stableId",
                          params.getProperty("stable_id", "")).toString();
    const auto idOpt = extractParamId(params);
    const auto name = params.getProperty("name", "").toString();
    const auto resolution = bridge.resolveParameter(stableId, idOpt.value_or(-1), name);
    if (!resolution.success)
        return toJString(json{
            {"success", false},
            {"error", resolution.error.toStdString()},
            {"queued", 0},
            {"rejected", 1}
        });
    const int id = resolution.index;
    const float value = static_cast<float>(params.getProperty("value", 0.0));

    // CRITICAL (Finding 2): Route through command queue for thread safety.
    // This serializes MCP thread → audio thread, preventing data race with
    // hosted plugin's setParameter() which is not thread-safe.
    // Trade-off: Changes only apply when audio is playing, but this is
    // better than crashing due to concurrent plugin API calls.
    if (!p.enqueueParameterSet(id, value, MorePhiProcessor::ParameterEditSource::MCP, true))
        return toJString(json{
            {"success", false},
            {"error", "queue_full"},
            {"index", id},
            {"value", value},
            {"queued", 0},
            {"rejected", 0}
        });

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(1);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "set_parameter";
    optimizer.recordUsage(usage);

    return toJString(json{
        {"success", true},
        {"index", id},
        {"value", value},
        {"queued", 1},
        {"rejected", 0}
    });
}

juce::String MCPToolHandler::setParametersBatch(const juce::var& params, MorePhiProcessor& p)
{
    const juce::var batchPayload = params.hasProperty("params")
        ? params.getProperty("params", juce::var())
        : params.getProperty("parameters", juce::var());
    auto* list = batchPayload.getArray();
    if (!list)
        return toJString(json{{"success",false},{"error","missing params/parameters array"}});

    auto& bridge = p.getParameterBridge();
    const int requested = list->size();
    int applied = 0;
    int rejected = 0;
    int queueFailures = 0;

    // CRITICAL (Finding 2): Route all parameter changes through the command queue
    // for thread safety. This prevents concurrent plugin API calls from MCP thread
    // and audio thread.
    for (const auto& item : *list)
    {
        const int rawId   = item.hasProperty("index")
            ? static_cast<int>(item.getProperty("index", -1))
            : static_cast<int>(item.getProperty("id", -1));
        const auto stableId = item.getProperty("stableId",
                              item.getProperty("stable_id", "")).toString();
        const auto name = item.getProperty("name", "").toString();
        const float value = static_cast<float>(item.getProperty("value", 0.0));
        const auto resolution = bridge.resolveParameter(stableId, rawId, name);
        if (resolution.success)
        {
            if (p.enqueueParameterSet(resolution.index, value,
                                      MorePhiProcessor::ParameterEditSource::MCP,
                                      true))
                ++applied;
            else
                ++queueFailures;
        }
        else
        {
            ++rejected;
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

    const bool allQueued = requested > 0
        && applied == requested
        && rejected == 0
        && queueFailures == 0;

    json response{
        {"success", allQueued},
        {"queued", applied},
        {"applied", applied},
        {"requested", requested},
        {"rejected", rejected},
        {"queueFailures", queueFailures}
    };

    if (queueFailures > 0)
        response["error"] = "queue_full";
    else if (requested == 0)
        response["error"] = "empty_batch";
    else if (applied == 0)
        response["error"] = "no_parameters_queued";
    else if (rejected > 0)
        response["error"] = "partial_rejected";

    return toJString(response);
}

juce::String MCPToolHandler::captureSnapshot(const juce::var& params, MorePhiProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    const bool includeState = params.getProperty("includeState",
                              params.getProperty("include_state", true));
    if (!p.captureSnapshotToSlot(slot, includeState))
        return toJString(json{{"success",false},{"error","capture_failed"},{"slot",slot}});

    return toJString(json{
        {"success", true},
        {"slot", slot},
        {"includeState", includeState},
        {"stateChunk", p.getSnapshotBank().hasStateChunk(slot)}
    });
}

juce::String MCPToolHandler::recallSnapshot(const juce::var& params, MorePhiProcessor& p)
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

    auto mode = MorePhiProcessor::SnapshotRecallMode::FastParamsOnly;
    const auto modeText = params.getProperty("mode", "").toString().trim().toLowerCase();
    const bool requestFull = params.getProperty("full",
                             params.getProperty("includeState",
                             params.getProperty("include_state", false)));
    if (requestFull || modeText == "full" || p.getRecallMode() == 1)
        mode = MorePhiProcessor::SnapshotRecallMode::FullStateAndParams;

    if (!p.recallSnapshot(slot, mode))
        return toJString(json{{"success",false},{"error","queue_full"}});
    const int queued = static_cast<int>(values.size());

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(queued);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "recall_snapshot";
    optimizer.recordUsage(usage);

    return toJString(json{
        {"success", true},
        {"slot", slot},
        {"queued", queued},
        {"mode", mode == MorePhiProcessor::SnapshotRecallMode::FullStateAndParams ? "full" : "fast"}
    });
}

juce::String MCPToolHandler::setMorphPosition(const juce::var& params, MorePhiProcessor& p)
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

    const bool hasX = params.hasProperty("x");
    const bool hasY = params.hasProperty("y");
    const bool hasFader = params.hasProperty("fader");

    int source = p.getMorphSource();
    if (!sourceExplicitlySet && (hasX || hasY))
        source = 0;
    if (hasFader)
        source = 1;
    if (sourceExplicitlySet)
        source = p.getMorphSource();

    p.setMorphPositionExternal(
        static_cast<float>(params.getProperty("x", p.getMorphX())), hasX,
        static_cast<float>(params.getProperty("y", p.getMorphY())), hasY,
        static_cast<float>(params.getProperty("fader", p.getFaderPos())), hasFader,
        source);

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::getMorphState(MorePhiProcessor& p)
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

// ── Ozone mastering tools ─────────────────────────────────────────────────────

juce::String MCPToolHandler::getMasteringState(MorePhiProcessor& p)
{
    auto& ame = p.getAutoMasteringEngine();
    const bool ozoneHosted = p.getHostManager().hasPlugin() &&
        OzoneParameterMap::isOzone11(
            p.getHostManager().getPlugin()->getName());

    json result;
    result["lufs_momentary"]  = ame.getLUFSMomentary();
    result["lufs_short_term"] = ame.getLUFSShortTerm();
    result["lufs_integrated"] = ame.getLUFSIntegrated();
    result["lra"]             = ame.getLRA();
    result["true_peak_dbtp"]  = ame.getTruePeak_dBTP();
    result["ozone_hosted"]    = ozoneHosted;
    result["ozone_applicator_active"] = ame.getChainPlanner().hasOzoneApplicator();

    // Per-band dynamics gain reduction
    json grArr = json::array();
    for (int i = 0; i < 4; ++i)
        grArr.push_back(ame.getGainReductionDB(i));
    result["dynamics_gr_db"] = grArr;

    // Last mastering plan (if any)
    const MultiEffectPlan& plan = ame.getChainPlanner().getLastPlan();
    if (plan.valid)
    {
        result["last_plan"] = {
            {"compression_need",  plan.compressionNeed},
            {"use_neural_comp",   plan.useNeuralComp},
            {"target_lufs",       plan.targetLUFS},
            {"ceiling_dbtp",      plan.ceilingDBTP},
            {"exciter_enabled",   plan.exciterEnabled},
            {"width_curve",       { plan.widthCurve[0], plan.widthCurve[1],
                                    plan.widthCurve[2], plan.widthCurve[3] }},
        };
    }

    return toJString(result);
}

juce::String MCPToolHandler::applyMasteringPlan(const juce::var& params, MorePhiProcessor& p)
{
    const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
    const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
    const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
    const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

    auto& planner = p.getAutoMasteringEngine().getChainPlanner();
    const MultiEffectPlan plan = planner.executePlan(
        genreIndex, dynamicRange, spectralTilt, correlationMS);

    json result;
    result["success"]           = plan.valid;
    result["ozone_applied"]     = planner.hasOzoneApplicator();
    result["compression_need"]  = plan.compressionNeed;
    result["use_neural_comp"]   = plan.useNeuralComp;
    result["target_lufs"]       = plan.targetLUFS;
    result["ceiling_dbtp"]      = plan.ceilingDBTP;
    result["exciter_enabled"]   = plan.exciterEnabled;
    result["width_curve"]       = { plan.widthCurve[0], plan.widthCurve[1],
                                    plan.widthCurve[2], plan.widthCurve[3] };
    result["eq_prescription"]   = plan.eqPrescriptionJSON.toStdString();
    return toJString(result);
}

} // namespace more_phi
