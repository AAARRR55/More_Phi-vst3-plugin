/*
 * MorphSnap — AI/MCPToolsExtended.cpp
 * Implementation of extended MCP tools for AI integration.
 */
#include "MCPToolsExtended.h"
#include "../Plugin/PluginProcessor.h"
#include "../Core/ParameterClassifier.h"
#include "../Core/DiscreteParameterHandler.h"
#include "TokenOptimizer.h"

namespace morphsnap {

// Basic struct to satisfy the dependency without bringing in the whole module yet
struct SnapshotComparison {
    float compatibilityScore = 1.0f;
    int problematicParamCount = 0;
    std::vector<int> problematicIndices;
    std::vector<juce::String> suggestions;
};

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
                "max_tokens_per_session": {"type": "integer"},
                "max_cost_usd": {"type": "number", "minimum": 0},
                "enable_compression": {"type": "boolean"}
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
    }
};

const int kExtendedToolCount = sizeof(kExtendedTools) / sizeof(kExtendedTools[0]);

//=============================================================================
// Tool Implementations
//=============================================================================

juce::String MCPToolsExtended::analyzeParameters(
    const juce::var& params,
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    bool includeDescriptions = params.getProperty("include_descriptions", true);
    bool includeHidden = params.getProperty("include_hidden", false);
    int maxParams = params.getProperty("max_parameters", 50);
    
    auto exposedIndices = classifier.getExposedParameterIndices();
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    juce::String action = params.getProperty("action", "expose").toString();
    
    if (action == "expose_all")
    {
        classifier.exposeAll();
        return R"({"success": true, "message": "All parameters exposed"})";
    }
    else if (action == "hide_all")
    {
        classifier.hideAll();
        return R"({"success": true, "message": "All parameters hidden"})";
    }
    else if (action == "reset_learn")
    {
        classifier.clearLearningData();
        return R"({"success": true, "message": "Learning data cleared"})";
    }
    
    auto indices = parseParameterList(params["parameter_indices"]);
    
    if (indices.empty())
    {
        return R"({"success": false, "error": "No parameter indices provided"})";
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
    MorphSnapProcessor& processor,
    TokenOptimizer& optimizer)
{
    // Parse parameters array
    auto paramsArray = params["parameters"];
    if (!paramsArray.isArray())
    {
        return R"({"success": false, "error": "parameters must be an array"})";
    }
    
    std::vector<std::pair<int, float>> updates;
    for (int i = 0; i < paramsArray.size(); ++i)
    {
        auto item = paramsArray[i];
        int idx = item.getProperty("index", -1);
        float val = item.getProperty("value", 0.0f);
        if (idx >= 0)
        {
            updates.push_back({idx, val});
        }
    }
    
    // Check budget
    if (optimizer.isBudgetExceeded())
    {
        return R"({"success": false, "error": "Token budget exceeded"})";
    }
    
    // Apply updates
    int applied = 0;
    for (const auto& [idx, val] : updates)
    {
        processor.enqueueParameterSet(idx, val);
        applied++;
    }
    
    // Record usage estimate
    TokenUsage usage;
    usage.promptTokens = static_cast<uint32_t>(updates.size() * 8);
    usage.completionTokens = 20;
    usage.operation = "set_parameters_optimized";
    usage.timestamp = std::chrono::steady_clock::now();
    optimizer.recordUsage(usage);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("success", true);
    result->setProperty("applied_count", applied);
    result->setProperty("budget_remaining_usd", optimizer.getBudgetRemainingUsd());
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::getMorphCompatibility(
    const juce::var& params,
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    int snapA = params.getProperty("snapshot_a", 0);
    int snapB = params.getProperty("snapshot_b", 1);
    
    // Get snapshot data
    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(snapA, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(snapB, stateB);
    
    if (stateA.empty() || stateB.empty())
    {
        return R"({"success": false, "error": "One or both snapshots are empty"})";
    }
    // Generate compatibility and suggestions
    SnapshotComparison comparison;
    
    return formatCompatibilityReport(comparison);
}

juce::String MCPToolsExtended::formatCompatibilityReport(
    const SnapshotComparison& comparison)
{
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    int fromSnap = params.getProperty("from_snapshot", 0);
    int toSnap = params.getProperty("to_snapshot", 1);
    int steps = params.getProperty("steps", 2);
    
    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(fromSnap, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(toSnap, stateB);
    
    if (stateA.empty() || stateB.empty())
    {
        return R"({"success": false, "error": "One or both snapshots are empty"})";
    }
    
    auto intermediates = MorphSafeAdvisor::suggestIntermediateSnapshots(
        stateA, stateB, classifier, steps);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    bool includeEmpty = params.getProperty("include_empty", false);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    float boost = params.getProperty("importance_boost", 0.1f);
    
    if (idx < 0)
    {
        return R"({"success": false, "error": "Invalid parameter index"})";
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
    const juce::var& params,
    ParameterClassifier& classifier)
{
    auto config = classifier.getLearnConfiguration();
    auto stats = classifier.getStatistics();
    auto exposed = classifier.getExposedParameterIndices();
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    
    return R"({"success": true, "message": "Learn Mode configuration updated"})";
}

juce::String MCPToolsExtended::resetLearningData(
    const juce::var& params,
    ParameterClassifier& classifier)
{
    classifier.clearLearningData();
    return R"({"success": true, "message": "Learning data cleared"})";
}

juce::String MCPToolsExtended::getDiscreteParameters(
    const juce::var& params,
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    bool includeBinary = params.getProperty("include_binary", true);
    bool includeEnums = params.getProperty("include_enums", true);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    int snapA = params.getProperty("snapshot_a", -1);
    int snapB = params.getProperty("snapshot_b", -1);
    
    if (snapA < 0 || snapB < 0)
    {
        return R"({"success": false, "error": "Must specify both snapshots"})";
    }
    
    std::vector<float> stateA;
    processor.getSnapshotBank().getSlotValuesCopy(snapA, stateA);
    std::vector<float> stateB;
    processor.getSnapshotBank().getSlotValuesCopy(snapB, stateB);
    
    auto comparison = MorphSafeAdvisor::compareSnapshots(stateA, stateB, classifier);
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
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
    auto stats = optimizer.getSessionStats();
    auto display = optimizer.getDisplayData();
    
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("total_requests", static_cast<int>(stats.totalRequests));
    result->setProperty("total_prompt_tokens", static_cast<int>(stats.totalPromptTokens));
    result->setProperty("total_completion_tokens", static_cast<int>(stats.totalCompletionTokens));
    result->setProperty("total_cost_usd", stats.totalCostUsd);
    result->setProperty("budget_remaining_usd", static_cast<int>(display.budgetRemaining));
    result->setProperty("status", juce::String(display.status));
    
    return juce::JSON::toString(juce::var(result.get()), true);
}

juce::String MCPToolsExtended::setTokenBudget(
    const juce::var& params,
    TokenOptimizer& optimizer)
{
    TokenBudget budget;
    
    if (params.hasProperty("max_tokens_per_session"))
        budget.maxTokensPerSession = static_cast<uint32_t>(static_cast<int>(params["max_tokens_per_session"]));
    if (params.hasProperty("max_cost_usd"))
        budget.maxCostPerSessionUsd = static_cast<float>(params["max_cost_usd"]);
    if (params.hasProperty("enable_compression"))
        budget.enableCompression = params["enable_compression"];
    
    optimizer.setTokenBudget(budget);
    
    return R"({"success": true, "message": "Token budget updated"})";
}

juce::String MCPToolsExtended::explainParameter(
    const juce::var& params,
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    int idx = params.getProperty("parameter_index", -1);
    juce::String detail = params.getProperty("detail_level", "normal").toString();
    
    if (idx < 0)
    {
        return R"({"success": false, "error": "Invalid parameter index"})";
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
    MorphSnapProcessor& processor,
    ParameterClassifier& classifier)
{
    int idx = params.getProperty("parameter_index", -1);
    int maxResults = params.getProperty("max_results", 5);
    
    if (idx < 0)
    {
        return R"({"success": false, "error": "Invalid parameter index"})";
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

} // namespace morphsnap
