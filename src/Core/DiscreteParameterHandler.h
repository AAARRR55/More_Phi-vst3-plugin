/*
 * More-Phi — Core/DiscreteParameterHandler.h
 * Handles discrete parameters during morphing to prevent clicks/pops.
 * Implements threshold-based switching and hysteresis to avoid rapid toggling.
 */
#pragma once

#include "ParameterClassifier.h"
#include <vector>
#include <atomic>

namespace more_phi {

// State tracking for discrete parameters during morph
struct DiscreteParamState
{
    float currentValue = 0.0f;
    float targetValue = 0.0f;
    bool isSwitching = false;
    uint32_t switchCooldown = 0;  // Frames until next switch allowed (legacy, HardSwitch)
    bool lastMorphedValue = false;

    // For step-based parameters
    int currentStep = 0;
    int targetStep = 0;

    // Fix 2.6: Time-based cooldown (seconds remaining) so Stepwise traversal
    // time is independent of host block size / sample rate. The legacy
    // switchCooldown (block count) is still used by HardSwitch for back-compat.
    float cooldownSeconds = 0.0f;
};

class DiscreteParameterHandler
{
public:
    DiscreteParameterHandler();
    
    // Initialize with parameter classification
    void initialize(const ParameterClassifier& classifier);
    
    // Process discrete parameters during interpolation
    // Call this AFTER interpolating continuous parameters.
    //
    // dt is the current block duration in seconds (blockSize / sampleRate).
    // It is used to make Stepwise strategy traversal time-independent of host
    // config (Fix 2.6). Default value preserves prior call-site behavior.
    void processDiscreteParameters(const std::vector<float>& interpolatedValues,
                                   std::vector<float>& outputValues,
                                   float morphAmount,  // 0.0 = source, 1.0 = target
                                   float dt = kRefDt);

    // Set configuration
    void setSwitchThreshold(float threshold);  // When to switch (default 0.5)
    void setHysteresis(float hysteresis);      // Prevent oscillation (default 0.1)
    void setCooldownFrames(uint32_t frames);   // Minimum frames between switches

    // Reference block config (512 smp @ 44.1 kHz). Used as the dt default so
    // legacy call sites and the derived cooldown time constant reproduce the
    // in-box feel exactly.
    static constexpr float kRefDt = 512.0f / 44100.0f;
    
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
    uint32_t cooldownFrames_ = 100;  // ~2ms at 48kHz/512 buffer (legacy, HardSwitch)
    // Fix 2.6: cooldownSeconds_ derived from cooldownFrames_ × kRefDt so that
    // the Stepwise strategy's traversal time is host-config independent.
    float cooldownSeconds_ = 100.0f * kRefDt;

    std::atomic<uint32_t> parameterCount_{0};

    // H-3 FIX: Per-parameter step count for accurate quantization.
    // Default 10 steps (backwards compatible with old hardcoded behavior).
    std::vector<int> stepCount_;

    // Pre-allocated scratch buffer for processDiscreteParameters() so that the
    // audio-path call never performs a heap allocation (resize is done once in
    // initialize()).
    std::vector<float> outputScratch_;

    // Internal processing
    float processDiscreteParameter(int index, float interpolatedValue,
                                   float morphAmount, float dt);
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

} // namespace more_phi
