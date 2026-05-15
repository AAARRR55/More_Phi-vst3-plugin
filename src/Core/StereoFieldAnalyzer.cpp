#include "StereoFieldAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEps = 1.0e-12f;

float safeRatio(float numerator, float denominator) noexcept
{
    return denominator > kEps ? numerator / denominator : 0.0f;
}

} // namespace

StereoFieldAnalyzer::StereoFieldAnalyzer() = default;
StereoFieldAnalyzer::~StereoFieldAnalyzer() = default;

void StereoFieldAnalyzer::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    windowSamples_ = std::max(1, static_cast<int>(sampleRate_ * 0.1));
    computeAllCoeffs();
    reset();
}

void StereoFieldAnalyzer::reset() noexcept
{
    for (auto& crossover : crossovers_)
    {
        for (auto& state : crossover.lpState)
            state = {};
        for (auto& state : crossover.hpState)
            state = {};
    }

    sumMS_.fill(0.0f);
    sumM2_.fill(0.0f);
    sumS2_.fill(0.0f);
    samplesInWindow_ = 0;
    frameIndex_ = 0;

    StereoFieldSnapshot empty;
    empty.sampleRate = sampleRate_;
    empty.windowSamples = windowSamples_;
    publishSnapshot(empty);
}

void StereoFieldAnalyzer::processBlock(const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    const float* left = buffer.getReadPointer(0);
    const float* right = numChannels > 1 ? buffer.getReadPointer(1) : left;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float mid = 0.5f * (left[sample] + right[sample]);
        const float side = 0.5f * (left[sample] - right[sample]);

        std::array<float, kNumBands> midBands {};
        std::array<float, kNumBands> sideBands {};
        splitBands(mid, 0, midBands);
        splitBands(side, 1, sideBands);

        for (int band = 0; band < kNumBands; ++band)
        {
            const float m = midBands[static_cast<size_t>(band)];
            const float s = sideBands[static_cast<size_t>(band)];
            sumMS_[static_cast<size_t>(band)] += m * s;
            sumM2_[static_cast<size_t>(band)] += m * m;
            sumS2_[static_cast<size_t>(band)] += s * s;
        }

        if (++samplesInWindow_ >= windowSamples_)
            publishCurrentWindow();
    }
}

bool StereoFieldAnalyzer::getSnapshot(StereoFieldSnapshot& out) const noexcept
{
    for (int attempt = 0; attempt < kMaxReadRetries; ++attempt)
    {
        const auto before = version_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue;

        out = publishedSnapshot_;

        const auto after = version_.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0u)
            return true;
    }

    return false;
}

void StereoFieldAnalyzer::computeButterworth2(float cutoffHz, double sampleRate, BiquadCoeffs& lp, BiquadCoeffs& hp) noexcept
{
    const float clampedCutoff = std::clamp(cutoffHz, 1.0f, static_cast<float>(sampleRate * 0.45));
    const float k = std::tan(kPi * clampedCutoff / static_cast<float>(sampleRate));
    const float k2 = k * k;
    const float sqrt2 = 1.41421356237f;
    const float norm = 1.0f / (1.0f + sqrt2 * k + k2);

    lp.b0 = k2 * norm;
    lp.b1 = 2.0f * k2 * norm;
    lp.b2 = k2 * norm;
    lp.a1 = 2.0f * (k2 - 1.0f) * norm;
    lp.a2 = (1.0f - sqrt2 * k + k2) * norm;

    hp.b0 = norm;
    hp.b1 = -2.0f * norm;
    hp.b2 = norm;
    hp.a1 = lp.a1;
    hp.a2 = lp.a2;
}

float StereoFieldAnalyzer::processBiquad(float input, BiquadState& state, const BiquadCoeffs& coeffs) noexcept
{
    const float output = coeffs.b0 * input + state.z1;
    state.z1 = coeffs.b1 * input - coeffs.a1 * output + state.z2;
    state.z2 = coeffs.b2 * input - coeffs.a2 * output;
    return output;
}

void StereoFieldAnalyzer::computeAllCoeffs() noexcept
{
    for (size_t i = 0; i < crossovers_.size(); ++i)
        computeButterworth2(crossoverFrequencies_[i], sampleRate_, crossovers_[i].lp, crossovers_[i].hp);
}

void StereoFieldAnalyzer::splitBands(float input, int streamIndex, std::array<float, kNumBands>& bands) noexcept
{
    const auto stream = static_cast<size_t>(streamIndex);
    bands[0] = processBiquad(input, crossovers_[0].lpState[stream], crossovers_[0].lp);
    const float rest0 = processBiquad(input, crossovers_[0].hpState[stream], crossovers_[0].hp);

    bands[1] = processBiquad(rest0, crossovers_[1].lpState[stream], crossovers_[1].lp);
    const float rest1 = processBiquad(rest0, crossovers_[1].hpState[stream], crossovers_[1].hp);

    bands[2] = processBiquad(rest1, crossovers_[2].lpState[stream], crossovers_[2].lp);
    bands[3] = processBiquad(rest1, crossovers_[2].hpState[stream], crossovers_[2].hp);
}

void StereoFieldAnalyzer::publishCurrentWindow() noexcept
{
    StereoFieldSnapshot snapshot;
    snapshot.sampleRate = sampleRate_;
    snapshot.windowSamples = windowSamples_;
    snapshot.frameIndex = ++frameIndex_;

    float totalM2 = 0.0f;
    float totalS2 = 0.0f;

    for (int band = 0; band < kNumBands; ++band)
    {
        const auto idx = static_cast<size_t>(band);
        const float m2 = sumM2_[idx];
        const float s2 = sumS2_[idx];
        const float denom = std::sqrt(std::max(m2 * s2, 0.0f));

        snapshot.correlation[idx] = denom > kEps ? std::clamp(sumMS_[idx] / denom, -1.0f, 1.0f) : 0.0f;
        snapshot.msEnergyRatio[idx] = safeRatio(s2, m2);

        totalM2 += m2;
        totalS2 += s2;
    }

    snapshot.stereoWidth = std::sqrt(safeRatio(totalS2, totalM2));
    publishSnapshot(snapshot);

    sumMS_.fill(0.0f);
    sumM2_.fill(0.0f);
    sumS2_.fill(0.0f);
    samplesInWindow_ = 0;
}

void StereoFieldAnalyzer::publishSnapshot(const StereoFieldSnapshot& snapshot) noexcept
{
    const auto before = version_.load(std::memory_order_relaxed);
    version_.store(before + 1u, std::memory_order_release);
    publishedSnapshot_ = snapshot;
    version_.store(before + 2u, std::memory_order_release);
}

} // namespace more_phi
