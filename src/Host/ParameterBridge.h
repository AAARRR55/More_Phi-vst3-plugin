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
#include <atomic>
#include <limits>
#include <vector>
#include <utility>
#include <cstdint>
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

    // B4 FIX: number of times a hosted-plugin setValue() threw during apply.
    // Apply is on the audio thread (silent catch), so without this counter a
    // hosted plugin that throws on every write would no-op forever with no
    // diagnostic. Saturating to avoid wraparound. Safe to read from any thread.
    uint64_t getApplyExceptionCount() const noexcept
    {
        return applyExceptionCount_.load(std::memory_order_relaxed);
    }
    void resetApplyExceptionCount() noexcept
    {
        applyExceptionCount_.store(0, std::memory_order_relaxed);
    }

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

    // B4 FIX: saturating counter for hosted-plugin setValue() exceptions on the
    // apply path (audio thread can't log). Stops at uint64 max to avoid wrap.
    mutable std::atomic<uint64_t> applyExceptionCount_{0};

    bool shouldThrottle(int index, float newValue, juce::uint32 now) const;
    void updateThrottleState(int index, float value, juce::uint32 now);

    // B4 FIX: saturating increment of applyExceptionCount_ (audio-safe).
    void bumpApplyException() const noexcept
    {
        uint64_t cur = applyExceptionCount_.load(std::memory_order_relaxed);
        if (cur != std::numeric_limits<uint64_t>::max())
            applyExceptionCount_.fetch_add(1, std::memory_order_relaxed);
    }

    template<typename Ret, typename Fn>
    static Ret withPlugin(IPluginHostManager& host, PluginHostManager* cachedHost,
                          const char* context, Ret defaultValue, Fn&& fn);
};

} // namespace more_phi
