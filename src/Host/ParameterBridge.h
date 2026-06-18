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

    int getParameterCount() const noexcept override;
    float getParameterNormalized(int index) const noexcept override;
    float getParameterNormalized(juce::AudioPluginInstance& plugin, int index) const noexcept;
    void setParameterNormalized(int index, float value) noexcept override;
    void setParameterNormalized(juce::AudioPluginInstance& plugin, int index, float value) noexcept;
    juce::String getParameterName(int index) const override;

    void applyParameterState(const float* values, int count) noexcept override;
    void applyParameterState(const std::vector<float>& values) noexcept override;

    std::vector<float> captureParameterState() const noexcept override;
    void captureAllNormalized(float* outValues, int count) const noexcept override;
    void captureAllNames(juce::StringArray& outNames, int count) const override;

    bool isDiscrete(int index) const noexcept override;
    std::vector<bool> getDiscreteMap() const noexcept override;

    juce::String getParameterLabel(int index) const override;
    juce::String getParameterDisplayValue(int index) const override;
    juce::String getParameterDisplayValueAtNormalized(int index, float normalizedValue) const;
    float getParameterDefault(int index) const noexcept override;
    juce::StringArray getParameterValueStrings(int index) const override;
    juce::String getParameterStableID(int index) const override;
    int getParameterNumSteps(int index) const noexcept override;
    bool isBoolean(int index) const noexcept;
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
