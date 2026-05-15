/*
 * More-Phi — Core/StereoImager.cpp
 */
#include "StereoImager.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

StereoImager::StereoImager()
{
    // Default width curve
    width_[0].store(0.0f, std::memory_order_relaxed);  // Sub: mono
    width_[1].store(0.7f, std::memory_order_relaxed);  // Low
    width_[2].store(1.0f, std::memory_order_relaxed);  // Mid: natural
    width_[3].store(1.3f, std::memory_order_relaxed);  // High: enhanced
}

void StereoImager::setWidth(int regionIndex, float width) noexcept
{
    if (regionIndex >= 0 && regionIndex < kNumRegions)
        width_[regionIndex].store(width, std::memory_order_relaxed);
}

void StereoImager::setWidthCurve(float sub, float low, float mid, float high) noexcept
{
    width_[0].store(sub,  std::memory_order_relaxed);
    width_[1].store(low,  std::memory_order_relaxed);
    width_[2].store(mid,  std::memory_order_relaxed);
    width_[3].store(high, std::memory_order_relaxed);
}

void StereoImager::computeAllCoeffs() noexcept
{
    for (int i = 0; i < 3; ++i)
        computeButterworth2(crossFreqs_[i], sampleRate_, xo_[i].lp, xo_[i].hp);
}

void StereoImager::computeButterworth2(float fc, double sr,
                                        BiquadCoeffs& lp,
                                        BiquadCoeffs& hp) noexcept
{
    const float K    = std::tan(3.14159265f * fc / static_cast<float>(sr));
    const float K2   = K * K;
    const float sq2  = 1.41421356f;
    const float norm = 1.0f / (1.0f + sq2 * K + K2);

    lp.b0 = K2 * norm;
    lp.b1 = 2.0f * K2 * norm;
    lp.b2 = K2 * norm;
    lp.a1 = 2.0f * (K2 - 1.0f) * norm;
    lp.a2 = (1.0f - sq2 * K + K2) * norm;

    hp.b0 =  norm;
    hp.b1 = -2.0f * norm;
    hp.b2 =  norm;
    hp.a1 =  lp.a1;
    hp.a2 =  lp.a2;
}

float StereoImager::processLR4_LP(float x, int idx, int ch) noexcept
{
    x = processBiquad(x, xo_[idx].lpS1[ch], xo_[idx].lp);
    x = processBiquad(x, xo_[idx].lpS2[ch], xo_[idx].lp);
    return x;
}

float StereoImager::processLR4_HP(float x, int idx, int ch) noexcept
{
    x = processBiquad(x, xo_[idx].hpS1[ch], xo_[idx].hp);
    x = processBiquad(x, xo_[idx].hpS2[ch], xo_[idx].hp);
    return x;
}

void StereoImager::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_          = sampleRate;
    corrWindowSamples_   = static_cast<int>(sampleRate * 0.1);  // 100 ms
    computeAllCoeffs();
    reset();
}

void StereoImager::reset() noexcept
{
    for (int x = 0; x < 3; ++x)
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            xo_[x].lpS1[ch] = xo_[x].lpS2[ch] = {};
            xo_[x].hpS1[ch] = xo_[x].hpS2[ch] = {};
        }
    corrAccumMS_ = corrAccumM_ = corrAccumS_ = 0.0f;
    corrSamples_ = 0;
    midWidthGuard_.store(1.0f, std::memory_order_relaxed);
}

void StereoImager::updateCorrelationGuard() noexcept
{
    if (corrSamples_ < 1) return;
    const float ms  = corrAccumMS_ / static_cast<float>(corrSamples_);
    const float m2  = corrAccumM_  / static_cast<float>(corrSamples_);
    const float s2  = corrAccumS_  / static_cast<float>(corrSamples_);
    const float den = std::sqrt(m2 * s2 + 1e-12f);
    const float corr = ms / den;

    // If correlation in the Mid band is strongly out-of-phase, reduce width
    const float guard = (corr < -0.3f) ? 0.5f : 1.0f;
    midWidthGuard_.store(guard, std::memory_order_relaxed);

    corrAccumMS_ = corrAccumM_ = corrAccumS_ = 0.0f;
    corrSamples_ = 0;
}

void StereoImager::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (buf.getNumChannels() < 2) return;

    const int ns = buf.getNumSamples();
    float* L = buf.getWritePointer(0);
    float* R = buf.getWritePointer(1);

    const float w0 = width_[0].load(std::memory_order_relaxed);
    const float w1 = width_[1].load(std::memory_order_relaxed);
    const float w2 = width_[2].load(std::memory_order_relaxed) *
                     midWidthGuard_.load(std::memory_order_relaxed);
    const float w3 = width_[3].load(std::memory_order_relaxed);
    const bool  guard = correlationGuardEnabled_.load(std::memory_order_relaxed);

    for (int i = 0; i < ns; ++i)
    {
        // Encode to M/S
        float m = (L[i] + R[i]) * 0.5f;
        float s = (L[i] - R[i]) * 0.5f;

        // Split M into 4 bands using LR4 crossovers (same filters for M and S)
        const float mSub  = processLR4_LP(m, 0, 0);
        const float mRest = processLR4_HP(m, 0, 0);
        const float mLow  = processLR4_LP(mRest, 1, 0);
        const float mR2   = processLR4_HP(mRest, 1, 0);
        const float mMid  = processLR4_LP(mR2,   2, 0);
        const float mHigh = processLR4_HP(mR2,   2, 0);

        // Split S into 4 bands (channel index 1 for S to keep filter states separate)
        const float sSub  = processLR4_LP(s, 0, 1);
        const float sRest = processLR4_HP(s, 0, 1);
        const float sLow  = processLR4_LP(sRest, 1, 1);
        const float sR2   = processLR4_HP(sRest, 1, 1);
        const float sMid  = processLR4_LP(sR2,   2, 1);
        const float sHigh = processLR4_HP(sR2,   2, 1);

        // Apply per-band width to Side channel
        const float sMixed = w0 * sSub + w1 * sLow + w2 * sMid + w3 * sHigh;

        // Recombine M bands (unchanged)
        const float mMixed = mSub + mLow + mMid + mHigh;

        // Correlation tracking for mid band (for guard update on message thread)
        if (guard)
        {
            corrAccumMS_ += mMid * sMid;
            corrAccumM_  += mMid * mMid;
            corrAccumS_  += sMid * sMid;
            ++corrSamples_;
            if (corrSamples_ >= corrWindowSamples_)
                updateCorrelationGuard();
        }

        // Decode M/S → L/R
        L[i] = mMixed + sMixed;
        R[i] = mMixed - sMixed;
    }
}

} // namespace more_phi
