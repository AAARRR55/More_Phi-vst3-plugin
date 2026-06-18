/*
 * More-Phi — Core/DiscreteParameterHandler.cpp
 * Implementation of discrete parameter handling during morphing.
 */
#include "DiscreteParameterHandler.h"
#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace more_phi {

DiscreteParameterHandler::DiscreteParameterHandler()
    : switchThreshold_(0.5f)
    , hysteresis_(0.1f)
    , cooldownFrames_(100)
    , defaultStrategy_(BlendStrategy::HardSwitch)
{
}

void DiscreteParameterHandler::initialize(const ParameterClassifier& classifier)
{
    discreteMask_ = classifier.getDiscreteMap();
    parameterCount_.store(static_cast<uint32_t>(discreteMask_.size()));

    const uint32_t count = parameterCount_.load();
    paramStates_.resize(count);
    paramStrategies_.resize(count, defaultStrategy_);
    strategyOverrides_.clear();

    // H-3 FIX: Read per-parameter step counts from classifier metadata.
    // Default to 10 steps when classifier reports 0 (backwards compatible).
    stepCount_.resize(count, 10);
    for (uint32_t i = 0; i < count; ++i)
    {
        auto meta = classifier.getMetadata(static_cast<int>(i));
        if (meta.stepCount > 0)
            stepCount_[i] = static_cast<int>(meta.stepCount);
    }

    // Pre-allocate output scratch buffer to the maximum parameter count so that
    // processDiscreteParameters() never calls resize() on the audio thread.
    outputScratch_.resize(static_cast<size_t>(MAX_PARAMETERS), 0.0f);

    // Reset states
    for (auto& state : paramStates_)
    {
        state = DiscreteParamState{};
    }
}

void DiscreteParameterHandler::processDiscreteParameters(
    const std::vector<float>& interpolatedValues,
    std::vector<float>& outputValues,
    float morphAmount,
    float dt)
{
    const uint32_t count = std::min(
        static_cast<uint32_t>(interpolatedValues.size()),
        parameterCount_.load()
    );

    // Do NOT resize on the audio thread — callers must pre-size outputValues
    // to at least interpolatedValues.size() before calling this function.
    // outputScratch_ is pre-allocated in initialize() to MAX_PARAMETERS.
    if (outputValues.size() < interpolatedValues.size()) {
        jassert(false);
        outputValues.resize(interpolatedValues.size(), 0.0f); // safe fallback
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        if (discreteMask_[i])
        {
            outputValues[i] = processDiscreteParameter(
                static_cast<int>(i), interpolatedValues[i], morphAmount, dt);
        }
        else
        {
            outputValues[i] = interpolatedValues[i];
        }
    }

    // Pass through any remaining parameters beyond the discrete mask.
    // This handles the case where parameterCount_ is 0 (no discrete params)
    // or where the interpolated vector is larger than the discrete map.
    for (uint32_t i = count; i < static_cast<uint32_t>(interpolatedValues.size()); ++i)
        outputValues[i] = interpolatedValues[i];
}

float DiscreteParameterHandler::processDiscreteParameter(
    int index, float interpolatedValue, float morphAmount, float dt)
{
    if (index < 0 || index >= static_cast<int>(paramStates_.size()))
        return interpolatedValue;

    auto& state = paramStates_[index];
    BlendStrategy strategy = getStrategyForParameter(index);

    // Decrement the legacy block-count cooldown (used by HardSwitch).
    if (state.switchCooldown > 0)
    {
        state.switchCooldown--;
    }

    // Fix 2.6: Decrement the time-based cooldown (used by Stepwise) by the
    // actual block duration so traversal time is host-config independent.
    if (state.cooldownSeconds > 0.0f)
    {
        state.cooldownSeconds -= dt;
        if (state.cooldownSeconds < 0.0f) state.cooldownSeconds = 0.0f;
    }

    state.targetValue = interpolatedValue;

    switch (strategy)
    {
        case BlendStrategy::HardSwitch:
        {
            if (state.switchCooldown == 0 && shouldSwitch(index, interpolatedValue))
            {
                // Apply hysteresis
                float threshold = state.currentValue > 0.5f ?
                    switchThreshold_ - hysteresis_ :
                    switchThreshold_ + hysteresis_;

                bool newValue = interpolatedValue >= threshold;
                if (newValue != (state.currentValue > 0.5f))
                {
                    state.currentValue = newValue ? 1.0f : 0.0f;
                    state.switchCooldown = cooldownFrames_;
                }
            }
            break;
        }

        case BlendStrategy::Crossfade:
        {
            // Some discrete parameters can be crossfaded (e.g., oscillator mix)
            // Only do this if morphAmount is not at extremes
            if (morphAmount > 0.1f && morphAmount < 0.9f)
            {
                // Allow smooth interpolation
                state.currentValue = interpolatedValue;
            }
            else
            {
                // Snap to nearest at extremes
                state.currentValue = interpolatedValue >= 0.5f ? 1.0f : 0.0f;
            }
            break;
        }

        case BlendStrategy::Stepwise:
        {
            int newStep = valueToStep(index, interpolatedValue);
            // Fix 2.6: gate on the time-based cooldown, not the block count,
            // so a multi-step traverse takes the same wall-clock time at any
            // sample rate / block size.
            if (newStep != state.currentStep && state.cooldownSeconds <= 0.0f)
            {
                // Determine direction
                int stepDelta = newStep - state.currentStep;
                int stepSize = std::abs(stepDelta);

                if (stepSize == 1)
                {
                    // Single step - go directly
                    state.currentStep = newStep;
                    state.currentValue = stepToValue(index, newStep);
                    state.cooldownSeconds = cooldownSeconds_ * 0.5f;
                }
                else
                {
                    // Multiple steps - move one step toward target
                    state.currentStep += (stepDelta > 0) ? 1 : -1;
                    state.currentValue = stepToValue(index, state.currentStep);
                    state.cooldownSeconds = cooldownSeconds_;
                }
            }
            break;
        }

        case BlendStrategy::HoldSource:
        {
            // Keep source value until we're close to target.
            // Fix 2.6: morphAmount is now synthesized correctly for both XY-pad
            // and Fader sources by the caller (PluginProcessor), so this strategy
            // behaves consistently regardless of morph source.
            if (morphAmount < 0.9f)
            {
                // Still hold source
                // state.currentValue unchanged
            }
            else
            {
                // Snap to target
                state.currentValue = interpolatedValue;
            }
            break;
        }

        case BlendStrategy::HoldTarget:
        {
            // Jump to target immediately
            state.currentValue = interpolatedValue;
            break;
        }
    }

    return state.currentValue;
}

DiscreteParameterHandler::BlendStrategy DiscreteParameterHandler::getStrategyForParameter(int index) const
{
    // Check for override
    for (const auto& override : strategyOverrides_)
    {
        if (override.parameterIndex == index)
            return override.strategy;
    }
    
    // Return per-parameter or default
    if (index >= 0 && index < static_cast<int>(paramStrategies_.size()))
    {
        return paramStrategies_[index];
    }
    
    return defaultStrategy_;
}

bool DiscreteParameterHandler::shouldSwitch(int index, float interpolatedValue) const
{
    const auto& state = paramStates_[index];
    
    float threshold = state.currentValue > 0.5f ? 
        switchThreshold_ - hysteresis_ : 
        switchThreshold_ + hysteresis_;
    
    return interpolatedValue >= threshold;
}

int DiscreteParameterHandler::valueToStep(int index, float value) const
{
    // H-3 FIX: Use actual per-parameter step count instead of hardcoded 9.999f
    const int steps = (index >= 0 && index < static_cast<int>(stepCount_.size()))
                        ? stepCount_[index] : 10;
    const float maxStep = static_cast<float>(steps - 1);
    // Use std::round to ensure values map to the nearest step.
    // Truncating via static_cast<int> causes the highest step to be practically unreachable.
    return std::clamp(static_cast<int>(std::round(value * maxStep)), 0, steps - 1);
}

float DiscreteParameterHandler::stepToValue(int index, int step) const
{
    const int steps = (index >= 0 && index < static_cast<int>(stepCount_.size()))
                        ? stepCount_[index] : 10;
    const float maxStep = static_cast<float>(steps - 1);
    return (maxStep > 0.0f) ? static_cast<float>(step) / maxStep : 0.0f;
}

void DiscreteParameterHandler::setSwitchThreshold(float threshold)
{
    switchThreshold_ = std::clamp(threshold, 0.0f, 1.0f);
}

void DiscreteParameterHandler::setHysteresis(float hysteresis)
{
    hysteresis_ = std::clamp(hysteresis, 0.0f, 0.5f);
}

void DiscreteParameterHandler::setCooldownFrames(uint32_t frames)
{
    cooldownFrames_ = frames;
    // Fix 2.6: derive the time-based cooldown used by Stepwise so traversal
    // time is independent of host sample rate / block size. At the reference
    // block config (kRefDt) this reproduces the legacy per-block rate exactly.
    cooldownSeconds_ = static_cast<float>(frames) * kRefDt;
}

void DiscreteParameterHandler::setBlendStrategy(int index, BlendStrategy strategy)
{
    if (index >= 0 && index < static_cast<int>(paramStrategies_.size()))
    {
        paramStrategies_[index] = strategy;
    }
}

void DiscreteParameterHandler::setDefaultStrategy(BlendStrategy strategy)
{
    defaultStrategy_ = strategy;
    // Update all parameters that don't have specific overrides
    for (auto& strat : paramStrategies_)
    {
        strat = strategy;
    }
}

void DiscreteParameterHandler::addStrategyOverride(const StrategyOverride& override)
{
    // Remove existing override for this parameter
    strategyOverrides_.erase(
        std::remove_if(strategyOverrides_.begin(), strategyOverrides_.end(),
            [&override](const StrategyOverride& o) {
                return o.parameterIndex == override.parameterIndex;
            }),
        strategyOverrides_.end()
    );
    
    strategyOverrides_.push_back(override);
}

void DiscreteParameterHandler::clearStrategyOverrides()
{
    strategyOverrides_.clear();
}

bool DiscreteParameterHandler::isDiscrete(int index) const
{
    if (index < 0 || index >= static_cast<int>(discreteMask_.size()))
        return false;
    return discreteMask_[index];
}

void DiscreteParameterHandler::forceDiscreteValue(int index, float value)
{
    if (index >= 0 && index < static_cast<int>(paramStates_.size()))
    {
        paramStates_[index].currentValue = value;
        paramStates_[index].switchCooldown = 0;
    }
}

std::vector<DiscreteParameterHandler::MorphProblem> 
DiscreteParameterHandler::analyzeMorphCompatibility(
    const std::vector<float>& snapshotA,
    const std::vector<float>& snapshotB) const
{
    std::vector<MorphProblem> problems;
    
    const uint32_t count = std::min(
        static_cast<uint32_t>(std::min(snapshotA.size(), snapshotB.size())),
        parameterCount_.load()
    );
    
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!discreteMask_[i])
            continue;
        
        float diff = std::abs(snapshotA[i] - snapshotB[i]);
        if (diff < 0.01f)
            continue;  // Values are close enough
        
        MorphProblem problem;
        problem.parameterIndex = static_cast<int>(i);
        problem.severity = diff;
        
        // Determine reason
        if (diff > 0.9f)
        {
            problem.reason = "binary_switch";
        }
        else
        {
            problem.reason = "discrete_change";
        }
        
        problems.push_back(problem);
    }
    
    // Sort by severity
    std::sort(problems.begin(), problems.end(),
        [](const MorphProblem& a, const MorphProblem& b) {
            return a.severity > b.severity;
        });
    
    return problems;
}

std::vector<std::string> DiscreteParameterHandler::getMorphRecommendations(
    const std::vector<float>& snapshotA,
    const std::vector<float>& snapshotB) const
{
    std::vector<std::string> recommendations;
    auto problems = analyzeMorphCompatibility(snapshotA, snapshotB);
    
    if (problems.empty())
    {
        recommendations.push_back("Snapshots should morph smoothly.");
        return recommendations;
    }
    
    int binarySwitches = 0;
    int discreteChanges = 0;
    
    for (const auto& p : problems)
    {
        if (p.reason == "binary_switch")
            binarySwitches++;
        else
            discreteChanges++;
    }
    
    if (binarySwitches > 0)
    {
        recommendations.push_back(
            "Warning: " + std::to_string(binarySwitches) + 
            " binary parameters will switch during morph. Expect clicks."
        );
        recommendations.push_back(
            "Consider enabling 'Sanity Mode' to protect volume/bypass parameters."
        );
    }
    
    if (discreteChanges > 5)
    {
        recommendations.push_back(
            "Many discrete parameters differ. Try using more similar snapshot sources."
        );
    }
    
    if (problems.size() > 10)
    {
        recommendations.push_back(
            "High parameter divergence detected. Consider using 'Breeding' instead of morphing."
        );
    }
    
    recommendations.push_back(
        "Tip: Use 'Elastic' mode with 'Slow' preset for smoother transitions on discrete parameters."
    );
    
    return recommendations;
}

//=============================================================================
// MorphSafeAdvisor Implementation
//=============================================================================

MorphSafeAdvisor::SnapshotComparison MorphSafeAdvisor::compareSnapshots(
    const std::vector<float>& snapshotA,
    const std::vector<float>& snapshotB,
    const ParameterClassifier& classifier)
{
    SnapshotComparison result{};
    
    const auto discreteMap = classifier.getDiscreteMap();
    const size_t count = std::min({
        snapshotA.size(), 
        snapshotB.size(), 
        discreteMap.size()
    });
    
    float totalDifference = 0.0f;
    int significantChanges = 0;
    
    for (size_t i = 0; i < count; ++i)
    {
        float diff = std::abs(snapshotA[i] - snapshotB[i]);
        totalDifference += diff;
        
        if (diff > 0.1f)
        {
            significantChanges++;
            
            if (discreteMap[i])
            {
                result.problematicIndices.push_back(static_cast<int>(i));
            }
        }
    }
    
    result.problematicParamCount = static_cast<int>(result.problematicIndices.size());
    
    // Calculate compatibility score
    float avgDifference = count > 0 ? totalDifference / count : 0.0f;
    float discretePenalty = result.problematicParamCount * 0.05f;
    result.compatibilityScore = std::max(0.0f, 1.0f - avgDifference - discretePenalty);
    
    // Generate suggestions
    if (result.problematicParamCount > 0)
    {
        result.suggestions.push_back(
            "Found " + std::to_string(result.problematicParamCount) + 
            " discrete parameters that will cause clicks during morph."
        );
        
        if (result.compatibilityScore < 0.5f)
        {
            result.suggestions.push_back(
                "Low compatibility score. These snapshots may not morph well together."
            );
            result.suggestions.push_back(
                "Consider creating intermediate snapshots or using Breeding mode."
            );
        }
        else if (result.problematicParamCount < 3)
        {
            result.suggestions.push_back(
                "Few problematic parameters. Morph should work with some audible switching."
            );
        }
    }
    else
    {
        result.suggestions.push_back("Snapshots are morph-compatible!");
    }
    
    return result;
}

std::vector<std::vector<float>> MorphSafeAdvisor::suggestIntermediateSnapshots(
    const std::vector<float>& snapshotA,
    const std::vector<float>& snapshotB,
    const ParameterClassifier& classifier,
    int steps)
{
    std::vector<std::vector<float>> intermediates;
    
    const auto discreteMap = classifier.getDiscreteMap();
    const size_t count = std::min({
        snapshotA.size(),
        snapshotB.size(),
        discreteMap.size()
    });
    
    for (int step = 1; step <= steps; ++step)
    {
        float t = static_cast<float>(step) / (steps + 1);
        
        std::vector<float> intermediate(count);
        
        for (size_t i = 0; i < count; ++i)
        {
            float interpolated = snapshotA[i] * (1.0f - t) + snapshotB[i] * t;
            
            if (discreteMap[i])
            {
                // For discrete params, don't interpolate - pick closest source
                intermediate[i] = (t < 0.5f) ? snapshotA[i] : snapshotB[i];
            }
            else
            {
                intermediate[i] = interpolated;
            }
        }
        
        intermediates.push_back(intermediate);
    }
    
    return intermediates;
}

bool MorphSafeAdvisor::areMorphCompatible(
    const std::vector<float>& snapshotA,
    const std::vector<float>& snapshotB,
    const ParameterClassifier& classifier,
    float threshold)
{
    auto comparison = compareSnapshots(snapshotA, snapshotB, classifier);
    return comparison.compatibilityScore >= threshold;
}

} // namespace more_phi
