#include "RealtimeSpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace more_phi {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEps = 1.0e-12f;
constexpr float kMinDB = -120.0f;
constexpr float kMaxDB = 24.0f;

float amplitudeToDB(float value) noexcept
{
    return std::clamp(20.0f * std::log10(std::max(value, kEps)), kMinDB, kMaxDB);
}

} // namespace

RealtimeSpectrumAnalyzer::RealtimeSpectrumAnalyzer() = default;
RealtimeSpectrumAnalyzer::~RealtimeSpectrumAnalyzer() = default;

void RealtimeSpectrumAnalyzer::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    fftSize_ = kDefaultFFTSize;
    hopSize_ = kDefaultHopSize;
    numBins_ = fftSize_ / 2 + 1;
    writePos_ = 0;
    hopCounter_ = 0;
    frameIndex_ = 0;

    fft_ = std::make_unique<juce::dsp::FFT>(11);
    window_.assign(static_cast<size_t>(fftSize_), 0.0f);
    inputBuffer_.assign(static_cast<size_t>(fftSize_), 0.0f);
    linearFrame_.assign(static_cast<size_t>(fftSize_), 0.0f);
    fftScratch_.assign(static_cast<size_t>(fftSize_ * 2), 0.0f);
    rawMagnitude_.assign(static_cast<size_t>(numBins_), 0.0f);
    previousMagnitude_.assign(static_cast<size_t>(numBins_), 0.0f);
    computeHannWindow();
    reset();
}

void RealtimeSpectrumAnalyzer::reset() noexcept
{
    std::fill(inputBuffer_.begin(), inputBuffer_.end(), 0.0f);
    std::fill(linearFrame_.begin(), linearFrame_.end(), 0.0f);
    std::fill(fftScratch_.begin(), fftScratch_.end(), 0.0f);
    std::fill(rawMagnitude_.begin(), rawMagnitude_.end(), 0.0f);
    std::fill(previousMagnitude_.begin(), previousMagnitude_.end(), 0.0f);

    writePos_ = 0;
    hopCounter_ = 0;
    frameIndex_ = 0;

    SpectrumSnapshot empty;
    empty.binCount = kMaxBins;
    empty.sampleRate = sampleRate_;
    empty.fftSize = fftSize_;
    empty.magnitudeDB.fill(kMinDB);
    publishSnapshot(empty);
}

void RealtimeSpectrumAnalyzer::processBlock(const juce::AudioBuffer<float>& buffer) noexcept
{
    if (fft_ == nullptr || inputBuffer_.empty() || buffer.getNumSamples() <= 0)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(buffer.getNumChannels(), 2);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        float mono = 0.0f;
        if (numChannels == 1)
        {
            mono = buffer.getSample(0, sample);
        }
        else if (numChannels >= 2)
        {
            mono = 0.5f * (buffer.getSample(0, sample) + buffer.getSample(1, sample));
        }

        inputBuffer_[static_cast<size_t>(writePos_)] = mono;
        writePos_ = (writePos_ + 1) % fftSize_;

        if (++hopCounter_ >= hopSize_)
        {
            hopCounter_ = 0;
            processFrame();
        }
    }
}

bool RealtimeSpectrumAnalyzer::getSnapshot(SpectrumSnapshot& out) const noexcept
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

void RealtimeSpectrumAnalyzer::processFrame() noexcept
{
    // AUDIT-FIX: Write windowed samples in JUCE's interleaved real/imag format.
    // Previously samples were packed contiguously (scratch[i]=x[i]) which JUCE's
    // performRealOnlyForwardTransform interprets as x[0]=real[0], x[1]=imag[0],
    // x[2]=real[1], ... — corrupting the entire spectrum from bin 0 upward.
    for (int i = 0; i < fftSize_; ++i)
    {
        const int src = (writePos_ + i) % fftSize_;
        const float sample = inputBuffer_[static_cast<size_t>(src)] * window_[static_cast<size_t>(i)];
        linearFrame_[static_cast<size_t>(i)] = sample;
        fftScratch_[static_cast<size_t>(i * 2)]     = sample;   // real
        fftScratch_[static_cast<size_t>(i * 2 + 1)] = 0.0f;     // imag = 0 (purely real input)
    }
    // The loop above now fills the entire fftScratch_ (positions 0..2*fftSize_-1),
    // so the old trailing-zero fill is no longer needed.

    fft_->performRealOnlyForwardTransform(fftScratch_.data(), true);

    SpectrumSnapshot snapshot;
    snapshot.binCount = kMaxBins;
    snapshot.sampleRate = sampleRate_;
    snapshot.fftSize = fftSize_;
    snapshot.frameIndex = ++frameIndex_;

    float energySum = 0.0f;
    float weightedFreqSum = 0.0f;
    float fluxSum = 0.0f;
    float peak = 0.0f;
    float rmsSum = 0.0f;

    for (int i = 0; i < fftSize_; ++i)
    {
        const float s = linearFrame_[static_cast<size_t>(i)];
        peak = std::max(peak, std::abs(s));
        rmsSum += s * s;
    }

    for (int bin = 0; bin < numBins_; ++bin)
    {
        float real = 0.0f;
        float imag = 0.0f;

        real = fftScratch_[static_cast<size_t>(bin * 2)];
        imag = fftScratch_[static_cast<size_t>(bin * 2 + 1)];

        const float mag = std::sqrt(real * real + imag * imag) / static_cast<float>(fftSize_);
        const float energy = mag * mag;
        const float freq = static_cast<float>(bin) * static_cast<float>(sampleRate_) / static_cast<float>(fftSize_);
        rawMagnitude_[static_cast<size_t>(bin)] = mag;
        energySum += energy;
        weightedFreqSum += freq * energy;

        const float delta = mag - previousMagnitude_[static_cast<size_t>(bin)];
        if (delta > 0.0f)
            fluxSum += delta * delta;
    }

    snapshot.spectralCentroid = energySum > kEps ? weightedFreqSum / energySum : 0.0f;
    snapshot.spectralFlux = std::sqrt(fluxSum / static_cast<float>(std::max(1, numBins_)));

    const float rms = std::sqrt(rmsSum / static_cast<float>(std::max(1, fftSize_)));
    snapshot.crestFactor = rms > kEps ? peak / rms : 0.0f;

    const float rolloffTarget = energySum * 0.85f;
    float cumulative = 0.0f;
    snapshot.spectralRolloff = 0.0f;
    for (int bin = 0; bin < numBins_; ++bin)
    {
        const float mag = rawMagnitude_[static_cast<size_t>(bin)];
        cumulative += mag * mag;
        if (cumulative >= rolloffTarget)
        {
            snapshot.spectralRolloff = static_cast<float>(bin) * static_cast<float>(sampleRate_) / static_cast<float>(fftSize_);
            break;
        }
    }

    const int rawBinsPerPublished = std::max(1, (numBins_ - 1) / kMaxBins);
    for (int outBin = 0; outBin < kMaxBins; ++outBin)
    {
        const int start = outBin * rawBinsPerPublished;
        const int end = outBin == kMaxBins - 1 ? numBins_ : std::min(numBins_, start + rawBinsPerPublished);
        float sum = 0.0f;
        int count = 0;
        for (int rawBin = start; rawBin < end; ++rawBin)
        {
            sum += rawMagnitude_[static_cast<size_t>(rawBin)];
            ++count;
        }
        snapshot.magnitudeDB[static_cast<size_t>(outBin)] = amplitudeToDB(count > 0 ? sum / static_cast<float>(count) : 0.0f);
    }

    float sumX = 0.0f;
    float sumY = 0.0f;
    float sumXX = 0.0f;
    float sumXY = 0.0f;
    int regressionCount = 0;
    for (int bin = 1; bin < numBins_; ++bin)
    {
        const float freq = static_cast<float>(bin) * static_cast<float>(sampleRate_) / static_cast<float>(fftSize_);
        if (freq <= 0.0f)
            continue;

        const float x = std::log2(freq);
        const float y = amplitudeToDB(rawMagnitude_[static_cast<size_t>(bin)]);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        ++regressionCount;
    }

    const float denom = static_cast<float>(regressionCount) * sumXX - sumX * sumX;
    snapshot.spectralTilt = std::abs(denom) > kEps
        ? (static_cast<float>(regressionCount) * sumXY - sumX * sumY) / denom
        : 0.0f;

    std::copy(rawMagnitude_.begin(), rawMagnitude_.end(), previousMagnitude_.begin());
    publishSnapshot(snapshot);
}

void RealtimeSpectrumAnalyzer::publishSnapshot(const SpectrumSnapshot& snapshot) noexcept
{
    const auto before = version_.load(std::memory_order_relaxed);
    version_.store(before + 1u, std::memory_order_release);
    publishedSnapshot_ = snapshot;
    version_.store(before + 2u, std::memory_order_release);
}

void RealtimeSpectrumAnalyzer::computeHannWindow() noexcept
{
    if (window_.empty())
        return;

    const float denom = static_cast<float>(std::max(1, fftSize_ - 1));
    for (int i = 0; i < fftSize_; ++i)
        window_[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / denom));
}

} // namespace more_phi
