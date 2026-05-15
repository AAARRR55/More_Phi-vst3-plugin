/*
 * More-Phi — Host/ParameterBridge.cpp
 *
 * ATS-H1: Audio-thread throttle state writes use tryEnter — non-blocking.
 * PERF-H2: cachedConcreteHost_ eliminates per-call dynamic_cast.
 * PERF-M2: applyParameterState acquires throttle lock once per batch.
 */
#include "ParameterBridge.h"
#include "PluginHostManager.h"
#include <exception>
#include <cmath>

namespace more_phi {

namespace {

juce::String getStableParameterId(juce::AudioProcessorParameter& parameter)
{
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(&parameter))
        return hosted->getParameterID();

    return {};
}

ParameterBridge::ParameterDescriptor buildDescriptor(int index, juce::AudioProcessorParameter& parameter)
{
    const float value = parameter.getValue();
    const int steps = parameter.getNumSteps();

    ParameterBridge::ParameterDescriptor descriptor;
    descriptor.index = index;
    descriptor.stableId = getStableParameterId(parameter);
    descriptor.name = parameter.getName(128);
    descriptor.value = value;
    descriptor.displayValue = parameter.getText(value, 64);
    descriptor.label = parameter.getLabel();
    descriptor.discrete = parameter.isDiscrete() || (steps > 0 && steps <= 32);
    descriptor.boolean = parameter.isBoolean();
    descriptor.numSteps = steps;
    descriptor.defaultValue = parameter.getDefaultValue();
    return descriptor;
}

} // namespace

ParameterBridge::ParameterBridge(IPluginHostManager& host)
    : host_(host)
    , cachedConcreteHost_(dynamic_cast<PluginHostManager*>(&host))
{
    throttleStates_.resize(8192);
}

template<typename Ret, typename Fn>
Ret ParameterBridge::withPlugin(IPluginHostManager& host, PluginHostManager* cachedHost,
                                 const char* context, Ret defaultValue, Fn&& fn)
{
    juce::AudioPluginInstance* plugin = nullptr;

    if (cachedHost != nullptr)
        plugin = cachedHost->acquirePluginForUse();
    else
        plugin = host.getPlugin();

    if (!plugin)
        return defaultValue;

    struct ScopedRelease
    {
        explicit ScopedRelease(PluginHostManager* h) : host(h) {}
        ~ScopedRelease()
        {
            if (host != nullptr)
                host->releasePluginFromUse();
        }
        PluginHostManager* host;
    } release(cachedHost);

    try
    {
        return fn(*plugin);
    }
    catch (...)
    {
        // Audio-reachable bridge calls must not allocate or log while unwinding
        // a hosted plugin fault.
        juce::ignoreUnused(context);
        return defaultValue;
    }
}

int ParameterBridge::getParameterCount() const
{
    if (!testDescriptors_.empty())
        return static_cast<int>(testDescriptors_.size());

    return withPlugin(host_, cachedConcreteHost_, "getParameterCount", 0,
        [](juce::AudioPluginInstance& plugin)
    {
        return static_cast<int>(plugin.getParameters().size());
    });
}

float ParameterBridge::getParameterNormalized(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterNormalized", 0.0f,
        [index](juce::AudioPluginInstance& plugin) -> float
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return 0.0f;
        return params[index]->getValue();
    });
}

void ParameterBridge::setParameterNormalized(int index, float value)
{
    withPlugin(host_, cachedConcreteHost_, "setParameterNormalized", 0,
        [this, index, value](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return 0;

        const float clamped = juce::jlimit(0.0f, 1.0f, value);
        const auto now = juce::Time::getMillisecondCounter();

        if (shouldThrottle(index, clamped, now))
            return 0;

        try {
            params[index]->setValue(clamped);
        } catch (...) {
            // Silent — hosted plugin setValue threw, but we can't log on audio thread
        }

        updateThrottleState(index, clamped, now);
        return 0;
    });
}

juce::String ParameterBridge::getParameterName(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterName", juce::String{},
        [index](juce::AudioPluginInstance& plugin) -> juce::String
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        return params[index]->getName(128);
    });
}

void ParameterBridge::applyParameterState(const std::vector<float>& values)
{
    applyParameterState(values.data(), static_cast<int>(values.size()));
}

void ParameterBridge::applyParameterState(const float* values, int count)
{
    if (!values || count <= 0) return;

    withPlugin(host_, cachedConcreteHost_, "applyParameterState", 0,
        [this, values, count](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        const int safeCount = juce::jmin(count, (int)params.size(), (int)throttleStates_.size());
        const auto now = juce::Time::getMillisecondCounter();

        const bool hasThrottleLock = throttleMutex_.tryEnter();

        for (int i = 0; i < safeCount; ++i)
        {
            const float clamped = juce::jlimit(0.0f, 1.0f, values[i]);

            if (!shouldThrottle(i, clamped, now))
            {
                params[i]->setValue(clamped);

                if (hasThrottleLock && i < (int)throttleStates_.size())
                    throttleStates_[i] = {clamped, now};
            }
        }

        if (hasThrottleLock)
            throttleMutex_.exit();

        return 0;
    });
}

std::vector<float> ParameterBridge::captureParameterState() const
{
    return withPlugin(host_, cachedConcreteHost_, "captureParameterState", std::vector<float>{},
        [](juce::AudioPluginInstance& plugin) -> std::vector<float>
    {
        auto& params = plugin.getParameters();
        std::vector<float> state(params.size());
        for (int i = 0; i < (int)params.size(); ++i)
            state[i] = params[i]->getValue();
        return state;
    });
}

bool ParameterBridge::isDiscrete(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "isDiscrete", false,
        [index](juce::AudioPluginInstance& plugin) -> bool
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return false;
        auto* p = params[index];
        if (p->isDiscrete()) return true;
        int steps = p->getNumSteps();
        return steps > 0 && steps <= 32;
    });
}

std::vector<bool> ParameterBridge::getDiscreteMap() const
{
    return withPlugin(host_, cachedConcreteHost_, "getDiscreteMap", std::vector<bool>{},
        [](juce::AudioPluginInstance& plugin) -> std::vector<bool>
    {
        auto& params = plugin.getParameters();
        std::vector<bool> map(params.size(), false);
        for (int i = 0; i < (int)params.size(); ++i)
        {
            auto* p = params[i];
            if (p->isDiscrete()) { map[i] = true; continue; }
            int steps = p->getNumSteps();
            if (steps > 0 && steps <= 32) map[i] = true;
        }
        return map;
    });
}

bool ParameterBridge::shouldThrottle(int index, float newValue, juce::uint32 now) const
{
    if (!throttleMutex_.tryEnter())
        return false;  // Can't check — allow update (audio-safe default)

    const bool result = [this, index, newValue, now]() -> bool
    {
        if (index >= (int)throttleStates_.size() || index < 0)
            return false;

        const auto& state = throttleStates_[index];

        if (state.lastValue < 0.0f)
            return false;

        const juce::uint32 delta = now - state.lastUpdateTime;
        if (delta >= 2)
            return false;

        if (std::abs(newValue - state.lastValue) > 0.01f)
            return false;

        if (newValue == state.lastValue)
            return true;

        return true;
    }();

    throttleMutex_.exit();
    return result;
}

void ParameterBridge::updateThrottleState(int index, float value, juce::uint32 now)
{
    // Non-blocking: skip update if lock is contested
    if (throttleMutex_.tryEnter())
    {
        if (index >= 0 && index < (int)throttleStates_.size())
            throttleStates_[index] = {value, now};
        throttleMutex_.exit();
    }
}

juce::String ParameterBridge::getParameterLabel(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterLabel", juce::String{},
        [index](juce::AudioPluginInstance& plugin) -> juce::String
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        return params[index]->getLabel();
    });
}

juce::String ParameterBridge::getParameterDisplayValue(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterDisplayValue", juce::String{},
        [index](juce::AudioPluginInstance& plugin) -> juce::String
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        float value = params[index]->getValue();
        return params[index]->getText(value, 64);
    });
}

float ParameterBridge::getParameterDefault(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterDefault", -1.0f,
        [index](juce::AudioPluginInstance& plugin) -> float
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return -1.0f;
        return params[index]->getDefaultValue();
    });
}

juce::StringArray ParameterBridge::getParameterValueStrings(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterValueStrings", juce::StringArray{},
        [index](juce::AudioPluginInstance& plugin) -> juce::StringArray
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        return params[index]->getAllValueStrings();
    });
}

juce::String ParameterBridge::getParameterStableID(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterStableID", juce::String{},
        [index](juce::AudioPluginInstance& plugin) -> juce::String
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        return getStableParameterId(*params[index]);
    });
}

int ParameterBridge::getParameterNumSteps(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterNumSteps", 0,
        [index](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return 0;
        return params[index]->getNumSteps();
    });
}

bool ParameterBridge::isBoolean(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "isBoolean", false,
        [index](juce::AudioPluginInstance& plugin) -> bool
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return false;
        return params[index]->isBoolean();
    });
}

ParameterBridge::ParameterDescriptor ParameterBridge::getParameterDescriptor(int index) const
{
    return withPlugin(host_, cachedConcreteHost_, "getParameterDescriptor", ParameterDescriptor{},
        [index](juce::AudioPluginInstance& plugin) -> ParameterDescriptor
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return {};
        return buildDescriptor(index, *params[index]);
    });
}

std::vector<ParameterBridge::ParameterDescriptor> ParameterBridge::getParameterDescriptors() const
{
    if (!testDescriptors_.empty())
        return testDescriptors_;

    return withPlugin(host_, cachedConcreteHost_, "getParameterDescriptors", std::vector<ParameterDescriptor>{},
        [](juce::AudioPluginInstance& plugin) -> std::vector<ParameterDescriptor>
    {
        auto& params = plugin.getParameters();
        std::vector<ParameterDescriptor> descriptors;
        descriptors.reserve(static_cast<size_t>(params.size()));

        for (int i = 0; i < (int)params.size(); ++i)
            descriptors.push_back(buildDescriptor(i, *params[i]));

        return descriptors;
    });
}

int ParameterBridge::findParameterIndex(const juce::String& name) const
{
    if (!testDescriptors_.empty())
    {
        for (int i = 0; i < static_cast<int>(testDescriptors_.size()); ++i)
        {
            if (testDescriptors_[static_cast<size_t>(i)].name == name)
                return i;
        }

        return -1;
    }

    return withPlugin(host_, cachedConcreteHost_, "findParameterIndex", -1,
        [&name](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        for (int i = 0; i < (int)params.size(); ++i)
        {
            if (params[i]->getName(128) == name)
                return i;
        }
        return -1;
    });
}

ParameterBridge::ParameterResolution ParameterBridge::resolveParameter(
    const juce::String& stableId,
    int index,
    const juce::String& name) const
{
    auto descriptors = getParameterDescriptors();
    ParameterResolution resolution;

    if (stableId.isNotEmpty())
    {
        int matched = -1;
        int matches = 0;
        for (const auto& descriptor : descriptors)
        {
            if (descriptor.stableId == stableId)
            {
                matched = descriptor.index;
                ++matches;
            }
        }

        if (matches == 1)
            return {matched, true, false, {}};
        if (matches > 1)
            return {-1, false, true, "ambiguous_stable_id"};

        return {-1, false, false, "invalid_stable_id"};
    }

    if (index >= 0)
    {
        if (index < static_cast<int>(descriptors.size()))
            return {index, true, false, {}};
        return {-1, false, false, "invalid_param_id"};
    }

    const auto query = name.trim().toLowerCase();
    if (query.isEmpty())
        return {-1, false, false, "missing_parameter_identifier"};

    int matched = -1;
    int matches = 0;
    for (const auto& descriptor : descriptors)
    {
        if (descriptor.name.trim().toLowerCase() == query)
        {
            matched = descriptor.index;
            ++matches;
        }
    }

    if (matches == 1)
        return {matched, true, false, {}};
    if (matches > 1)
        return {-1, false, true, "ambiguous_param_name"};

    matched = -1;
    matches = 0;
    for (const auto& descriptor : descriptors)
    {
        if (descriptor.name.toLowerCase().contains(query))
        {
            matched = descriptor.index;
            ++matches;
        }
    }

    if (matches == 1)
        return {matched, true, false, {}};
    if (matches > 1)
        return {-1, false, true, "ambiguous_param_name"};

    return {-1, false, false, "invalid_param_name"};
}

} // namespace more_phi
