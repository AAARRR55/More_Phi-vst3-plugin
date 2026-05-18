/*
 * More-Phi — Core/BrickwallLimiter.cpp
 */
#include "BrickwallLimiter.h"
#include <algorithm>
#include <cstring>

namespace more_phi {

BrickwallLimiter::BrickwallLimiter()
{
    delayL_.fill(0.f);
    delayR_.fill(0.f);
}

void BrickwallLimiter::setCeiling(float dBTP) noexcept
{
    ceilingLinear_.store(std::pow(10.0f, dBTP / 20.0f), std::memory_order_relaxed);
}

void BrickwallLimiter::setRelease(float ms) noexcept
{
    releaseMs_.store(ms, std::memory_order_relaxed);
    recomputeReleaseCoeff();
}

void BrickwallLimiter::recomputeReleaseCoeff() noexcept
{
    const float ms = releaseMs_.load(std::memory_order_relaxed);
    releaseCoeff_ = std::exp(-1.0f / (ms * 0.001f * static_cast<float>(sampleRate_)));
}

void BrickwallLimiter::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_       = sampleRate;
    lookaheadSamples_ = std::min(static_cast<int>(sampleRate * 0.004),  // 4 ms
                                  kLookaheadBufSize);
    delayL_.fill(0.f);
    delayR_.fill(0.f);
    writePos_      = 0;
    gainSmoothed_  = 1.0f;
    recomputeReleaseCoeff();

    lookaheadBuf_.setSize(kMaxChannels, maxBlockSize + lookaheadSamples_ + 8);
    truePeak_.prepare(sampleRate, maxBlockSize + lookaheadSamples_ + 8);
}

void BrickwallLimiter::reset() noexcept
{
    delayL_.fill(0.f);
    delayR_.fill(0.f);
    writePos_     = 0;
    gainSmoothed_ = 1.0f;
    gainReductionDB_.store(0.0f, std::memory_order_relaxed);
    truePeak_.reset();
}

void BrickwallLimiter::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed))
    {
        truePeak_.processBlock(buf);
        return;
    }

    const int ns  = buf.getNumSamples();
    const int nch = std::min(buf.getNumChannels(), kMaxChannels);

    const float ceiling = ceilingLinear_.load(std::memory_order_relaxed);

    float maxGainReduction = 1.0f;  // track worst-case GR this block

    for (int i = 0; i < ns; ++i)
    {
        // ── 1. Write current sample into delay line ────────────────────────
        if (nch >= 1) delayL_[static_cast<size_t>(writePos_)] = buf.getReadPointer(0)[i];
        if (nch >= 2) delayR_[static_cast<size_t>(writePos_)] = buf.getReadPointer(1)[i];

        // ── 2. Scan lookahead window for true peak ────────────────────────
        // Fill a 1-sample lookahead scan buffer and run TruePeakEstimator
        // For efficiency we scan just the current incoming sample ahead
        // (the delay line already holds lookaheadSamples_ of future content)
        {
            // Peek at lookahead: newest sample written is writePos_
            // The sample from lookaheadSamples_ ago is the "current output"
            // We need the peak over [current_output ... write_pos]
            // Simple per-sample abs max scan over the ring buffer:
            float peakL = 0.f, peakR = 0.f;
            for (int k = 0; k < lookaheadSamples_; ++k)
            {
                const int idx = (writePos_ - k + kLookaheadBufSize) % kLookaheadBufSize;
                if (nch >= 1) { const float a = std::abs(delayL_[static_cast<size_t>(idx)]); if (a > peakL) peakL = a; }
                if (nch >= 2) { const float a = std::abs(delayR_[static_cast<size_t>(idx)]); if (a > peakR) peakR = a; }
            }
            const float peak = std::max(peakL, peakR);

            // ── 3. Compute target gain ───────────────────────────────────────
            float targetGain = 1.0f;
            if (peak > ceiling && peak > 1e-12f)
                targetGain = ceiling / peak;

            // ── 4. Smooth gain: instantaneous attack, exponential release ────
            if (targetGain < gainSmoothed_)
                gainSmoothed_ = targetGain;            // attack = instant
            else
                gainSmoothed_ = gainSmoothed_ * releaseCoeff_
                              + targetGain * (1.0f - releaseCoeff_);

            if (gainSmoothed_ < maxGainReduction)
                maxGainReduction = gainSmoothed_;
        }

        // ── 5. Output delayed sample with gain applied ────────────────────
        const int readPos = (writePos_ - lookaheadSamples_ + 1 + kLookaheadBufSize)
                            % kLookaheadBufSize;

        if (nch >= 1) buf.getWritePointer(0)[i] = delayL_[static_cast<size_t>(readPos)] * gainSmoothed_;
        if (nch >= 2) buf.getWritePointer(1)[i] = delayR_[static_cast<size_t>(readPos)] * gainSmoothed_;

        writePos_ = (writePos_ + 1) % kLookaheadBufSize;
    }

    truePeak_.processBlock(buf);

    // Update gain reduction meter
    const float grDB = (maxGainReduction < 1.0f && maxGainReduction > 1e-12f)
                       ? 20.0f * std::log10(maxGainReduction)
                       : 0.0f;
    gainReductionDB_.store(grDB, std::memory_order_relaxed);
}

} // namespace more_phi
