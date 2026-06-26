/*
 * More-Phi — Core/TransientShaper.cpp
 */
#include "Core/TransientShaper.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

TransientShaper::TransientShaper() = default;

void TransientShaper::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    // One-pole coeff for a time constant τ: a = exp(-1 / (τ * fs)).
    // Fast: 3 ms (tracks transients). Slow: 150 ms (tracks sustain body).
    const double fastMs = 3.0;
    const double slowMs = 150.0;
    fastCoeff_ = static_cast<float>(std::exp(-1.0 / (fastMs * 0.001 * sampleRate_)));
    slowCoeff_ = static_cast<float>(std::exp(-1.0 / (slowMs * 0.001 * sampleRate_)));
    reset();
}

void TransientShaper::reset() noexcept
{
    for (auto& c : channels_) { c.fastEnv = 0.0f; c.slowEnv = 0.0f; }
}

void TransientShaper::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float amount = amount_.load(std::memory_order_relaxed);
    if (amount == 0.0f && outputGain_.load(std::memory_order_relaxed) == 1.0f)
        return;  // unity bypass — nothing to do

    const int numCh = std::min(buf.getNumChannels(), kMaxChannels);
    const int ns = buf.getNumSamples();
    if (numCh <= 0 || ns <= 0) return;

    const float aFast = fastCoeff_;
    const float aSlow = slowCoeff_;
    const float outGain = outputGain_.load(std::memory_order_relaxed);
    const float amt = amount;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto& st = channels_[static_cast<size_t>(ch)];
        float* data = buf.getWritePointer(ch);
        for (int i = 0; i < ns; ++i)
        {
            const float x = data[i];
            const float x2 = x * x;
            // One-pole mean-square followers. Fast tracks the instantaneous
            // energy; slow tracks the sustain baseline.
            st.fastEnv = aFast * st.fastEnv + (1.0f - aFast) * x2;
            st.slowEnv = aSlow * st.slowEnv + (1.0f - aSlow) * x2;
            // Gain from the transient/sustain ratio. envFast > envSlow on
            // attacks (ratio > 1) → emphasis when amount > 0.
            const float ratio = st.slowEnv > kEnvFloor
                ? std::sqrt(st.fastEnv / st.slowEnv)
                : 1.0f;
            float g = 1.0f + amt * (ratio - 1.0f);
            g = std::clamp(g, kGainMin, kGainMax);
            data[i] = x * g * outGain;
        }
    }
}

} // namespace more_phi
