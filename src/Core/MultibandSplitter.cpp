/*
 * More-Phi — Core/MultibandSplitter.cpp
 */
#include "MultibandSplitter.h"
#include <cmath>
#include <cstring>

namespace more_phi {

MultibandSplitter::MultibandSplitter()
{
    crossFreqs_[0] = kDefaultCrossFreqs[0];
    crossFreqs_[1] = kDefaultCrossFreqs[1];
    crossFreqs_[2] = kDefaultCrossFreqs[2];
}

void MultibandSplitter::setCrossoverFrequencies(float f1, float f2, float f3) noexcept
{
    crossFreqs_[0] = f1;
    crossFreqs_[1] = f2;
    crossFreqs_[2] = f3;
}

void MultibandSplitter::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;
    computeAllCoeffs();
    reset();
}

void MultibandSplitter::reset() noexcept
{
    for (int x = 0; x < 3; ++x)
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            xo_[x].lpS1[ch] = {};
            xo_[x].lpS2[ch] = {};
            xo_[x].hpS1[ch] = {};
            xo_[x].hpS2[ch] = {};
        }
    }
}

void MultibandSplitter::computeAllCoeffs() noexcept
{
    for (int i = 0; i < 3; ++i)
        computeButterworth2(crossFreqs_[i], sampleRate_, xo_[i].lp, xo_[i].hp);
}

// 2nd-order Butterworth LP+HP via bilinear transform.
// K = tan(π·fc/fs); Q = 1/√2 = 0.7071 (Butterworth maximally flat)
void MultibandSplitter::computeButterworth2(float fc, double sr,
                                             BiquadCoeffs& lp,
                                             BiquadCoeffs& hp) noexcept
{
    const float K    = std::tan(3.14159265f * fc / static_cast<float>(sr));
    const float K2   = K * K;
    const float sqr2 = 1.41421356f;   // sqrt(2) = Q reciprocal for Butterworth
    const float norm = 1.0f / (1.0f + sqr2 * K + K2);

    // Low-pass
    lp.b0 =  K2 * norm;
    lp.b1 =  2.0f * K2 * norm;
    lp.b2 =  K2 * norm;
    lp.a1 =  2.0f * (K2 - 1.0f) * norm;
    lp.a2 = (1.0f - sqr2 * K + K2) * norm;

    // High-pass (same denominator)
    hp.b0 =  norm;
    hp.b1 = -2.0f * norm;
    hp.b2 =  norm;
    hp.a1 =  lp.a1;
    hp.a2 =  lp.a2;
}

// ── Audio thread ─────────────────────────────────────────────────────────────

void MultibandSplitter::processBlock(const juce::AudioBuffer<float>& input,
                                      juce::AudioBuffer<float>        bands[kNumBands]) noexcept
{
    const int numCh = std::min(input.getNumChannels(), kMaxChannels);
    const int ns    = input.getNumSamples();

    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* in = input.getReadPointer(ch);
        float* b0 = bands[0].getWritePointer(ch);
        float* b1 = bands[1].getWritePointer(ch);
        float* b2 = bands[2].getWritePointer(ch);
        float* b3 = bands[3].getWritePointer(ch);

        for (int i = 0; i < ns; ++i)
        {
            const float x = in[i];

            // Crossover 0 (80 Hz): splits into sub and rest
            const float sub  = processLR4_LP(x, 0, ch);
            const float h0   = processLR4_HP(x, 0, ch);

            // Crossover 1 (250 Hz): splits rest into low and rest2
            const float low  = processLR4_LP(h0, 1, ch);
            const float h1   = processLR4_HP(h0, 1, ch);

            // Crossover 2 (5000 Hz): splits rest2 into mid and high
            const float mid  = processLR4_LP(h1, 2, ch);
            const float high = processLR4_HP(h1, 2, ch);

            b0[i] = sub;
            b1[i] = low;
            b2[i] = mid;
            b3[i] = high;
        }
    }
}

} // namespace more_phi
