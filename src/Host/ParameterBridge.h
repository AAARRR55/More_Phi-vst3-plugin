/*
 * More-Phi — Host/ParameterBridge.h
 * Read/write hosted plugin parameters by normalized index.
 * Implements IParameterBridge for testability.
 *
 * PERF-H2: cachedConcreteHost_ eliminates per-call dynamic_cast.
 * ATS-H1: Throttle state uses tryEnter instead of blocking SpinLock.
 * PERF-M2: Single lock acquisition in batch applyParameterState.
 */
#pragma once

#include "IPluginHostManager.h"
#include <vector>
#include <utility>
#include <juce_core/juce_core.h>

namespace more_phi {

class PluginHostManager;

class ParameterBridge : public IParameterBridge
{
public:
    struct ParameterDescriptor
    {
        int index = -1;
        juce::String stableId;
        juce::String name;
        float value = 0.0f;
        juce::String displayValue;
        juce::String label;
        bool discrete = false;
        bool boolean = false;
        int numSteps = 0;
        float defaultValue = 0.0f;
    };

    struct ParameterResolution
    {
        int index = -1;
        bool success = false;
        bool ambiguous = false;
        juce::String error;
    };

    explicit ParameterBridge(IPluginHostManager& host);

    int getParameterCount() const override;
    float getParameterNormalized(int index) const override;
    void setParameterNormalized(int index, float value) override;
    juce::String getParameterName(int index) const override;

    void applyParameterState(const float* values, int count) override;
    void applyParameterState(const std::vector<float>& values) override;

    std::vector<float> captureParameterState() const override;

    bool isDiscrete(int index) const override;
    std::vector<bool> getDiscreteMap() const override;

    juce::String getParameterLabel(int index) const;
    juce::String getParameterDisplayValue(int index) const;
    float getParameterDefault(int index) const;
    juce::StringArray getParameterValueStrings(int index) const;
    juce::String getParameterStableID(int index) const;
    int getParameterNumSteps(int index) const;
    bool isBoolean(int index) const;
    ParameterDescriptor getParameterDescriptor(int index) const;
    std::vector<ParameterDescriptor> getParameterDescriptors() const;

#if MORE_PHI_TEST_MODE
    void setParameterDescriptorsForTesting(std::vector<ParameterDescriptor> descriptors)
    {
        testDescriptors_ = std::move(descriptors);
    }

    void clearParameterDescriptorsForTesting()
    {
        testDescriptors_.clear();
    }
#endif

    int findParameterIndex(const juce::String& name) const;
    ParameterResolution resolveParameter(const juce::String& stableId,
                                         int index,
                                         const juce::String& name) const;

private:
    struct ThrottleState
    {
        float lastValue = -1.0f;
        juce::uint32 lastUpdateTime = 0;
    };

    IPluginHostManager& host_;
    PluginHostManager* const cachedConcreteHost_;

    mutable juce::SpinLock throttleMutex_;
    mutable std::vector<ThrottleState> throttleStates_;
    std::vector<ParameterDescriptor> testDescriptors_;

    bool shouldThrottle(int index, float newValue, juce::uint32 now) const;
    void updateThrottleState(int index, float value, juce::uint32 now);

    template<typename Ret, typename Fn>
    static Ret withPlugin(IPluginHostManager& host, PluginHostManager* cachedHost,
                          const char* context, Ret defaultValue, Fn&& fn);
};

} // namespace more_phi
