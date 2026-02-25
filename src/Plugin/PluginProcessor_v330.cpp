/*
 * MorphSnap — PluginProcessor v3.3.0 Additions
 * Implementation of new methods for Learn Mode and Discrete Parameter Handling
 */
#include "PluginProcessor.h"

namespace morphsnap {

//=============================================================================
// New v3.3.0 Integration Methods
//=============================================================================

void MorphSnapProcessor::refreshParameterClassification()
{
    // Analyze the currently loaded plugin
    parameterClassifier_.analyzeParameters(hostManager);
    
    // Initialize discrete parameter handler with classification
    discreteHandler_.initialize(parameterClassifier_);
    
    // Update token optimizer with parameter count
    auto stats = parameterClassifier_.getStatistics();
    (void)stats; // May be used for initial budget estimation
}

void MorphSnapProcessor::recordParameterModification(int paramIndex)
{
    // Track user modification for Learn Mode
    parameterClassifier_.recordModification(paramIndex);
}

bool MorphSnapProcessor::areSnapshotsCompatible(int slotA, int slotB) const
{
    auto stateA = snapshotBank.getSlotValuesCopy(slotA);
    auto stateB = snapshotBank.getSlotValuesCopy(slotB);
    
    if (stateA.empty() || stateB.empty())
        return false;
    
    return MorphSafeAdvisor::areMorphCompatible(
        stateA, stateB, parameterClassifier_, 0.7f
    );
}

juce::String MorphSnapProcessor::getMorphCompatibilityReport(int slotA, int slotB) const
{
    auto stateA = snapshotBank.getSlotValuesCopy(slotA);
    auto stateB = snapshotBank.getSlotValuesCopy(slotB);
    
    if (stateA.empty())
        return "Snapshot " + juce::String(slotA) + " is empty";
    if (stateB.empty())
        return "Snapshot " + juce::String(slotB) + " is empty";
    
    auto comparison = MorphSafeAdvisor::compareSnapshots(
        stateA, stateB, parameterClassifier_
    );
    
    juce::String report;
    report += "Compatibility Score: " + juce::String(comparison.compatibilityScore, 2) + "/1.0\n";
    report += "Problematic Parameters: " + juce::String(comparison.problematicParamCount) + "\n";
    report += "Status: " + juce::String(comparison.compatibilityScore >= 0.7f ? "COMPATIBLE" : "CAUTION") + "\n\n";
    
    report += "Suggestions:\n";
    for (const auto& s : comparison.suggestions)
    {
        report += "- " + juce::String(s) + "\n";
    }
    
    return report;
}

//=============================================================================
// Modified processBlock Integration
//=============================================================================

/*
 * In the main processBlock, after interpolation, add:
 * 
 * // Process discrete parameters with hysteresis
 * if (!finalOutput_.empty()) {
 *     discreteHandler_.processDiscreteParameters(
 *         morphOutput, finalOutput_, morphAmount
 *     );
 *     paramBridge.applyParameterState(finalOutput_);
 * } else {
 *     paramBridge.applyParameterState(morphOutput);
 * }
 */

//=============================================================================
// Token Optimizer Integration
//=============================================================================

/*
 * Call this during initialization:
 * 
 * tokenOptimizer_.setCostModel(CostModels::Claude35Sonnet());
 * 
 * TokenBudget budget;
 * budget.maxTokensPerSession = 100000;
 * budget.maxCostPerSessionUsd = 5.0f;
 * tokenOptimizer_.setTokenBudget(budget);
 */

} // namespace morphsnap
