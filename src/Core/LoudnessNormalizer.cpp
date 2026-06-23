/*
 * More-Phi — Core/LoudnessNormalizer.cpp
 */
#include "LoudnessNormalizer.h"
#include <algorithm>

namespace more_phi {

LoudnessNormalizer::LoudnessNormalizer() = default;

void LoudnessNormalizer::prepare(double sampleRate, LUFSMeter& meter) noexcept
{
    sampleRate_ = sampleRate;
    meter_      = &meter;
    // One-pole ramp: τ = 1 s → α = exp(-1/(sr * 1.0))
    rampCoeff_  = std::exp(-1.0f / static_cast<float>(sampleRate));
    reset();
}

void LoudnessNormalizer::reset() noexcept
{
    gainRamped_ = 1.0f;
    correctionDB_.store(0.0f, std::memory_order_relaxed);
    correctionGain_.store(1.0f, std::memory_order_relaxed);
}

void LoudnessNormalizer::updateCorrectionGain() noexcept
{
    if (!enabled_.load(std::memory_order_relaxed) || meter_ == nullptr)
    {
        correctionDB_.store(0.0f, std::memory_order_relaxed);
        correctionGain_.store(1.0f, std::memory_order_relaxed);
        return;
    }

    const float measuredLUFS = meter_->getIntegrated();

    // Don't correct until we have a valid measurement
    if (measuredLUFS <= -200.0f)
        return;

    // AUDIT-FIX (M3): subtract the target margin so the limiter has headroom.
    // Effective target = targetLUFS - margin. Default margin 0.0 => unchanged.
    const float target = targetLUFS_.load(std::memory_order_relaxed)
                       - targetMarginLU_.load(std::memory_order_relaxed);
    const float corrDB = std::clamp(target - measuredLUFS,
                                    kMinCorrectionDB,
                                    kMaxCorrectionDB);

    correctionDB_.store(corrDB, std::memory_order_relaxed);
    correctionGain_.store(std::pow(10.0f, corrDB / 20.0f), std::memory_order_relaxed);
}

void LoudnessNormalizer::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed)) return;

    const float targetGain = correctionGain_.load(std::memory_order_relaxed);
    const int   ns         = buf.getNumSamples();
    const int   nch        = buf.getNumChannels();

    for (int i = 0; i < ns; ++i)
    {
        // One-pole ramp toward target gain (prevents clicks)
        gainRamped_ = gainRamped_ * rampCoeff_ + targetGain * (1.0f - rampCoeff_);

        for (int ch = 0; ch < nch; ++ch)
            buf.getWritePointer(ch)[i] *= gainRamped_;
    }
}

} // namespace more_phi
