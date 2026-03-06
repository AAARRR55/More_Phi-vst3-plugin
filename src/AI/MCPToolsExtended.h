/*
 * MorphSnap — AI/MCPToolsExtended.h
 * Extended MCP tools for Learn Mode, AI Teacher, Token Management, and
 * advanced features demonstrated in SnappySnap videos.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <memory>
#include "Core/DiscreteParameterHandler.h"

// Forward declarations
namespace morphsnap {
    class MorphSnapProcessor;
    class ParameterClassifier;
    class TokenOptimizer;
    struct InstanceIdentity;
}

namespace morphsnap {

// Extended tool handler with new AI capabilities
class MCPToolsExtended
{
public:
    // Tool: analyze_parameters
    // Provides AI-friendly analysis of plugin parameters with descriptions and relationships
    static juce::String analyzeParameters(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: expose_parameters
    // Configure which parameters are visible to AI (Learn Mode control)
    static juce::String exposeParameters(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: get_token_estimate
    // Get token/cost estimate before making changes
    static juce::String getTokenEstimate(
        const juce::var& params,
        TokenOptimizer& optimizer);
    
    // Tool: set_parameters_optimized
    // Set parameters with automatic token optimization
    static juce::String setParametersOptimized(
        const juce::var& params,
        MorphSnapProcessor& processor,
        TokenOptimizer& optimizer);
    
    // Tool: get_morph_compatibility
    // Analyze how well two snapshots will morph together
    static juce::String getMorphCompatibility(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: suggest_intermediate_snapshots
    // Get suggestions for smooth morphing between distant snapshots
    static juce::String suggestIntermediateSnapshots(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: get_parameter_categories
    // Get parameters organized by category (Oscillator, Filter, etc.)
    static juce::String getParameterCategories(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: learn_from_adjustment
    // Record that a parameter was important (Learn Mode feedback)
    static juce::String learnFromAdjustment(
        const juce::var& params,
        ParameterClassifier& classifier);
    
    // Tool: get_learn_mode_status
    // Get current Learn Mode configuration and statistics
    static juce::String getLearnModeStatus(
        const juce::var& params,
        ParameterClassifier& classifier);
    
    // Tool: set_learn_mode_config
    // Configure Learn Mode behavior
    static juce::String setLearnModeConfig(
        const juce::var& params,
        ParameterClassifier& classifier);
    
    // Tool: reset_learning_data
    // Clear learned parameter importance
    static juce::String resetLearningData(
        const juce::var& params,
        ParameterClassifier& classifier);
    
    // Tool: get_discrete_parameters
    // Get list of discrete/non-interpolatable parameters
    static juce::String getDiscreteParameters(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: suggest_morph_settings
    // Get recommended physics mode and settings for a morph
    static juce::String suggestMorphSettings(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: get_usage_stats
    // Get AI usage statistics and cost tracking
    static juce::String getUsageStats(
        const juce::var& params,
        TokenOptimizer& optimizer);
    
    // Tool: set_token_budget
    // Configure token/cost budget
    static juce::String setTokenBudget(
        const juce::var& params,
        TokenOptimizer& optimizer);
    
    // Tool: explain_parameter
    // Get human-readable explanation of a parameter's function
    static juce::String explainParameter(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);
    
    // Tool: find_related_parameters
    // Find parameters that typically work together
    static juce::String findRelatedParameters(
        const juce::var& params,
        MorphSnapProcessor& processor,
        ParameterClassifier& classifier);

    // Tool: generate_dataset
    // Renders multiple audio clips with random parameter settings to create a machine learning dataset
    static juce::String generateDataset(const juce::var& params, MorphSnapProcessor& processor);

    // Tool: generate_dataset_v2
    // Full ML pipeline: Latin Hypercube Sampling, plugin chains, feature extraction, validation, train/val/test splitting
    static juce::String generateDatasetV2(const juce::var& params, MorphSnapProcessor& processor);

    // Tool: generate_dataset_v3
    // Async pipeline with worker pool, adaptive throttling, crash recovery, and watchdog
    static juce::String generateDatasetV3(const juce::var& params, MorphSnapProcessor& processor);

private:
    // Helper: Build parameter JSON with metadata
    static juce::var buildParameterInfo(
        int index,
        const ParameterClassifier& classifier,
        MorphSnapProcessor& processor);
    
    // Helper: Parse parameter list from request
    static std::vector<int> parseParameterList(const juce::var& params);
    
    // Helper: Format compatibility report
    static juce::String formatCompatibilityReport(
        const MorphSafeAdvisor::SnapshotComparison& comparison);

};


// Tool descriptions for MCP discovery
struct ExtendedToolInfo
{
    const char* name;
    const char* description;
    const char* schema;
};

extern const ExtendedToolInfo kExtendedTools[];
extern const int kExtendedToolCount;

} // namespace morphsnap
