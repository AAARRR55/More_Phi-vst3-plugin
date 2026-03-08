/*
 * MorphSnap — Host/ParameterBridge.h
 * Read/write hosted plugin parameters by normalized index.
 * Implements IParameterBridge for testability.
 */
#pragma once

#include "IPluginHostManager.h"
#include <vector>
#include <unordered_map>
#include <mutex>

namespace morphsnap {

class ParameterBridge : public IParameterBridge
{
public:
    explicit ParameterBridge(IPluginHostManager& host) : host_(host) {}

    // IParameterBridge implementation
    int getParameterCount() const override;
    float getParameterNormalized(int index) const override;
    void setParameterNormalized(int index, float value) override;
    juce::String getParameterName(int index) const override;

    // Apply contiguous normalized values [0,1] to hosted plugin
    void applyParameterState(const float* values, int count) override;

    // Apply a full state vector to the hosted plugin
    void applyParameterState(const std::vector<float>& values) override;

    // Read all current values into a vector
    std::vector<float> captureParameterState() const override;

    // Discrete parameter classification (for Listen Mode)
    bool isDiscrete(int index) const override;
    std::vector<bool> getDiscreteMap() const override;

private:
    struct ThrottleState
    {
        float lastValue = -1.0f;
        juce::uint32 lastUpdateTime = 0;
    };

    IPluginHostManager& host_;
    
    // Throttle tracking (per-parameter index)
    mutable std::mutex throttleMutex_;
    mutable std::vector<ThrottleState> throttleStates_;
    
    // Ensures throttleStates_ is sized correctly
    void ensureThrottleSize(int count) const;
    
    // Core throttled update logic
    bool shouldThrottle(int index, float newValue, juce::uint32 now) const;
};

} // namespace morphsnap
