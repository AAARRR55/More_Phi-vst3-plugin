/*
 * More-Phi — AI/MCPToolsExtended.cpp
 * Implementation of extended MCP tools for AI integration.
 */
#include "MCPToolsExtended.h"
#include "../Plugin/PluginProcessor.h"
#include "../Core/ParameterClassifier.h"
#include "../Core/DiscreteParameterHandler.h"
#include "TokenOptimizer.h"
#include "Dataset/DatasetGeneratorV2.h"
#include "Dataset/DatasetGeneratorV3.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <future>
#include <map>

namespace more_phi {

const ExtendedToolInfo kExtendedTools[] = {
    {
        "analyze_parameters",
        "Analyze plugin parameters with AI-friendly descriptions, types, and relationships. "
        "Returns structured parameter information including categories, importance scores, and suggested use.",
        R"({
            "type": "object",
            "properties": {
                "include_descriptions": {"type": "boolean", "default": true},
                "include_hidden": {"type": "boolean", "default": false},
                "max_parameters": {"type": "integer", "default": 50}
            }
        })"
    },
    {
        "expose_parameters",
        "Control which parameters are visible to AI. Use Learn Mode to automatically "
        "expose important parameters based on user behavior.",
        R"({
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["expose", "hide", "expose_all", "hide_all", "reset_learn"]},
                "parameter_indices": {"type": "array", "items": {"type": "integer"}},
                "category": {"type": "string"}
            },
            "required": ["action"]
        })"
    },
    {
        "get_token_estimate",
        "Get token and cost estimate before making parameter changes. "
        "Helps manage API costs and stay within budget.",
        R"({
            "type": "object",
            "properties": {
                "operation": {"type": "string", "enum": ["set_parameter", "analyze", "morph", "batch_update"]},
                "parameter_count": {"type": "integer"}
            },
            "required": ["operation"]
        })"
    },
    {
        "set_parameters_optimized",
        "Set parameters with automatic token optimization. Will intelligently select "
        "which parameters to include based on importance and budget.",
        R"({
            "type": "object",
            "properties": {
                "parameters": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "index": {"type": "integer"},
                            "value": {"type": "number"}
                        },
                        "required": ["index", "value"]
                    }
                },
                "max_tokens": {"type": "integer"}
            },
            "required": ["parameters"]
        })"
    },
    {
        "get_morph_compatibility",
        "Analyze how well two snapshots will morph together. "
        "Identifies discrete parameter conflicts and suggests solutions.",
        R"({
            "type": "object",
            "properties": {
                "snapshot_a": {"type": "integer", "minimum": 0, "maximum": 11},
                "snapshot_b": {"type": "integer", "minimum": 0, "maximum": 11}
            },
            "required": ["snapshot_a", "snapshot_b"]
        })"
    },
    {
        "suggest_intermediate_snapshots",
        "Get suggestions for intermediate snapshots to create smooth morphing "
        "between snapshots with incompatible discrete parameters.",
        R"({
            "type": "object",
            "properties": {
                "from_snapshot": {"type": "integer", "minimum": 0, "maximum": 11},
                "to_snapshot": {"type": "integer", "minimum": 0, "maximum": 11},
                "steps": {"type": "integer", "default": 2, "minimum": 1, "maximum": 5}
            },
            "required": ["from_snapshot", "to_snapshot"]
        })"
    },
    {
        "get_parameter_categories",
        "Get parameters organized by category (Oscillator, Filter, Envelope, etc.). "
        "Helps AI understand plugin structure.",
        R"({
            "type": "object",
            "properties": {
                "include_empty": {"type": "boolean", "default": false}
            }
        })"
    },
    {
        "learn_from_adjustment",
        "Record that a parameter was important. Improves Learn Mode accuracy "
        "by tracking user modifications.",
        R"({
            "type": "object",
            "properties": {
                "parameter_index": {"type": "integer"},
                "importance_boost": {"type": "number", "default": 0.1, "minimum": 0, "maximum": 0.5}
            },
            "required": ["parameter_index"]
        })"
    },
    {
        "get_learn_mode_status",
        "Get current Learn Mode configuration, statistics, and exposed parameters.",
        R"({
            "type": "object",
            "properties": {}
        })"
    },
    {
        "set_learn_mode_config",
        "Configure Learn Mode behavior including exposure threshold and auto-learning.",
        R"({
            "type": "object",
            "properties": {
                "enabled": {"type": "boolean"},
                "exposure_threshold": {"type": "number", "minimum": 0, "maximum": 1},
                "auto_learn": {"type": "boolean"},
                "max_exposed_parameters": {"type": "integer", "minimum": 1, "maximum": 100}
            }
        })"
    },
    {
        "reset_learning_data",
        "Clear all learned parameter importance scores. Start fresh with Learn Mode.",
        R"({
            "type": "object",
            "properties": {}
        })"
    },
    {
        "get_discrete_parameters",
        "Get list of discrete/non-interpolatable parameters that may cause clicks during morph.",
        R"({
            "type": "object",
            "properties": {
                "include_binary": {"type": "boolean", "default": true},
                "include_enums": {"type": "boolean", "default": true}
            }
        })"
    },
    {
        "suggest_morph_settings",
        "Get recommended physics mode and settings for smooth morphing between snapshots.",
        R"({
            "type": "object",
            "properties": {
                "snapshot_a": {"type": "integer", "minimum": 0, "maximum": 11},
                "snapshot_b": {"type": "integer", "minimum": 0, "maximum": 11}
            }
        })"
    },
    {
        "get_usage_stats",
        "Get AI usage statistics including tokens used, costs, and budget remaining.",
        R"({
            "type": "object",
            "properties": {}
        })"
    },
    {
        "set_token_budget",
        "Configure token and cost budget limits for AI operations.",
        R"({
            "type": "object",
            "properties": {
                "max_tokens_per_request": {"type": "integer"},
                "max_tokens_per_session": {"type": "integer"},
                "max_cost_usd": {"type": "number", "minimum": 0},
                "enable_compression": {"type": "boolean"},
                "prioritize_important_params": {"type": "boolean"},
                "keep_last_n_parameters": {"type": "integer"}
            }
        })"
    },
    {
        "explain_parameter",
        "Get human-readable explanation of a parameter's function and effect on sound.",
        R"({
            "type": "object",
            "properties": {
                "parameter_index": {"type": "integer"},
                "detail_level": {"type": "string", "enum": ["brief", "normal", "detailed"], "default": "normal"}
            },
            "required": ["parameter_index"]
        })"
    },
    {
        "find_related_parameters",
        "Find parameters that typically work together (e.g., filter cutoff + resonance).",
        R"({
            "type": "object",
            "properties": {
                "parameter_index": {"type": "integer"},
                "max_results": {"type": "integer", "default": 5}
            },
            "required": ["parameter_index"]
        })"
    },
    {
        "generate_dataset",
        "Dataset generation compatibility entry point. Use pipeline=legacy, v2, or v3 to select the generator.",
        R"json({
            "type": "object",
            "properties": {
                "pipeline": {"type": "string", "enum": ["legacy", "v2", "v3"], "default": "v3"},
                "samples": {"type": "integer", "default": 1000},
                "output_path": {"type": "string"},
                "dataset_name": {"type": "string", "default": "morephi_dataset"},
                "source_audio_dir": {"type": "string"},
                "sample_rate": {"type": "number", "default": 48000},
                "chain_type": {"type": "string", "enum": ["eq", "dynamics", "mastering", "mixing", "creative", "custom"], "default": "mastering"},
                "output_format": {"type": "string", "enum": ["wav32", "wav24", "flac24"], "default": "wav32"},
                "duration": {"type": "number", "default": 1.0, "description": "Legacy pipeline render duration in seconds"},
                "input_audio": {"type": "string", "description": "Legacy pipeline input audio file"},
                "respect_sanity": {"type": "boolean", "default": true},
                "full_duration": {"type": "number", "default": 30.0},
                "transient_duration": {"type": "number", "default": 2.0},
                "steady_state_duration": {"type": "number", "default": 5.0},
                "use_augmentation": {"type": "boolean", "default": true},
                "enable_validation": {"type": "boolean", "default": true},
                "parallel_threads": {"type": "integer", "default": 4},
                "train_ratio": {"type": "number", "default": 0.70},
                "val_ratio": {"type": "number", "default": 0.15},
                "test_ratio": {"type": "number", "default": 0.15},
                "batch_size": {"type": "integer", "default": 2048},
                "worker_threads": {"type": "integer", "default": 0},
                "checkpoint_interval": {"type": "integer", "default": 100},
                "enable_watchdog": {"type": "boolean", "default": true},
                "memory_limit_mb": {"type": "integer", "default": 2048},
                "resume_checkpoint": {"type": "boolean", "default": false},
                "seed": {"type": "integer", "default": 42},
                "dry_run": {"type": "boolean", "default": false},
                "config_file": {"type": "string", "description": "Path to JSON config file (overrides compatible params)"}
            }
        })json"
    },
    {
        "generate_dataset_v2",
        "Full ML dataset pipeline with Latin Hypercube Sampling, multi-plugin chains, feature extraction, "
        "validation, and train/val/test splitting. Returns structured dataset with audio, metadata, and features.",
        R"json({
            "type": "object",
            "properties": {
                "samples": {"type": "integer", "default": 1000, "description": "Total samples to generate"},
                "output_path": {"type": "string", "description": "Output directory path"},
                "dataset_name": {"type": "string", "default": "morephi_dataset"},
                "source_audio_dir": {"type": "string", "description": "Directory of source audio files"},
                "sample_rate": {"type": "number", "default": 48000},
                "chain_type": {"type": "string", "enum": ["eq", "dynamics", "mastering", "mixing", "creative", "custom"], "default": "mastering"},
                "output_format": {"type": "string", "enum": ["wav32", "wav24", "flac24"], "default": "wav32"},
                "full_duration": {"type": "number", "default": 30.0},
                "transient_duration": {"type": "number", "default": 2.0},
                "steady_state_duration": {"type": "number", "default": 5.0},
                "use_augmentation": {"type": "boolean", "default": true},
                "enable_validation": {"type": "boolean", "default": true},
                "parallel_threads": {"type": "integer", "default": 4},
                "train_ratio": {"type": "number", "default": 0.70},
                "val_ratio": {"type": "number", "default": 0.15},
                "test_ratio": {"type": "number", "default": 0.15},
                "seed": {"type": "integer", "default": 42},
                "dry_run": {"type": "boolean", "default": false},
                "config_file": {"type": "string", "description": "Path to JSON config file (overrides other params)"}
            }
        })json"
    },
    {
        "generate_dataset_v3",
        "Async high-performance dataset pipeline with worker pool, adaptive throttling, crash recovery, "
        "and watchdog monitoring. Wraps V2 pipeline in parallel architecture for large-scale generation.",
        R"json({
            "type": "object",
            "properties": {
                "samples": {"type": "integer", "default": 1000},
                "output_path": {"type": "string"},
                "dataset_name": {"type": "string", "default": "morephi_dataset"},
                "source_audio_dir": {"type": "string"},
                "sample_rate": {"type": "number", "default": 48000},
                "chain_type": {"type": "string", "enum": ["eq", "dynamics", "mastering", "mixing", "creative", "custom"], "default": "mastering"},
                "output_format": {"type": "string", "enum": ["wav32", "wav24", "flac24"], "default": "wav32"},
                "batch_size": {"type": "integer", "default": 2048},
                "worker_threads": {"type": "integer", "default": 0, "description": "0 = auto-detect"},
                "checkpoint_interval": {"type": "integer", "default": 100},
                "enable_watchdog": {"type": "boolean", "default": true},
                "memory_limit_mb": {"type": "integer", "default": 2048},
                "resume_checkpoint": {"type": "boolean", "default": false, "description": "Resume from last checkpoint"},
                "seed": {"type": "integer", "default": 42},
                "dry_run": {"type": "boolean", "default": false},
                "config_file": {"type": "string", "description": "Path to JSON config file (overrides other params)"}
            }
        })json"
    }
};

const int kExtendedToolCount = sizeof(kExtendedTools) / sizeof(kExtendedTools[0]);

//=============================================================================
// Tool Implementations
//=============================================================================

juce::String MCPToolsExtended::analyzeParameters(
    const juce::var& params,
    MorePhiProcessor& processor,
    ParameterClassifier& classifier)
{
    bool includeDescriptions = params.getProperty("include_descriptions", true);
    bool includeHidden = params.getProperty("include_hidden", false);
    int maxParams = params.getProperty("max_parameters", 50);

    auto exposedIndices = classifier.getExposedParameterIndices();

    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    juce::Array<juce::var> paramArray;
    
    int count = 0;
    for (int idx : exposedIndices)
    {
        if (count >= maxParams)
            break;
        
        const auto& meta = classifier.getMetadata(idx);
        
        if (!includeHidden && !meta.isExposed)
            continue;
        
        juce::DynamicObject::Ptr paramInfo = new juce::DynamicObject();
        paramInfo->setProperty("index", idx);
        paramInfo->setProperty("name", juce::String(meta.name));
        paramInfo->setProperty("type", static_cast<int>(meta.type));
        paramInfo->setProperty("category", juce::String(meta.category));
        paramInfo->setProperty("current_value", processor.getParameterBridge().getParameterNormalized(idx));
        paramInfo->setProperty("importance_score", meta.importanceScore);
        paramInfo->setProperty("is_discrete", !meta.isInterpolatable);
        paramInfo->setProperty("is_exposed", meta.isExposed);
        
        if (includeDescriptions && meta.description[0] != '\0')
        {
            paramInfo->setProperty("description", juce::String(meta.description));
        }
        
        paramArray.add(paramInfo.get());
        count++;
    }
    
    result->setProperty("parameters", paramArray);
    result->setProperty("total_exposed", static_cast<int>(exposedIndices.size()));
    result->setProperty("returned", count);
    
    auto stats = classifier.getStatistics();
    result->setProperty("statistics", juce::var(new juce::DynamicObject()));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::exposeParameters(
    const juce::var& params,
    MorePhiProcessor& /*processor*/,
    ParameterClassifier& classifier)
{
    juce::String action = params.getProperty("action", "expose").toString();
    
    if (action == "expose_all")
    {
        classifier.exposeAll();
        return R"json({"success": true, "message": "All parameters exposed"})json";
    }
    else if (action == "hide_all")
    {
        classifier.hideAll();
        return R"json({"success": true, "message": "All parameters hidden"})json";
    }
    else if (action == "reset_learn")
    {
        classifier.clearLearningData();
        return R"json({"success": true, "message": "Learning data cleared"})json";
    }
    
    auto indices = parseParameterList(params["parameter_indices"]);
    
    if (indices.empty())
    {
        return R"json({"success": false, "error": "No parameter indices provided"})json";
    }
    
    int changed = 0;
    for (int idx : indices)
    {
        auto meta = classifier.getMetadata(idx);
        if (action == "expose")
        {
            meta.isExposed = true;
            classifier.setMetadata(idx, meta);
            changed++;
        }
        else if (action == "hide")
        {
            meta.isExposed = false;
            classifier.setMetadata(idx, meta);
            changed++;
        }
    }
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("changed_count", changed);
    result->setProperty("action", action);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getTokenEstimate(
    const juce::var& params,
    TokenOptimizer& optimizer)
{
    juce::String operation = params.getProperty("operation", "set_parameter").toString();
    int paramCount = params.getProperty("parameter_count", 10);
    
    TokenOptimizer::Estimate est;
    
    if (operation == "set_parameter")
        est = optimizer.estimateSetParameter(paramCount);
    else if (operation == "analyze")
        est = optimizer.estimateAnalyzePlugin(paramCount);
    else if (operation == "morph")
        est = optimizer.estimateMorphRequest();
    else
        est = optimizer.estimateRequest(paramCount, true, {});
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("estimated_tokens", static_cast<int>(est.totalTokens));
    result->setProperty("system_tokens", static_cast<int>(est.systemTokens));
    result->setProperty("parameter_tokens", static_cast<int>(est.parameterTokens));
    result->setProperty("estimated_cost_usd", est.estimatedCostUsd);
    result->setProperty("within_budget", est.withinBudget);
    
    juce::Array<juce::var> warnings;
    for (const auto& w : est.warnings)
    {
        warnings.add(juce::String(w));
    }
    result->setProperty("warnings", warnings);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::setParametersOptimized(
    const juce::var& params,
    MorePhiProcessor& processor,
    TokenOptimizer& optimizer)
{
    // Parse parameters array
    auto paramsArray = params["parameters"];
    if (!paramsArray.isArray())
    {
        return R"json({"success": false, "error": "parameters must be an array"})json";
    }
    
    auto& bridge = processor.getParameterBridge();
    const int maxParamCount = bridge.getParameterCount();
    const int requested = paramsArray.size();
    int rejected = 0;
    std::vector<std::pair<int, float>> updates;
    for (int i = 0; i < paramsArray.size(); ++i)
    {
        auto item = paramsArray[i];
        const int idx = item.hasProperty("index")
            ? static_cast<int>(item.getProperty("index", -1))
            : static_cast<int>(item.getProperty("id", -1));
        const auto stableId = item.getProperty("stableId",
                              item.getProperty("stable_id", "")).toString();
        const auto name = item.getProperty("name", "").toString();
        const float val = item.getProperty("value", 0.0f);
        const auto resolution = bridge.resolveParameter(stableId, idx, name);
        if (resolution.success && resolution.index >= 0 && resolution.index < maxParamCount)
        {
            // MCP-PARAMS-01: reject non-finite values (juce::jlimit is a no-op for NaN).
            if (!std::isfinite(val)) { ++rejected; continue; }
            updates.push_back({resolution.index, juce::jlimit(0.0f, 1.0f, val)});
        }
        else
        {
            ++rejected;
        }
    }
    
    // Check budget
    if (optimizer.isBudgetExceeded())
    {
        return R"json({"success": false, "error": "token budget exceeded"})json";
    }
    
    // Apply updates (defensive bounds check before enqueue)
    int applied = 0;
    int queueFailures = 0;
    for (const auto& [idx, val] : updates)
    {
        if (idx < 0 || idx >= maxParamCount)
            continue;
        if (processor.enqueueParameterSet(idx, val,
                                          MorePhiProcessor::ParameterEditSource::MCP,
                                          true))
            applied++;
        else
            queueFailures++;
    }
    
    // Record usage estimate
    const auto estimate = optimizer.estimateSetParameter(static_cast<int>(updates.size()));
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.operation = "set_parameters_optimized";
    usage.timestamp = std::chrono::steady_clock::now();
    optimizer.recordUsage(usage);

    const auto flush = applied > 0
        ? processor.flushPendingParameterCommandsForAssistant(juce::jmax(2048, applied))
        : MorePhiProcessor::ParameterCommandFlushResult{};
    
    const bool allQueued = requested > 0
        && applied == requested
        && rejected == 0
        && queueFailures == 0;

    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", allQueued);
    result->setProperty("queued_count", applied);
    result->setProperty("applied_count", applied);
    result->setProperty("applied_now_count", flush.drained);
    result->setProperty("requested_count", requested);
    result->setProperty("rejected_count", rejected);
    result->setProperty("queue_failures", queueFailures);
    result->setProperty("pending_after", flush.pendingAfter);
    result->setProperty("flush_plugin_unavailable", flush.pluginUnavailable);
    result->setProperty("flush_exclusive_access_timed_out", flush.exclusiveAccessTimedOut);
    if (queueFailures > 0)
        result->setProperty("error", "queue_full");
    else if (requested == 0)
        result->setProperty("error", "empty_parameters");
    else if (applied == 0)
        result->setProperty("error", "no_parameters_queued");
    else if (rejected > 0)
        result->setProperty("error", "partial_rejected");
    result->setProperty("estimated_tokens", static_cast<int>(estimate.totalTokens));
    result->setProperty("estimated_cost_usd", estimate.estimatedCostUsd);
    result->setProperty("budget_remaining_usd", optimizer.getBudgetRemainingUsd());
    result->setProperty("token_budget_remaining", static_cast<int>(optimizer.getTokenBudgetRemaining()));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getMorphCompatibility(
    const juce::var& params,
    MorePhiProcessor& processor,
    ParameterClassifier& classifier)
{
    int snapA = params.getProperty("snapshot_a", 0);
    int snapB = params.getProperty("snapshot_b", 1);

    if (snapA < 0 || snapA >= SnapshotBank::NUM_SLOTS || snapB < 0 || snapB >= SnapshotBank::NUM_SLOTS)
        return R"json({"success":false,"error":"invalid slot index"})json";

    // Get snapshot data
    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(snapA, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(snapB, stateB);
    
    if (stateA.empty() || stateB.empty())
    {
        return R"json({"success": false, "error": "One or both snapshots are empty"})json";
    }

    const auto comparison = MorphSafeAdvisor::compareSnapshots(stateA, stateB, classifier);
    
    return formatCompatibilityReport(comparison);
}

juce::String MCPToolsExtended::formatCompatibilityReport(
    const MorphSafeAdvisor::SnapshotComparison& comparison)
{
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("compatibility_score", comparison.compatibilityScore);
    result->setProperty("problematic_parameter_count", comparison.problematicParamCount);
    
    juce::Array<juce::var> problematic;
    for (int idx : comparison.problematicIndices)
    {
        problematic.add(idx);
    }
    result->setProperty("problematic_indices", problematic);
    
    juce::Array<juce::var> suggestions;
    for (const auto& s : comparison.suggestions)
    {
        suggestions.add(juce::String(s));
    }
    result->setProperty("suggestions", suggestions);
    
    result->setProperty("morph_compatible", comparison.compatibilityScore >= 0.7f);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::suggestIntermediateSnapshots(
    const juce::var& params,
    MorePhiProcessor& processor,
    ParameterClassifier& classifier)
{
    int fromSnap = params.getProperty("from_snapshot", 0);
    int toSnap = params.getProperty("to_snapshot", 1);
    int steps = params.getProperty("steps", 2);
    
    if (fromSnap < 0 || fromSnap >= SnapshotBank::NUM_SLOTS ||
        toSnap < 0 || toSnap >= SnapshotBank::NUM_SLOTS)
    {
        return "{\"success\": false, \"error\": \"Snapshot index out of range (must be 0-11)\"}";
    }

    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(fromSnap, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(toSnap, stateB);

    if (stateA.empty() || stateB.empty())
    {
        return "{\"success\": false, \"error\": \"One or both snapshots are empty\"}";
    }
    
    auto intermediates = MorphSafeAdvisor::suggestIntermediateSnapshots(
        stateA, stateB, classifier, steps);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("from_snapshot", fromSnap);
    result->setProperty("to_snapshot", toSnap);
    result->setProperty("suggested_steps", steps);
    
    juce::Array<juce::var> stepsArray;
    for (int i = 0; i < intermediates.size(); ++i)
    {
        juce::DynamicObject::Ptr stepObj = new juce::DynamicObject();
        stepObj->setProperty("step_number", i + 1);
        
        // Store only changed parameters (not full state)
        juce::DynamicObject::Ptr changes = new juce::DynamicObject();
        for (int j = 0; j < intermediates[i].size() && j < stateA.size(); ++j)
        {
            if (std::abs(intermediates[i][j] - stateA[j]) > 0.01f)
            {
                changes->setProperty(juce::String(j), intermediates[i][j]);
            }
        }
        stepObj->setProperty("parameter_changes", changes.get());
        stepsArray.add(stepObj.get());
    }
    result->setProperty("intermediate_snapshots", stepsArray);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getParameterCategories(
    const juce::var& params,
    MorePhiProcessor& /*processor*/,
    ParameterClassifier& classifier)
{
    bool includeEmpty = params.getProperty("include_empty", false);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    juce::DynamicObject::Ptr categories = new juce::DynamicObject();
    
    // Group parameters by category
    std::map<juce::String, juce::Array<juce::var>> categoryMap;
    
    auto exposedIndices = classifier.getExposedParameterIndices();
    for (int idx : exposedIndices)
    {
        const auto& meta = classifier.getMetadata(idx);
        juce::String cat = meta.category;
        if (cat.isEmpty())
            cat = "Uncategorized";
        
        juce::DynamicObject::Ptr paramInfo = new juce::DynamicObject();
        paramInfo->setProperty("index", idx);
        paramInfo->setProperty("name", juce::String(meta.name));
        paramInfo->setProperty("importance", meta.importanceScore);
        
        categoryMap[cat].add(paramInfo.get());
    }
    
    for (const auto& [cat, catParams] : categoryMap)
    {
        if (!includeEmpty && catParams.size() == 0)
            continue;
        
        juce::DynamicObject::Ptr catObj = new juce::DynamicObject();
        catObj->setProperty("count", catParams.size());
        catObj->setProperty("parameters", catParams);
        categories->setProperty(cat, catObj.get());
    }
    
    result->setProperty("categories", categories.get());
    result->setProperty("total_categorized", static_cast<int>(exposedIndices.size()));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::learnFromAdjustment(
    const juce::var& params,
    ParameterClassifier& classifier)
{
    int idx = params.getProperty("parameter_index", -1);
    (void)params.getProperty("importance_boost", 0.1f);  // boost param reserved for future weighted recording
    
    if (idx < 0)
    {
        return R"json({"success": false, "error": "Invalid parameter index"})json";
    }
    
    classifier.recordModification(idx);
    
    const auto& meta = classifier.getMetadata(idx);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("parameter_index", idx);
    result->setProperty("new_importance_score", meta.importanceScore);
    result->setProperty("total_modifications", static_cast<int>(meta.modificationCount));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getLearnModeStatus(
    const juce::var& /*params*/,
    ParameterClassifier& classifier)
{
    auto config = classifier.getLearnConfiguration();
    auto stats = classifier.getStatistics();
    auto exposed = classifier.getExposedParameterIndices();
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("enabled", config.enabled);
    result->setProperty("exposure_threshold", config.exposureThreshold);
    result->setProperty("auto_learn", config.autoLearn);
    result->setProperty("max_exposed_parameters", static_cast<int>(config.maxExposedParameters));
    result->setProperty("currently_exposed", static_cast<int>(exposed.size()));
    result->setProperty("total_parameters", static_cast<int>(stats.totalParameters));
    result->setProperty("continuous_parameters", static_cast<int>(stats.continuousCount));
    result->setProperty("discrete_parameters", static_cast<int>(stats.discreteCount));
    result->setProperty("binary_parameters", static_cast<int>(stats.binaryCount));
    result->setProperty("average_importance_score", stats.averageImportance);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::setLearnModeConfig(
    const juce::var& params,
    ParameterClassifier& classifier)
{
    auto config = classifier.getLearnConfiguration();
    
    if (params.hasProperty("enabled"))
        config.enabled = params["enabled"];
    if (params.hasProperty("exposure_threshold"))
        config.exposureThreshold = juce::jlimit(0.0f, 1.0f, static_cast<float>(params["exposure_threshold"]));
    if (params.hasProperty("auto_learn"))
        config.autoLearn = params["auto_learn"];
    if (params.hasProperty("max_exposed_parameters"))
        config.maxExposedParameters = static_cast<uint32_t>(static_cast<int>(params["max_exposed_parameters"]));
    
    classifier.setLearnConfiguration(config);
    
    return R"json({"success": true, "message": "Learn Mode configuration updated"})json";
}

juce::String MCPToolsExtended::resetLearningData(
    const juce::var& /*params*/,
    ParameterClassifier& classifier)
{
    classifier.clearLearningData();
    return R"json({"success": true, "message": "Learning data cleared"})json";
}

juce::String MCPToolsExtended::getDiscreteParameters(
    const juce::var& params,
    MorePhiProcessor& /*processor*/,
    ParameterClassifier& classifier)
{
    bool includeBinary = params.getProperty("include_binary", true);
    bool includeEnums = params.getProperty("include_enums", true);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    juce::Array<juce::var> discreteParams;
    juce::Array<juce::var> binaryParams;
    juce::Array<juce::var> enumParams;
    
    auto exposed = classifier.getExposedParameterIndices();
    for (int idx : exposed)
    {
        const auto& meta = classifier.getMetadata(idx);
        
        if (meta.type == ParameterType::Binary && includeBinary)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty("index", idx);
            p->setProperty("name", juce::String(meta.name));
            binaryParams.add(p.get());
        }
        else if (meta.type == ParameterType::Enumeration && includeEnums)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty("index", idx);
            p->setProperty("name", juce::String(meta.name));
            enumParams.add(p.get());
        }
        else if (meta.type == ParameterType::Discrete)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty("index", idx);
            p->setProperty("name", juce::String(meta.name));
            discreteParams.add(p.get());
        }
    }
    
    result->setProperty("binary_parameters", binaryParams);
    result->setProperty("enumeration_parameters", enumParams);
    result->setProperty("discrete_parameters", discreteParams);
    result->setProperty("total_non_interpolatable", 
        static_cast<int>(binaryParams.size() + enumParams.size() + discreteParams.size()));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::suggestMorphSettings(
    const juce::var& params,
    MorePhiProcessor& processor,
    ParameterClassifier& classifier)
{
    int snapA = params.getProperty("snapshot_a", -1);
    int snapB = params.getProperty("snapshot_b", -1);
    
    if (snapA < 0 || snapA >= SnapshotBank::NUM_SLOTS ||
        snapB < 0 || snapB >= SnapshotBank::NUM_SLOTS)
    {
        return R"json({"success": false, "error": "Snapshot index out of range (must be 0-11)"})json";
    }
    
    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(snapA, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(snapB, stateB);
    
    auto comparison = MorphSafeAdvisor::compareSnapshots(stateA, stateB, classifier);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("compatibility_score", comparison.compatibilityScore);
    
    // Suggest physics mode based on compatibility
    juce::String physicsMode = "Direct";
    juce::String elasticPreset = "Medium";
    
    if (comparison.problematicParamCount > 5)
    {
        physicsMode = "Elastic";
        elasticPreset = "Slow";
        result->setProperty("recommendation", 
            "Many discrete parameters. Use Elastic mode with Slow preset for smoothest transitions.");
    }
    else if (comparison.problematicParamCount > 0)
    {
        physicsMode = "Elastic";
        elasticPreset = "Medium";
        result->setProperty("recommendation",
            "Some discrete parameters detected. Elastic mode recommended.");
    }
    else
    {
        result->setProperty("recommendation",
            "Snapshots are highly compatible. Any mode will work well.");
    }
    
    result->setProperty("suggested_physics_mode", physicsMode);
    result->setProperty("suggested_elastic_preset", elasticPreset);
    result->setProperty("use_crossfade_for_discrete", comparison.problematicParamCount > 0);
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getUsageStats(
    const juce::var& params,
    TokenOptimizer& optimizer)
{
    juce::ignoreUnused(params);
    auto stats = optimizer.getSessionStats();
    auto display = optimizer.getDisplayData();
    auto budget = optimizer.getTokenBudget();
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("total_requests", static_cast<int>(stats.totalRequests));
    result->setProperty("total_prompt_tokens", static_cast<int>(stats.totalPromptTokens));
    result->setProperty("total_completion_tokens", static_cast<int>(stats.totalCompletionTokens));
    result->setProperty("total_tokens", static_cast<int>(stats.totalTokens()));
    result->setProperty("total_cost_usd", stats.totalCostUsd);
    result->setProperty("budget_remaining_usd", optimizer.getBudgetRemainingUsd());
    result->setProperty("token_budget_remaining", static_cast<int>(optimizer.getTokenBudgetRemaining()));
    result->setProperty("max_cost_per_session_usd", budget.maxCostPerSessionUsd);
    result->setProperty("max_tokens_per_session", static_cast<int>(budget.maxTokensPerSession));
    result->setProperty("status", juce::String(display.status));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::setTokenBudget(
    const juce::var& params,
    TokenOptimizer& optimizer)
{
    TokenBudget budget = optimizer.getTokenBudget();
    
    if (params.hasProperty("max_tokens_per_request"))
        budget.maxTokensPerRequest = static_cast<uint32_t>(juce::jmax(1, static_cast<int>(params["max_tokens_per_request"])));
    if (params.hasProperty("max_tokens_per_session"))
        budget.maxTokensPerSession = static_cast<uint32_t>(juce::jmax(1, static_cast<int>(params["max_tokens_per_session"])));
    if (params.hasProperty("max_cost_usd"))
        budget.maxCostPerSessionUsd = juce::jmax(0.0f, static_cast<float>(params["max_cost_usd"]));
    if (params.hasProperty("enable_compression"))
        budget.enableCompression = params["enable_compression"];
    if (params.hasProperty("prioritize_important_params"))
        budget.prioritizeImportantParams = params["prioritize_important_params"];
    if (params.hasProperty("keep_last_n_parameters"))
        budget.keepLastN_Parameters = static_cast<uint32_t>(juce::jmax(1, static_cast<int>(params["keep_last_n_parameters"])));
    
    optimizer.setTokenBudget(budget);

    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("message", "Token budget updated");
    result->setProperty("max_tokens_per_request", static_cast<int>(budget.maxTokensPerRequest));
    result->setProperty("max_tokens_per_session", static_cast<int>(budget.maxTokensPerSession));
    result->setProperty("max_cost_usd", budget.maxCostPerSessionUsd);
    result->setProperty("enable_compression", budget.enableCompression);
    result->setProperty("prioritize_important_params", budget.prioritizeImportantParams);
    result->setProperty("keep_last_n_parameters", static_cast<int>(budget.keepLastN_Parameters));

    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::explainParameter(
    const juce::var& params,
    MorePhiProcessor& /*processor*/,
    ParameterClassifier& classifier)
{
    int idx = params.getProperty("parameter_index", -1);
    juce::String detail = params.getProperty("detail_level", "normal").toString();
    
    if (idx < 0)
    {
        return R"json({"success": false, "error": "Invalid parameter index"})json";
    }
    
    const auto& meta = classifier.getMetadata(idx);
    juce::String name = meta.name;
    
    // Generate explanation based on parameter name and type
    juce::String explanation;
    juce::String typicalRange;
    juce::String effect;
    
    // This would ideally use a database or AI-generated descriptions
    // For now, use heuristics based on name patterns
    if (name.containsIgnoreCase("cutoff") || name.containsIgnoreCase("freq"))
    {
        explanation = "Controls the cutoff frequency of the filter.";
        typicalRange = "20 Hz to 20 kHz";
        effect = "Higher values let more high frequencies through.";
    }
    else if (name.containsIgnoreCase("resonance") || name.containsIgnoreCase("q"))
    {
        explanation = "Controls the emphasis around the cutoff frequency.";
        typicalRange = "0 to 100%";
        effect = "Higher values create a sharper peak at cutoff.";
    }
    else if (name.containsIgnoreCase("attack"))
    {
        explanation = "Time for the envelope to reach full level after a note is played.";
        typicalRange = "0 ms to 5 seconds";
        effect = "Lower values = more immediate sound, higher = slower fade-in.";
    }
    else if (name.containsIgnoreCase("release"))
    {
        explanation = "Time for the sound to fade out after the note is released.";
        typicalRange = "0 ms to 10 seconds";
        effect = "Higher values = longer sustain after note off.";
    }
    else
    {
        explanation = "Plugin parameter affecting sound characteristics.";
        typicalRange = "0 to 100%";
        effect = "Adjust to taste.";
    }
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("index", idx);
    result->setProperty("name", name);
    result->setProperty("explanation", explanation);
    
    if (detail == "detailed")
    {
        result->setProperty("typical_range", typicalRange);
        result->setProperty("effect_description", effect);
        result->setProperty("type", static_cast<int>(meta.type));
    }
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::findRelatedParameters(
    const juce::var& params,
    MorePhiProcessor& /*processor*/,
    ParameterClassifier& classifier)
{
    int idx = params.getProperty("parameter_index", -1);
    int maxResults = params.getProperty("max_results", 5);
    
    if (idx < 0)
    {
        return R"json({"success": false, "error": "Invalid parameter index"})json";
    }
    
    const auto& sourceMeta = classifier.getMetadata(idx);
    juce::String sourceName = sourceMeta.name;
    juce::String sourceCat = sourceMeta.category;
    
    juce::Array<juce::var> related;
    
    // Find parameters in same category
    auto exposed = classifier.getExposedParameterIndices();
    for (int otherIdx : exposed)
    {
        if (otherIdx == idx)
            continue;
        
        const auto& meta = classifier.getMetadata(otherIdx);
        
        // Same category = related
        if (juce::String(meta.category) == sourceCat)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty("index", otherIdx);
            p->setProperty("name", juce::String(meta.name));
            p->setProperty("relationship", "same_category");
            related.add(p.get());
            
            if (related.size() >= maxResults)
                break;
        }
    }
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("source_parameter", idx);
    result->setProperty("source_name", sourceName);
    result->setProperty("related_parameters", related);
    result->setProperty("found_count", related.size());
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

// Helper implementations
std::vector<int> MCPToolsExtended::parseParameterList(const juce::var& params)
{
    std::vector<int> result;
    if (params.isArray())
    {
        for (int i = 0; i < params.size(); ++i)
        {
            result.push_back(static_cast<int>(params[i]));
        }
    }
    return result;
}

} // namespace more_phi

juce::String more_phi::MCPToolsExtended::generateDataset(const juce::var& params, more_phi::MorePhiProcessor& processor)
{
    const auto pipeline = params.getProperty("pipeline", "v3").toString().trim().toLowerCase();

    // ponytail: the standalone legacy DatasetGenerator was removed; "legacy" is
    // now an alias for the V2 pipeline (V3 remains the default).
    if (pipeline == "v2" || pipeline == "legacy")
        return generateDatasetV2(params, processor);

    if (pipeline == "v3")
        return generateDatasetV3(params, processor);

    nlohmann::json result;
    result["success"] = false;
    result["error"] = "Invalid pipeline. Expected one of: legacy, v2, v3.";
    return juce::String(result.dump());
}

// ── V2 Dataset Generation ─────────────────────────────────────────────────────

static more_phi::ChainType parseChainType(const juce::String& s)
{
    if (s == "eq")        return more_phi::ChainType::EQOnly;
    if (s == "dynamics")  return more_phi::ChainType::DynamicsOnly;
    if (s == "mixing")    return more_phi::ChainType::Mixing;
    if (s == "creative")  return more_phi::ChainType::Creative;
    if (s == "custom")    return more_phi::ChainType::Custom;
    return more_phi::ChainType::Mastering;
}

static more_phi::OutputFormat parseOutputFormat(const juce::String& s)
{
    if (s == "wav24")  return more_phi::OutputFormat::WAV24;
    if (s == "flac24") return more_phi::OutputFormat::FLAC24;
    return more_phi::OutputFormat::WAV32Float;
}

juce::String more_phi::MCPToolsExtended::generateDatasetV2(
    const juce::var& params, more_phi::MorePhiProcessor& processor)
{
    // If a config file is provided, load from it
    juce::String configFilePath = params.getProperty("config_file", "");
    DatasetGeneratorConfig config;

    if (configFilePath.isNotEmpty())
    {
        juce::File configFile(configFilePath);
        if (!configFile.existsAsFile())
        {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "Config file not found: " + configFilePath.toStdString();
            return juce::String(err.dump());
        }
        config = DatasetGeneratorConfig::fromFile(configFile);
    }
    else
    {
        // Parse individual parameters
        config.totalSamples = static_cast<int>(params.getProperty("samples", 1000));
        config.datasetName = params.getProperty("dataset_name", "morephi_dataset").toString();
        config.sampleRate = static_cast<double>(params.getProperty("sample_rate", 48000.0));
        config.fullDuration = static_cast<float>(params.getProperty("full_duration", 30.0));
        config.transientDuration = static_cast<float>(params.getProperty("transient_duration", 2.0));
        config.steadyStateDuration = static_cast<float>(params.getProperty("steady_state_duration", 5.0));
        config.chainType = parseChainType(params.getProperty("chain_type", "mastering").toString());
        config.outputFormat = parseOutputFormat(params.getProperty("output_format", "wav32").toString());
        config.enableValidation = static_cast<bool>(params.getProperty("enable_validation", true));
        config.numParallelThreads = static_cast<int>(params.getProperty("parallel_threads", 4));
        config.randomSeed = static_cast<unsigned int>(static_cast<int>(params.getProperty("seed", 42)));
        config.dryRun = static_cast<bool>(params.getProperty("dry_run", false));

        // Split ratios
        config.splitConfig.trainRatio = static_cast<float>(params.getProperty("train_ratio", 0.70));
        config.splitConfig.valRatio = static_cast<float>(params.getProperty("val_ratio", 0.15));
        config.splitConfig.testRatio = static_cast<float>(params.getProperty("test_ratio", 0.15));

        // Source audio
        juce::String sourceDir = params.getProperty("source_audio_dir", "");
        if (sourceDir.isNotEmpty())
            config.sourceAudioDirectory = juce::File(sourceDir);
    }

    // Output path
    juce::String outputPath = params.getProperty("output_path", "");
    if (outputPath.isEmpty())
        config.outputDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("MorePhi_Datasets")
            .getChildFile(config.datasetName + "_v2_" + juce::Time::getCurrentTime().toString(true, true));
    else
        config.outputDirectory = juce::File(outputPath);

    // Validate config
    juce::String validationError;
    if (!DatasetGeneratorV2::validateConfig(config, validationError))
    {
        nlohmann::json err;
        err["success"] = false;
        err["error"] = "Invalid config: " + validationError.toStdString();
        return juce::String(err.dump());
    }

    // Run generation synchronously (MCP call blocks until complete)
    DatasetGeneratorV2 generator;
    generator.setConfig(config);
    generator.setHostManager(&processor.getHostManager());

    // Use a promise/future to capture the async result
    std::promise<GenerationResult> promise;
    auto future = promise.get_future();

    generator.onComplete = [&promise](const GenerationResult& r) {
        promise.set_value(r);
    };

    if (!generator.startGeneration())
    {
        nlohmann::json err;
        err["success"] = false;
        err["error"] = "Failed to start V2 generation. Check config and plugin state.";
        return juce::String(err.dump());
    }

    // Wait for completion (10-second timeout to avoid blocking the MCP thread indefinitely)
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        nlohmann::json timeout;
        timeout["success"] = false;
        timeout["error"] = "generation_timeout";
        timeout["message"] = "Dataset generation is still running. Check logs for progress.";
        return juce::String(timeout.dump());
    }
    auto genResult = future.get();

    // Build response
    nlohmann::json result;
    result["success"] = genResult.success;
    result["version"] = "v2";
    result["samples_generated"] = genResult.samplesGenerated;
    result["train_samples"] = genResult.trainSamples;
    result["val_samples"] = genResult.valSamples;
    result["test_samples"] = genResult.testSamples;
    result["total_time_ms"] = genResult.totalTimeMs;
    result["samples_per_hour"] = genResult.samplesPerHour;
    result["output_directory"] = genResult.datasetDirectory.getFullPathName().toStdString();

    if (genResult.manifestFile.existsAsFile())
        result["manifest_file"] = genResult.manifestFile.getFullPathName().toStdString();

    if (!genResult.errors.isEmpty())
    {
        nlohmann::json errArr = nlohmann::json::array();
        for (const auto& e : genResult.errors)
            errArr.push_back(e.toStdString());
        result["errors"] = errArr;
    }

    if (!genResult.warnings.isEmpty())
    {
        nlohmann::json warnArr = nlohmann::json::array();
        for (const auto& w : genResult.warnings)
            warnArr.push_back(w.toStdString());
        result["warnings"] = warnArr;
    }

    return juce::String(result.dump());
}

// ── V3 Dataset Generation ─────────────────────────────────────────────────────

juce::String more_phi::MCPToolsExtended::generateDatasetV3(
    const juce::var& params, more_phi::MorePhiProcessor& processor)
{
    V3GenerationConfig v3Config;

    // If a config file is provided, load base config from it
    juce::String configFilePath = params.getProperty("config_file", "");
    if (configFilePath.isNotEmpty())
    {
        juce::File configFile(configFilePath);
        if (!configFile.existsAsFile())
        {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "Config file not found: " + configFilePath.toStdString();
            return juce::String(err.dump());
        }
        v3Config.baseConfig = DatasetGeneratorConfig::fromFile(configFile);
    }
    else
    {
        // Parse V2 base config fields
        v3Config.baseConfig.totalSamples = static_cast<int>(params.getProperty("samples", 1000));
        v3Config.baseConfig.datasetName = params.getProperty("dataset_name", "morephi_dataset").toString();
        v3Config.baseConfig.sampleRate = static_cast<double>(params.getProperty("sample_rate", 48000.0));
        v3Config.baseConfig.chainType = parseChainType(params.getProperty("chain_type", "mastering").toString());
        v3Config.baseConfig.outputFormat = parseOutputFormat(params.getProperty("output_format", "wav32").toString());
        v3Config.baseConfig.randomSeed = static_cast<unsigned int>(static_cast<int>(params.getProperty("seed", 42)));
        v3Config.baseConfig.dryRun = static_cast<bool>(params.getProperty("dry_run", false));

        juce::String sourceDir = params.getProperty("source_audio_dir", "");
        if (sourceDir.isNotEmpty())
            v3Config.baseConfig.sourceAudioDirectory = juce::File(sourceDir);
    }

    // V3-specific params
    v3Config.batchSize = static_cast<int>(params.getProperty("batch_size", 2048));
    v3Config.workerThreads = static_cast<int>(params.getProperty("worker_threads", 0));
    v3Config.checkpointInterval = static_cast<int>(params.getProperty("checkpoint_interval", 100));
    v3Config.enableWatchdog = static_cast<bool>(params.getProperty("enable_watchdog", true));
    v3Config.memoryLimitBytes = static_cast<uint64_t>(static_cast<int>(params.getProperty("memory_limit_mb", 2048))) * 1024ULL * 1024ULL;

    // Output path
    juce::String outputPath = params.getProperty("output_path", "");
    if (outputPath.isEmpty())
    {
        auto baseDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("MorePhi_Datasets")
            .getChildFile(v3Config.baseConfig.datasetName + "_v3_" + juce::Time::getCurrentTime().toString(true, true));
        v3Config.outputDirectory = baseDir;
        v3Config.baseConfig.outputDirectory = baseDir;
        v3Config.logDirectory = baseDir.getChildFile("logs");
        v3Config.checkpointDirectory = baseDir.getChildFile("checkpoints");
    }
    else
    {
        juce::File outDir(outputPath);
        v3Config.outputDirectory = outDir;
        v3Config.baseConfig.outputDirectory = outDir;
        v3Config.logDirectory = outDir.getChildFile("logs");
        v3Config.checkpointDirectory = outDir.getChildFile("checkpoints");
    }

    bool resumeFromCheckpoint = static_cast<bool>(params.getProperty("resume_checkpoint", false));

    // Run generation
    DatasetGeneratorV3 generator;
    generator.setConfig(v3Config);
    generator.getV2Module().setHostManager(&processor.getHostManager());

    std::promise<std::pair<bool, std::string>> promise;
    auto future = promise.get_future();

    generator.onComplete = [&promise](bool success, const std::string& message) {
        promise.set_value({success, message});
    };

    bool started = false;
    if (resumeFromCheckpoint)
        started = generator.resumeFromCheckpoint();
    else
        started = generator.startGeneration();

    if (!started)
    {
        nlohmann::json err;
        err["success"] = false;
        err["error"] = "Failed to start V3 generation pipeline.";
        return juce::String(err.dump());
    }

    // Wait for completion (10-second timeout to avoid blocking the MCP thread indefinitely)
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        nlohmann::json timeout;
        timeout["success"] = false;
        timeout["error"] = "generation_timeout";
        timeout["message"] = "Dataset generation is still running. Check logs for progress.";
        return juce::String(timeout.dump());
    }
    auto [success, message] = future.get();

    // Get final progress snapshot
    auto progress = generator.getProgress();

    nlohmann::json result;
    result["success"] = success;
    result["version"] = "v3";
    result["message"] = message;
    result["batches_completed"] = progress.batchesCompleted;
    result["batches_total"] = progress.batchesTotal;
    result["frames_completed"] = progress.framesCompleted;
    result["batches_per_second"] = progress.batchesPerSecond;
    result["elapsed_seconds"] = progress.elapsedSeconds;
    result["percent_complete"] = progress.percentComplete;
    result["output_directory"] = v3Config.outputDirectory.getFullPathName().toStdString();

    return juce::String(result.dump());
}
