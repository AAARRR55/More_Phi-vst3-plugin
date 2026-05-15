/*
 * More-Phi — Core/MultibandDynamicsProcessor.cpp
 */
#include "MultibandDynamicsProcessor.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

struct DefaultBandParams {
    float threshDB, ratio, attackMs, releaseMs;
};

static const DefaultBandParams kDefaults[MultibandDynamicsProcessor::kNumBands] = {
    { -18.f, 1.5f,  50.f, 200.f },   // 0: Sub
    { -20.f, 2.5f,  15.f, 150.f },   // 1: Low
    { -22.f, 3.0f,   8.f, 120.f },   // 2: Mid
    { -18.f, 2.0f,   3.f,  80.f },   // 3: High
};

MultibandDynamicsProcessor::MultibandDynamicsProcessor()
{
    applyDefaults();
    for (auto& gr : grDB_)
        gr.store(0.f, std::memory_order_relaxed);
}

void MultibandDynamicsProcessor::applyDefaults() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        bd.thresholdLinear.store(std::pow(10.f, kDefaults[b].threshDB / 20.f), std::memory_order_relaxed);
        bd.ratio.store(kDefaults[b].ratio, std::memory_order_relaxed);
        bd.attackMs.store(kDefaults[b].attackMs, std::memory_order_relaxed);
        bd.releaseMs.store(kDefaults[b].releaseMs, std::memory_order_relaxed);
        bd.makeupLinear.store(1.0f, std::memory_order_relaxed);
        bd.kneeDB.store(2.0f, std::memory_order_relaxed);
        bd.enabled.store(true, std::memory_order_relaxed);
    }
}

void MultibandDynamicsProcessor::setBandParams(int band, const BandParams& p) noexcept
{
    if (band < 0 || band >= kNumBands) return;
    auto& bd = bands_[band];
    bd.thresholdLinear.store(std::pow(10.f, p.thresholdDB / 20.f), std::memory_order_relaxed);
    bd.ratio.store(p.ratio, std::memory_order_relaxed);
    bd.attackMs.store(p.attackMs, std::memory_order_relaxed);
    bd.releaseMs.store(p.releaseMs, std::memory_order_relaxed);
    bd.makeupLinear.store(std::pow(10.f, p.makeupDB / 20.f), std::memory_order_relaxed);
    bd.kneeDB.store(p.kneeDB, std::memory_order_relaxed);

    // Recompute envelope follower coefficients now
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        bd.followers[ch].setAttack(p.attackMs);
        bd.followers[ch].setRelease(p.releaseMs);
    }
}

MultibandDynamicsProcessor::BandParams MultibandDynamicsProcessor::getBandParams(int band) const noexcept
{
    if (band < 0 || band >= kNumBands) return {};
    const auto& bd = bands_[band];
    BandParams p;
    const float tl = bd.thresholdLinear.load(std::memory_order_relaxed);
    p.thresholdDB = (tl > 1e-12f) ? 20.f * std::log10(tl) : -120.f;
    p.ratio       = bd.ratio.load(std::memory_order_relaxed);
    p.attackMs    = bd.attackMs.load(std::memory_order_relaxed);
    p.releaseMs   = bd.releaseMs.load(std::memory_order_relaxed);
    const float ml = bd.makeupLinear.load(std::memory_order_relaxed);
    p.makeupDB    = (ml > 1e-12f) ? 20.f * std::log10(ml) : 0.f;
    p.kneeDB      = bd.kneeDB.load(std::memory_order_relaxed);
    return p;
}

void MultibandDynamicsProcessor::setBandEnabled(int band, bool enabled) noexcept
{
    if (band >= 0 && band < kNumBands)
        bands_[band].enabled.store(enabled, std::memory_order_relaxed);
}

void MultibandDynamicsProcessor::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            bd.followers[ch].prepare(sampleRate);
            bd.followers[ch].setAttack(bd.attackMs.load(std::memory_order_relaxed));
            bd.followers[ch].setRelease(bd.releaseMs.load(std::memory_order_relaxed));
            bd.gainSmoothed[ch] = 1.0f;
        }
    }
}

void MultibandDynamicsProcessor::reset() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            bd.followers[ch].reset();
            bd.gainSmoothed[ch] = 1.0f;
        }
        grDB_[b].store(0.f, std::memory_order_relaxed);
    }
}

float MultibandDynamicsProcessor::computeCompressorGain(float rmsLinear,
                                                         float threshLinear,
                                                         float ratio,
                                                         float kneeDB,
                                                         float makeup) const noexcept
{
    const float rmsDB  = (rmsLinear > 1e-12f) ? 20.f * std::log10(rmsLinear) : -120.f;
    const float thrDB  = (threshLinear > 1e-12f) ? 20.f * std::log10(threshLinear) : -120.f;
    const float excess = rmsDB - thrDB;
    const float kneeH  = kneeDB * 0.5f;

    float gainDB = 0.f;
    if (excess <= -kneeH)
    {
        gainDB = 0.f;  // below knee: no reduction
    }
    else if (excess < kneeH)
    {
        // Soft-knee interpolation
        const float t = (excess + kneeH) / kneeDB;
        gainDB = (1.f / ratio - 1.f) * (excess + kneeH) * (excess + kneeH) / (2.f * kneeDB);
    }
    else
    {
        gainDB = excess / ratio - excess;  // = excess * (1/ratio - 1)
    }

    return makeup * std::pow(10.f, gainDB / 20.f);
}

void MultibandDynamicsProcessor::processBlock(juce::AudioBuffer<float> bands[kNumBands]) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        if (!bd.enabled.load(std::memory_order_relaxed)) continue;

        const float threshLinear = bd.thresholdLinear.load(std::memory_order_relaxed);
        const float ratio        = bd.ratio.load(std::memory_order_relaxed);
        const float kneeDB       = bd.kneeDB.load(std::memory_order_relaxed);
        const float makeup       = bd.makeupLinear.load(std::memory_order_relaxed);

        juce::AudioBuffer<float>& buf = bands[b];
        const int ns  = buf.getNumSamples();
        const int nch = std::min(buf.getNumChannels(), kMaxChannels);

        float worstGR = 1.0f;

        for (int ch = 0; ch < nch; ++ch)
        {
            float* data = buf.getWritePointer(ch);

            // RMS detection via EnvelopeFollower
            const float rms = bd.followers[ch].process(data, ns);

            // Compute target gain
            const float targetGain = computeCompressorGain(rms, threshLinear, ratio, kneeDB, makeup);

            // Smooth gain
            const float attackCoeff  = std::exp(-1.f / (bd.attackMs.load(std::memory_order_relaxed)
                                                * 0.001f * static_cast<float>(sampleRate_)));
            const float releaseCoeff = std::exp(-1.f / (bd.releaseMs.load(std::memory_order_relaxed)
                                                * 0.001f * static_cast<float>(sampleRate_)));

            for (int i = 0; i < ns; ++i)
            {
                if (targetGain < bd.gainSmoothed[ch])
                    bd.gainSmoothed[ch] = bd.gainSmoothed[ch] * attackCoeff + targetGain * (1.f - attackCoeff);
                else
                    bd.gainSmoothed[ch] = bd.gainSmoothed[ch] * releaseCoeff + targetGain * (1.f - releaseCoeff);

                data[i] *= bd.gainSmoothed[ch];
            }

            if (bd.gainSmoothed[ch] < worstGR) worstGR = bd.gainSmoothed[ch];
        }

        // Update GR meter
        const float grDB = (worstGR < 1.f && worstGR > 1e-12f)
                           ? 20.f * std::log10(worstGR) : 0.f;
        grDB_[b].store(grDB, std::memory_order_relaxed);
    }
}

} // namespace more_phi
