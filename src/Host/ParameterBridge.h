/*
 * MorphSnap — Host/ParameterBridge.h
 * Read/write hosted plugin parameters by normalized index.
 * Implements IParameterBridge for testability.
 */
#pragma once

#include "IPluginHostManager.h"
#include <vector>

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

private:
    IPluginHostManager& host_;
};

} // namespace morphsnap
