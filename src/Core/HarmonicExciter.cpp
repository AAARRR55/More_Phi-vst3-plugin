/*
 * More-Phi — Core/HarmonicExciter.cpp
 */
#include "HarmonicExciter.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

static constexpr float kPi = 3.14159265358979f;

HarmonicExciter::HarmonicExciter()
{
    hpState_.fill({0.f, 0.f});
}

void HarmonicExciter::setCutoff(float hz) noexcept
{
    cutoffHz_.store(hz, std::memory_order_relaxed);
    // Coefficients are recomputed in prepare(); if called at runtime,
    // they'll update on the next prepare() call. For live changes call
    // recomputeCoefficients() explicitly from the message thread.
    recomputeCoefficients();
}

void HarmonicExciter::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    recomputeCoefficients();
    reset();
}

void HarmonicExciter::reset() noexcept
{
    hpState_.fill({0.f, 0.f});
}

void HarmonicExciter::recomputeCoefficients() noexcept
{
    // 2nd-order Butterworth HP using bilinear transform
    const float fc   = cutoffHz_.load(std::memory_order_relaxed);
    const float fs   = static_cast<float>(sampleRate_);
    const float wc   = std::tan(kPi * fc / fs);
    const float wc2  = wc * wc;
    const float sq2  = 1.41421356f;                 // sqrt(2)
    const float norm = 1.f / (1.f + sq2 * wc + wc2);

    b0_ =  norm;
    b1_ = -2.f * norm;
    b2_ =  norm;
    a1_ =  2.f * (wc2 - 1.f) * norm;
    a2_ =  (1.f - sq2 * wc + wc2) * norm;
}

void HarmonicExciter::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed)) return;

    const float drive  = driveLinear_.load(std::memory_order_relaxed);
    const float wet    = dryWet_.load(std::memory_order_relaxed);
    const float dry    = 1.0f - wet;
    const int   ns     = buf.getNumSamples();
    const int   nch    = std::min(buf.getNumChannels(), kMaxChannels);

    for (int ch = 0; ch < nch; ++ch)
    {
        float* data = buf.getWritePointer(ch);
        BiquadState& st = hpState_[ch];

        for (int i = 0; i < ns; ++i)
        {
            const float in = data[i];

            // Transposed direct-form II biquad (HP)
            const float hp  = b0_ * in + st.z1;
            st.z1 = b1_ * in - a1_ * hp + st.z2;
            st.z2 = b2_ * in - a2_ * hp;

            // Drive + soft saturation via Padé tanh
            const float driven  = hp * drive;
            const float excited = fastTanh(driven);

            data[i] = dry * in + wet * excited;
        }
    }
}

} // namespace more_phi
