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
#include "PluginProfileDB.h"
#include "Dataset/OfflineBatchRenderer.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace more_phi {

using json = nlohmann::json;

// Serialize a nlohmann::json object to juce::String
static juce::String toJString(const json& j)
{
    return juce::String(j.dump());
}

struct ToolDefinition
{
    const char* name;
    const char* description;
    const char* inputSchema;
};

struct RenderCandidateRecord
{
    int index = 0;
    bool success = false;
    juce::String id;
    juce::String outputPath;
    float peakDb = 0.0f;
    float rmsDb = -100.0f;
    bool hasSilence = false;
    bool hasClipping = false;
    double renderTimeMs = 0.0;
    juce::String errorMessage;
};

struct RenderJobRecord
{
    juce::String id;
    juce::String status = "queued";
    juce::String message;
    juce::String inputPath;
    juce::String outputDirectory;
    juce::String pluginPath;
    OfflineBatchProgress progress;
    std::vector<RenderCandidateRecord> candidates;
    bool completed = false;
    bool success = false;
    mutable std::mutex mutex;
};

static std::mutex gRenderJobsMutex;
static std::map<std::string, std::shared_ptr<RenderJobRecord>> gRenderJobs;
static std::atomic<uint64_t> gNextRenderJobId{1};

static json parseSchema(const char* schemaText)
{
    if (schemaText == nullptr || schemaText[0] == '\0')
        return json{{"type", "object"}, {"properties", json::object()}};

    try
    {
        return json::parse(schemaText);
    }
    catch (...)
    {
        return json{{"type", "object"}, {"properties", json::object()}};
    }
}

static const ToolDefinition kCoreTools[] = {
    {"get_plugin_info", "Return More-Phi and hosted plugin identity information.", R"({"type":"object","properties":{}})"},
    {"list_parameters", "List hosted plugin parameters with stable IDs and normalized values.", R"({"type":"object","properties":{}})"},
    {"get_parameter", "Read a hosted plugin parameter by stableId, index, or exact name.", R"({"type":"object","properties":{"stableId":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"}}})"},
    {"set_parameter", "Queue a hosted plugin parameter change on the audio thread.", R"({"type":"object","properties":{"stableId":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"},"value":{"type":"number"}},"required":["value"]})"},
    {"set_parameters_batch", "Queue multiple hosted plugin parameter changes on the audio thread.", R"({"type":"object","properties":{"parameters":{"type":"array","items":{"type":"object"}},"params":{"type":"array","items":{"type":"object"}}}})"},
    {"capture_snapshot", "Capture the current hosted parameter state into a More-Phi snapshot slot.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"includeState":{"type":"boolean","default":true}},"required":["slot"]})"},
    {"recall_snapshot", "Recall a More-Phi snapshot through the realtime-safe queue.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"mode":{"type":"string","enum":["fast","full"]}},"required":["slot"]})"},
    {"set_morph_position", "Set More-Phi morph pad/fader state.", R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"fader":{"type":"number"},"source":{"type":"string","enum":["xy","xypad","fader"]}}})"},
    {"get_morph_state", "Return More-Phi morph pad/fader state.", R"({"type":"object","properties":{}})"},
    {"get_mastering_state", "Return current local mastering meters and hosted Ozone status.", R"({"type":"object","properties":{}})"},
    {"apply_mastering_plan", "Generate and apply a mastering plan from compact analysis metrics.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"hosted_plugin.scan", "Inspect a VST3 plugin path and return the discoverable plugin description.", R"({"type":"object","properties":{"path":{"type":"string"},"plugin_path":{"type":"string"}}})"},
    {"hosted_plugin.load", "Load a hosted VST3 plugin from an explicit path.", R"({"type":"object","properties":{"path":{"type":"string"},"plugin_path":{"type":"string"}}})"},
    {"hosted_plugin.info", "Alias for get_plugin_info.", R"({"type":"object","properties":{}})"},
    {"hosted_plugin.parameters", "Alias for list_parameters.", R"({"type":"object","properties":{}})"},
    {"hosted_plugin.set_parameters", "Alias for set_parameters_batch.", R"({"type":"object","properties":{"parameters":{"type":"array","items":{"type":"object"}}}})"},
    {"hosted_plugin.capture_state", "Capture hosted plugin parameter state, optionally into a snapshot slot.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"include_values":{"type":"boolean","default":false},"includeState":{"type":"boolean","default":true}}})"},
    {"analysis.get_summary", "Return compact More-Phi-owned analysis and metering data.", R"({"type":"object","properties":{}})"},
    {"analysis.capture_window", "Return the latest compact analysis snapshot for a requested window length.", R"({"type":"object","properties":{"window_seconds":{"type":"number","default":3.0}}})"},
    {"analysis.compare_render", "Compare two compact analysis summaries or compare a provided summary to current meters.", R"({"type":"object","properties":{"before":{"type":"object"},"after":{"type":"object"}}})"},
    {"mastering.plan_preview", "Generate a mastering plan without applying it.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.apply_plan", "Alias for apply_mastering_plan.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.render_batch", "Create dry-run mastering candidates or start an offline file-backed hosted-plugin render job.", R"({"type":"object","properties":{"candidate_count":{"type":"integer","default":3},"dry_run":{"type":"boolean","default":true},"input_path":{"type":"string"},"output_path":{"type":"string"},"plugin_path":{"type":"string"},"allow_passthrough":{"type":"boolean","default":false},"duration_seconds":{"type":"number"},"sample_rate":{"type":"number","default":48000},"block_size":{"type":"integer","default":512},"channels":{"type":"integer","default":2},"parallel_workers":{"type":"integer","default":1},"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.render_status", "Poll an offline mastering render job started by mastering.render_batch.", R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})"},
    {"mastering.select_candidate", "Select a candidate ID returned by mastering.render_batch.", R"({"type":"object","properties":{"candidate_id":{"type":"string"}},"required":["candidate_id"]})"},
    {"plugin_profile.audit_parameters", "Audit hosted plugin parameters into a versioned profile JSON object.", R"({"type":"object","properties":{}})"},
    {"plugin_profile.get", "Load a saved profile by ID, or audit the current hosted plugin when omitted.", R"({"type":"object","properties":{"profile_id":{"type":"string"}}})"},
    {"plugin_profile.save", "Audit and save the current hosted plugin profile under the user app-data directory.", R"({"type":"object","properties":{}})"}
};

static json planToJson(const MultiEffectPlan& plan)
{
    json result{
        {"valid", plan.valid},
        {"compression_need", plan.compressionNeed},
        {"use_neural_comp", plan.useNeuralComp},
        {"target_lufs", plan.targetLUFS},
        {"ceiling_dbtp", plan.ceilingDBTP},
        {"exciter_enabled", plan.exciterEnabled},
        {"width_curve", { plan.widthCurve[0], plan.widthCurve[1], plan.widthCurve[2], plan.widthCurve[3] }},
        {"eq_prescription", plan.eqPrescriptionJSON.toStdString()}
    };
    return result;
}

static json candidateToJson(const RenderCandidateRecord& candidate)
{
    return json{
        {"id", candidate.id.toStdString()},
        {"index", candidate.index},
        {"success", candidate.success},
        {"output_path", candidate.outputPath.toStdString()},
        {"peak_db", candidate.peakDb},
        {"rms_db", candidate.rmsDb},
        {"has_silence", candidate.hasSilence},
        {"has_clipping", candidate.hasClipping},
        {"render_time_ms", candidate.renderTimeMs},
        {"error", candidate.errorMessage.toStdString()}
    };
}

static json progressToJson(const OfflineBatchProgress& progress)
{
    return json{
        {"completed", progress.completed},
        {"total", progress.total},
        {"percentage", progress.percentage},
        {"status", progress.currentStatus.toStdString()},
        {"elapsed_ms", progress.elapsedMs},
        {"estimated_remaining_ms", progress.estimatedRemainingMs},
        {"successful_renders", progress.successfulRenders},
        {"failed_renders", progress.failedRenders}
    };
}

static std::shared_ptr<RenderJobRecord> findRenderJob(const juce::String& jobId)
{
    const std::lock_guard<std::mutex> guard(gRenderJobsMutex);
    const auto it = gRenderJobs.find(jobId.toStdString());
    return it != gRenderJobs.end() ? it->second : nullptr;
}

static juce::String makeRenderCandidateId(const juce::String& jobId, int index)
{
    return jobId + ":variation_" + juce::String::formatted("%04d", index);
}

static json renderJobToJson(const RenderJobRecord& job)
{
    const std::lock_guard<std::mutex> jobGuard(job.mutex);

    json candidates = json::array();
    for (const auto& candidate : job.candidates)
        candidates.push_back(candidateToJson(candidate));

    return json{
        {"success", true},
        {"job_id", job.id.toStdString()},
        {"status", job.status.toStdString()},
        {"completed", job.completed},
        {"render_success", job.success},
        {"message", job.message.toStdString()},
        {"input_path", job.inputPath.toStdString()},
        {"output_directory", job.outputDirectory.toStdString()},
        {"plugin_path", job.pluginPath.toStdString()},
        {"progress", progressToJson(job.progress)},
        {"candidates", candidates}
    };
}

static float getNumberProperty(const juce::var& object, const char* name, float fallback)
{
    if (!object.isObject() || !object.hasProperty(name))
        return fallback;
    return static_cast<float>(object.getProperty(name, fallback));
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

juce::String MCPToolHandler::getToolList()
{
    json tools = json::array();

    auto appendTool = [&tools](const char* name, const char* description, const char* schemaText)
    {
        tools.push_back({
            {"name", name},
            {"description", description != nullptr ? description : ""},
            {"inputSchema", parseSchema(schemaText)}
        });
    };

    for (const auto& tool : kCoreTools)
        appendTool(tool.name, tool.description, tool.inputSchema);

    for (int i = 0; i < kExtendedToolCount; ++i)
        appendTool(kExtendedTools[i].name, kExtendedTools[i].description, kExtendedTools[i].schema);

    return toJString(json{{"tools", tools}});
}

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

    // MCP workflow aliases from the hosted mastering integration plan
    if (method == "hosted_plugin.scan")          return scanHostedPlugin(params, p);
    if (method == "hosted_plugin.load")          return loadHostedPlugin(params, p);
    if (method == "hosted_plugin.info")          return getPluginInfo(p);
    if (method == "hosted_plugin.parameters")    return listParameters(params, p);
    if (method == "hosted_plugin.set_parameters")return setParametersBatch(params, p);
    if (method == "hosted_plugin.capture_state") return captureHostedState(params, p);
    if (method == "analysis.get_summary")        return getAnalysisSummary(p);
    if (method == "analysis.capture_window")     return captureAnalysisWindow(params, p);
    if (method == "analysis.compare_render")     return compareAnalysis(params, p);
    if (method == "mastering.plan_preview")      return previewMasteringPlan(params, p);
    if (method == "mastering.apply_plan")        return applyMasteringPlan(params, p);
    if (method == "mastering.render_batch")      return renderMasteringBatch(params, p);
    if (method == "mastering.render_status")     return getMasteringRenderStatus(params);
    if (method == "mastering.select_candidate")  return selectMasteringCandidate(params, p);
    if (method == "plugin_profile.audit_parameters") return auditPluginProfile(p);
    if (method == "plugin_profile.get")          return getPluginProfile(params, p);
    if (method == "plugin_profile.save")         return savePluginProfile(params, p);

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

// ── Hosted mastering workflow tools ──────────────────────────────────────────

juce::String MCPToolHandler::scanHostedPlugin(const juce::var& params, MorePhiProcessor& p)
{
    const auto path = params.getProperty("path",
                      params.getProperty("plugin_path", "")).toString();
    if (path.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_plugin_path"}});

    juce::PluginDescription description;
    juce::String error;
    const juce::File pluginFile(path);
    const bool found = PluginHostManager::discoverPlugin(
        p.getHostManager().getFormatManager(), pluginFile, description, error);

    if (!found)
        return toJString(json{
            {"success", false},
            {"error", "plugin_discovery_failed"},
            {"details", error.toStdString()}
        });

    return toJString(json{
        {"success", true},
        {"name", description.name.toStdString()},
        {"manufacturer", description.manufacturerName.toStdString()},
        {"format", description.pluginFormatName.toStdString()},
        {"file_or_identifier", description.fileOrIdentifier.toStdString()},
        {"unique_id", description.uniqueId}
    });
}

juce::String MCPToolHandler::loadHostedPlugin(const juce::var& params, MorePhiProcessor& p)
{
    const auto path = params.getProperty("path",
                      params.getProperty("plugin_path", "")).toString();
    if (path.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_plugin_path"}});

    juce::PluginDescription description;
    juce::String error;
    const juce::File pluginFile(path);
    if (!PluginHostManager::discoverPlugin(p.getHostManager().getFormatManager(),
                                           pluginFile, description, error))
    {
        return toJString(json{
            {"success", false},
            {"error", "plugin_discovery_failed"},
            {"details", error.toStdString()}
        });
    }

    const bool loaded = p.getHostManager().loadPlugin(description);
    if (loaded)
    {
        p.refreshDiscreteMap();
        p.getParameterClassifier().analyzeParameters(p.getParameterBridge());
        p.getDiscreteHandler().initialize(p.getParameterClassifier());
        p.reportLatencyToHost();
    }

    return toJString(json{
        {"success", loaded},
        {"name", description.name.toStdString()},
        {"manufacturer", description.manufacturerName.toStdString()},
        {"format", description.pluginFormatName.toStdString()},
        {"error", loaded ? "" : "plugin_load_failed"}
    });
}

juce::String MCPToolHandler::captureHostedState(const juce::var& params, MorePhiProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot >= 0)
    {
        if (slot >= SnapshotBank::NUM_SLOTS)
            return toJString(json{{"success", false}, {"error", "invalid_slot"}});

        const bool includeState = params.getProperty("includeState",
                                  params.getProperty("include_state", true));
        const bool ok = p.captureSnapshotToSlot(slot, includeState);
        return toJString(json{
            {"success", ok},
            {"slot", slot},
            {"includeState", includeState},
            {"stateChunk", ok && p.getSnapshotBank().hasStateChunk(slot)}
        });
    }

    auto values = p.getParameterBridge().captureParameterState();
    json result{
        {"success", p.getHostManager().hasPlugin()},
        {"parameter_count", static_cast<int>(values.size())}
    };

    if (params.getProperty("include_values", false))
        result["values"] = values;

    if (auto* plugin = p.getHostManager().getPlugin())
        result["hosted_plugin"] = plugin->getName().toStdString();
    else
        result["error"] = "no_hosted_plugin";

    return toJString(result);
}

juce::String MCPToolHandler::getAnalysisSummary(MorePhiProcessor& p)
{
    auto& ame = p.getAutoMasteringEngine();
    json result{
        {"success", true},
        {"schema_version", 1},
        {"source", "more_phi_local_analysis"},
        {"rms", p.getRmsLevel()},
        {"queue_usage", p.getCommandQueueUsage()},
        {"lufs_momentary", ame.getLUFSMomentary()},
        {"lufs_short_term", ame.getLUFSShortTerm()},
        {"lufs_integrated", ame.getLUFSIntegrated()},
        {"lra", ame.getLRA()},
        {"true_peak_dbtp", ame.getTruePeak_dBTP()},
        {"dynamics_gr_db", json::array()}
    };

    for (int i = 0; i < 4; ++i)
        result["dynamics_gr_db"].push_back(ame.getGainReductionDB(i));

    if (auto* plugin = p.getHostManager().getPlugin())
    {
        result["hosted_plugin"] = {
            {"name", plugin->getName().toStdString()},
            {"parameter_count", static_cast<int>(plugin->getParameters().size())},
            {"latency_samples", plugin->getLatencySamples()}
        };
    }
    else
    {
        result["hosted_plugin"] = nullptr;
    }

    return toJString(result);
}

juce::String MCPToolHandler::captureAnalysisWindow(const juce::var& params, MorePhiProcessor& p)
{
    const float windowSeconds = static_cast<float>(params.getProperty("window_seconds", 3.0f));
    auto summary = json::parse(getAnalysisSummary(p).toStdString());
    summary["window_seconds"] = std::max(0.0f, windowSeconds);
    summary["mode"] = "current_meter_snapshot";
    summary["warnings"] = json::array({
        "Rolling audio-window capture is not enabled yet; returned current local meter state."
    });
    return toJString(summary);
}

juce::String MCPToolHandler::compareAnalysis(const juce::var& params, MorePhiProcessor& p)
{
    const auto before = params.getProperty("before", juce::var());
    juce::var after = params.getProperty("after", juce::var());

    if (!after.isObject())
        after = juce::JSON::parse(getAnalysisSummary(p));

    if (!before.isObject() || !after.isObject())
        return toJString(json{{"success", false}, {"error", "missing_analysis_summary"}});

    const float beforeLufs = getNumberProperty(before, "lufs_integrated", 0.0f);
    const float afterLufs = getNumberProperty(after, "lufs_integrated", 0.0f);
    const float beforePeak = getNumberProperty(before, "true_peak_dbtp", 0.0f);
    const float afterPeak = getNumberProperty(after, "true_peak_dbtp", 0.0f);
    const float beforeLra = getNumberProperty(before, "lra", 0.0f);
    const float afterLra = getNumberProperty(after, "lra", 0.0f);

    return toJString(json{
        {"success", true},
        {"delta", {
            {"lufs_integrated", afterLufs - beforeLufs},
            {"true_peak_dbtp", afterPeak - beforePeak},
            {"lra", afterLra - beforeLra}
        }},
        {"warnings", afterPeak > -0.1f ? json::array({"true_peak_close_to_0_dbtp"}) : json::array()}
    });
}

juce::String MCPToolHandler::previewMasteringPlan(const juce::var& params, MorePhiProcessor& p)
{
    const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
    const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
    const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
    const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

    const auto plan = p.getAutoMasteringEngine().getChainPlanner().previewPlan(
        genreIndex, dynamicRange, spectralTilt, correlationMS);

    json result = planToJson(plan);
    result["success"] = plan.valid;
    result["applied"] = false;
    return toJString(result);
}

juce::String MCPToolHandler::renderMasteringBatch(const juce::var& params, MorePhiProcessor& p)
{
    const int candidateCount = juce::jlimit(1, 8, static_cast<int>(params.getProperty("candidate_count", 3)));
    const bool dryRun = params.getProperty("dry_run", true);

    if (!dryRun)
    {
        return toJString(json{
            {"success", false},
            {"error", "offline_render_requires_file_job"},
            {"details", "Use dry_run=true for plan candidates; file-backed offline render job wiring is not enabled in this MCP tool yet."}
        });
    }

    const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
    const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
    const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
    const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

    json candidates = json::array();
    for (int i = 0; i < candidateCount; ++i)
    {
        const float offset = static_cast<float>(i) - static_cast<float>(candidateCount - 1) * 0.5f;
        const auto plan = p.getAutoMasteringEngine().getChainPlanner().previewPlan(
            genreIndex,
            dynamicRange + offset,
            spectralTilt + offset * 0.25f,
            std::clamp(correlationMS + offset * 0.05f, -1.0f, 1.0f));
        json candidate = planToJson(plan);
        candidate["id"] = "dry_run_" + std::to_string(i + 1);
        candidate["score"] = 1.0f / static_cast<float>(i + 1);
        candidates.push_back(candidate);
    }

    return toJString(json{
        {"success", true},
        {"mode", "dry_run_plan_candidates"},
        {"candidates", candidates}
    });
}

juce::String MCPToolHandler::selectMasteringCandidate(const juce::var& params, MorePhiProcessor& /*p*/)
{
    const auto candidateId = params.getProperty("candidate_id", "").toString();
    if (candidateId.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_candidate_id"}});

    return toJString(json{
        {"success", true},
        {"candidate_id", candidateId.toStdString()},
        {"selected", true},
        {"applied", false}
    });
}

juce::String MCPToolHandler::auditPluginProfile(MorePhiProcessor& p)
{
    return PluginProfileDB::buildAuditJson(p.getHostManager(), p.getParameterBridge());
}

juce::String MCPToolHandler::getPluginProfile(const juce::var& params, MorePhiProcessor& p)
{
    const auto profileId = params.getProperty("profile_id", "").toString();
    if (profileId.isEmpty())
        return auditPluginProfile(p);

    const auto file = PluginProfileDB::getProfileFile(profileId);
    if (!file.existsAsFile())
        return toJString(json{{"success", false}, {"error", "profile_not_found"}});

    const auto text = file.loadFileAsString();
    try
    {
        const auto parsed = json::parse(text.toStdString());
        juce::ignoreUnused(parsed);
        return text;
    }
    catch (...)
    {
        return toJString(json{{"success", false}, {"error", "profile_json_invalid"}});
    }
}

juce::String MCPToolHandler::savePluginProfile(const juce::var& /*params*/, MorePhiProcessor& p)
{
    const auto audit = PluginProfileDB::buildAuditJson(p.getHostManager(), p.getParameterBridge());
    json parsed;
    try
    {
        parsed = json::parse(audit.toStdString());
    }
    catch (...)
    {
        return toJString(json{{"success", false}, {"error", "audit_json_invalid"}});
    }

    if (!parsed.value("success", false))
        return audit;

    const auto profileId = juce::String(parsed.value("profile_id", ""));
    const auto dir = PluginProfileDB::getProfileDirectory();
    if (!dir.createDirectory())
        return toJString(json{{"success", false}, {"error", "profile_directory_create_failed"}});

    const auto file = PluginProfileDB::getProfileFile(profileId);
    if (!file.replaceWithText(audit, false, false, "\n"))
        return toJString(json{{"success", false}, {"error", "profile_write_failed"}});

    parsed["saved"] = true;
    parsed["path"] = file.getFullPathName().toStdString();
    return toJString(parsed);
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
