/*
 * MorphSnap — Core/DiscreteParameterHandler.h
 * Handles discrete parameters during morphing to prevent clicks/pops.
 * Implements threshold-based switching and hysteresis to avoid rapid toggling.
 */
#pragma once

#include "ParameterClassifier.h"
#include <vector>
#include <atomic>

namespace morphsnap {

// State tracking for discrete parameters during morph
struct DiscreteParamState
{
    float currentValue = 0.0f;
    float targetValue = 0.0f;
    bool isSwitching = false;
    uint32_t switchCooldown = 0;  // Frames until next switch allowed
    bool lastMorphedValue = false;
    
    // For step-based parameters
    int currentStep = 0;
    int targetStep = 0;
};

class DiscreteParameterHandler
{
public:
    DiscreteParameterHandler();
    
    // Initialize with parameter classification
    void initialize(const ParameterClassifier& classifier);
    
    // Process discrete parameters during interpolation
    // Call this AFTER interpolating continuous parameters
    void processDiscreteParameters(const std::vector<float>& interpolatedValues,
                                   std::vector<float>& outputValues,
                                   float morphAmount);  // 0.0 = source, 1.0 = target
    
    // Set configuration
    void setSwitchThreshold(float threshold);  // When to switch (default 0.5)
    void setHysteresis(float hysteresis);      // Prevent oscillation (default 0.1)
    void setCooldownFrames(uint32_t frames);   // Minimum frames between switches
    
    // Get the discrete map for current interpolation
    std::vector<bool> getDiscreteMask() const { return discreteMask_; }
    
    // Check if a parameter should use discrete handling
    bool isDiscrete(int index) const;
    
    // Force immediate switch (for snapshot recall)
    void forceDiscreteValue(int index, float value);
    
    // Blending strategies
    enum class BlendStrategy
    {
        HardSwitch,     // Instant switch at threshold
        Crossfade,      // Crossfade between values (for some discrete params)
        Stepwise,       // Step through intermediate values
        HoldSource,     // Hold source until near target
        HoldTarget      // Jump to target immediately
    };
    
    void setBlendStrategy(int index, BlendStrategy strategy);
    void setDefaultStrategy(BlendStrategy strategy);
    
    // Per-parameter strategy override
    struct StrategyOverride
    {
        int parameterIndex;
        BlendStrategy strategy;
        float customThreshold = 0.5f;
    };
    void addStrategyOverride(const StrategyOverride& override);
    void clearStrategyOverrides();
    
    // Analysis: detect problematic parameters for morphing
    struct MorphProblem
    {
        int parameterIndex;
        float severity;      // 0-1, how much clicking expected
        std::string reason;  // "binary", "enumeration", "rapid_change"
    };
    std::vector<MorphProblem> analyzeMorphCompatibility(
        const std::vector<float>& snapshotA,
        const std::vector<float>& snapshotB) const;
    
    // Get recommendations for smooth morphing
    std::vector<std::string> getMorphRecommendations(
        const std::vector<float>& snapshotA,
        const std::vector<float>& snapshotB) const;

private:
    std::vector<DiscreteParamState> paramStates_;
    std::vector<bool> discreteMask_;
    std::vector<BlendStrategy> paramStrategies_;
    std::vector<StrategyOverride> strategyOverrides_;
    
    BlendStrategy defaultStrategy_ = BlendStrategy::HardSwitch;
    float switchThreshold_ = 0.5f;
    float hysteresis_ = 0.1f;
    uint32_t cooldownFrames_ = 100;  // ~2ms at 48kHz/512 buffer
    
    std::atomic<uint32_t> parameterCount_{0};
    
    // Internal processing
    float processDiscreteParameter(int index, float interpolatedValue, float morphAmount);
    BlendStrategy getStrategyForParameter(int index) const;
    bool shouldSwitch(int index, float interpolatedValue) const;
    int valueToStep(int index, float value) const;
    float stepToValue(int index, int step) const;
};

// Helper for "morph-safe" snapshot suggestions
class MorphSafeAdvisor
{
public:
    struct SnapshotComparison
    {
        float compatibilityScore;     // 0-1, how well snapshots morph together
        int problematicParamCount;
        std::vector<int> problematicIndices;
        std::vector<std::string> suggestions;
    };
    
    static SnapshotComparison compareSnapshots(
        const std::vector<float>& snapshotA,
        const std::vector<float>& snapshotB,
        const ParameterClassifier& classifier);
    
    // Suggest intermediate snapshots for smoother morph
    static std::vector<std::vector<float>> suggestIntermediateSnapshots(
        const std::vector<float>& snapshotA,
        const std::vector<float>& snapshotB,
        const ParameterClassifier& classifier,
        int steps = 3);
    
    // Check if two snapshots are "morph compatible"
    static bool areMorphCompatible(
        const std::vector<float>& snapshotA,
        const std::vector<float>& snapshotB,
        const ParameterClassifier& classifier,
        float threshold = 0.7f);
};

} // namespace morphsnap
