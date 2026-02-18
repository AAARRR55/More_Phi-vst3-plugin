/*
 * MorphSnap — Host/ParameterBridge.h
 * Read/write hosted plugin parameters by normalized index.
 */
#pragma once

#include "PluginHostManager.h"
#include <vector>

namespace morphsnap {

class ParameterBridge
{
public:
    explicit ParameterBridge(PluginHostManager& host) : host_(host) {}

    int getParameterCount() const;
    float getParameterNormalized(int index) const;
    void  setParameterNormalized(int index, float value);

    juce::String getParameterName(int index) const;

    // Apply a full state vector to the hosted plugin
    void applyParameterState(const std::vector<float>& values);

    // Read all current values into a vector
    std::vector<float> captureParameterState() const;

private:
    PluginHostManager& host_;
};

} // namespace morphsnap
