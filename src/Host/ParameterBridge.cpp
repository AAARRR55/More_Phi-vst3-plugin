/*
 * MorphSnap — Host/ParameterBridge.cpp
 * Uses the legacy AudioProcessor::setParameter() which directly calls
 * VST3's setParamNormalized via the JUCE wrapper.
 * Includes exception handling for robustness.
 */
#include "ParameterBridge.h"
#include "PluginHostManager.h"
#include <exception>
#include <cmath>

// Suppress deprecation warning for setParameter — it's the only way
// that reliably works for setting hosted VST3 plugin parameters.
JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4996)

namespace morphsnap {

// ── Private helpers ────────────────────────────────────────────────────────────

// Returns the plugin pointer or nullptr if not available.
// Avoids repeating the `host_.getPlugin()` null + try/catch guard in every method.
// Usage: wrap your body in the withPlugin() template below.
static juce::AudioPluginInstance* getHostPlugin(IPluginHostManager& host)
{
    return host.getPlugin();
}

// RAII helper: calls `fn(plugin)` only when plugin is non-null.
// Catches all exceptions and logs to DBG. Returns the default value on failure.
// This replaces the repeated 4-line guard in every method.
template<typename Ret, typename Fn>
static Ret withPlugin(IPluginHostManager& host, const char* context,
                      Ret defaultValue, Fn&& fn)
{
    auto* plugin = host.getPlugin();
    if (!plugin)
        return defaultValue;
    try
    {
        return fn(*plugin);
    }
    catch (...)
    {
        DBG(juce::String("Exception in ParameterBridge::") + context);
        return defaultValue;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

int ParameterBridge::getParameterCount() const
{
    return withPlugin(host_, "getParameterCount", 0, [](juce::AudioPluginInstance& plugin)
    {
        return plugin.getParameters().size();
    });
}

float ParameterBridge::getParameterNormalized(int index) const
{
    return withPlugin(host_, "getParameterNormalized", 0.0f,
        [index](juce::AudioPluginInstance& plugin) -> float
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= params.size()) return 0.0f;
        return params[index]->getValue();
    });
}

void ParameterBridge::setParameterNormalized(int index, float value)
{
    withPlugin(host_, "setParameterNormalized", 0,
        [this, index, value](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return 0;

        const float clamped = juce::jlimit(0.0f, 1.0f, value);
        const auto now = juce::Time::getMillisecondCounter();

        // High-density morphing can send thousands of updates per second.
        // If we've updated this parameter too recently AND the value hasn't
        // changed significantly, we throttle it to avoid CPU spikes in
        // the hosted plugin's GUI/controller refresh code.
        if (shouldThrottle(index, clamped, now))
            return 0;

        // Use the legacy setParameter() for reliable VST3/AU host compatibility.
        plugin.setParameter(index, clamped);

        // Update tracking state
        {
            std::lock_guard<std::mutex> lock(throttleMutex_);
            ensureThrottleSize((int)params.size());
            throttleStates_[index] = {clamped, now};
        }

        return 0;
    });
}

juce::String ParameterBridge::getParameterName(int index) const
{
    return withPlugin(host_, "getParameterName", juce::String{},
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

    withPlugin(host_, "applyParameterState", 0,
        [this, values, count](juce::AudioPluginInstance& plugin) -> int
    {
        auto& params = plugin.getParameters();
        const int safeCount = juce::jmin(count, (int)params.size());
        const auto now = juce::Time::getMillisecondCounter();

        for (int i = 0; i < safeCount; ++i)
        {
            const float clamped = juce::jlimit(0.0f, 1.0f, values[i]);
            
            // Apply throttling during batch updates
            if (!shouldThrottle(i, clamped, now))
            {
                plugin.setParameter(i, clamped);
                
                std::lock_guard<std::mutex> lock(throttleMutex_);
                ensureThrottleSize((int)params.size());
                throttleStates_[i] = {clamped, now};
            }
        }
        return 0;
    });
}

std::vector<float> ParameterBridge::captureParameterState() const
{
    return withPlugin(host_, "captureParameterState", std::vector<float>{},
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
    return withPlugin(host_, "isDiscrete", false,
        [index](juce::AudioPluginInstance& plugin) -> bool
    {
        auto& params = plugin.getParameters();
        if (index < 0 || index >= (int)params.size()) return false;
        auto* p = params[index];
        // JUCE's isDiscrete() covers VST3 params with step counts.
        // Also treat params with very few steps (≤32) as discrete —
        // these are typically toggles, dropdown selectors, waveform types etc.
        // that cause clicks/pops when interpolated during morphing.
        if (p->isDiscrete()) return true;
        int steps = p->getNumSteps();
        return steps > 0 && steps <= 32;
    });
}

std::vector<bool> ParameterBridge::getDiscreteMap() const
{
    return withPlugin(host_, "getDiscreteMap", std::vector<bool>{},
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

// ── Private Implementation ─────────────────────────────────────────────────────

void ParameterBridge::ensureThrottleSize(int count) const
{
    if (throttleStates_.size() < (size_t)count)
        throttleStates_.resize(count);
}

bool ParameterBridge::shouldThrottle(int index, float newValue, juce::uint32 now) const
{
    std::lock_guard<std::mutex> lock(throttleMutex_);
    
    if (index >= (int)throttleStates_.size())
        return false;
        
    const auto& state = throttleStates_[index];
    
    // 1. Initial update (lastValue == -1.0f)
    if (state.lastValue < 0.0f)
        return false;
        
    // 2. Minimum time threshold (e.g., 2ms between updates)
    // We allow 500 updates per second per parameter, which is more than enough
    // for smooth visual morphing without saturating hosted plugin GUI threads.
    const juce::uint32 delta = now - state.lastUpdateTime;
    if (delta >= 2) 
        return false;
        
    // 3. Significance guard: If the change is significant (> 1% of range), 
    // allow it anyway to ensure accuracy during high-speed morphs.
    if (std::abs(newValue - state.lastValue) > 0.01f)
        return false;
        
    // 4. Finality guard: If it's the exact same value as before, it's likely
    // the final "landing" value of a morph segment.
    if (newValue == state.lastValue)
        return true;
        
    return true;
}

} // namespace morphsnap

JUCE_END_IGNORE_WARNINGS_MSVC
