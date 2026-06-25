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
    rawFrame_.assign(static_cast<size_t>(fftSize_), 0.0f);
    fftScratch_.assign(static_cast<size_t>(fftSize_ * 2), 0.0f);
    rawMagnitude_.assign(static_cast<size_t>(numBins_), 0.0f);
    previousMagnitudeDb_.assign(static_cast<size_t>(numBins_), kMinDB);
    computeHannWindow();
    reset();
}

void RealtimeSpectrumAnalyzer::reset() noexcept
{
    std::fill(inputBuffer_.begin(), inputBuffer_.end(), 0.0f);
    std::fill(linearFrame_.begin(), linearFrame_.end(), 0.0f);
    std::fill(rawFrame_.begin(), rawFrame_.end(), 0.0f);
    std::fill(fftScratch_.begin(), fftScratch_.end(), 0.0f);
    std::fill(rawMagnitude_.begin(), rawMagnitude_.end(), 0.0f);
    std::fill(previousMagnitudeDb_.begin(), previousMagnitudeDb_.end(), kMinDB);

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
    // JUCE's performRealOnlyForwardTransform expects fftSize_ contiguous real
    // samples as input (not interleaved). The output is packed in JUCE's
    // real-only format: output[0]=DC, output[1]=real[1], output[2]=imag[1],
    // output[3]=real[2], output[4]=imag[2], ..., output[N-1]=Nyquist.
    // AUDIT-FIX (M2): also keep an UN-WINDOWED copy (rawFrame_) so peak/RMS/crest
    // are measured on the true signal, not the Hann-attenuated frame.
    for (int i = 0; i < fftSize_; ++i)
    {
        const int src = (writePos_ + i) % fftSize_;
        const float raw = inputBuffer_[static_cast<size_t>(src)];
        rawFrame_[static_cast<size_t>(i)] = raw;
        const float sample = raw * window_[static_cast<size_t>(i)];
        linearFrame_[static_cast<size_t>(i)] = sample;
        fftScratch_[static_cast<size_t>(i)] = sample;                // contiguous real input
    }

    fft_->performRealOnlyForwardTransform(fftScratch_.data(), true);

    SpectrumSnapshot snapshot;
    snapshot.binCount = kMaxBins;
    snapshot.sampleRate = sampleRate_;
    snapshot.fftSize = fftSize_;
    snapshot.frameIndex = ++frameIndex_;

    float energySum = 0.0f;
    float weightedFreqSum = 0.0f;
    float fluxSum = 0.0f;
    // AUDIT-FIX (M2): peak / RMS computed over the UN-WINDOWED raw frame. The
    // previous code used linearFrame_ (= raw * Hann), which biases the reported
    // peak down by up to -6 dB whenever the true peak does not sit at the
    // window centre, and skews RMS by the window's mean gain.
    float peak = 0.0f;
    double rmsSum = 0.0;

    for (int i = 0; i < fftSize_; ++i)
    {
        const float s = rawFrame_[static_cast<size_t>(i)];
        const float a = std::abs(s);
        if (a > peak) peak = a;
        rmsSum += static_cast<double>(s) * static_cast<double>(s);
    }

    int fluxCount = 0;
    for (int bin = 0; bin < numBins_; ++bin)
    {
        // JUCE's real-only FFT output format:
        //   output[0] = DC real    (imag always 0)
        //   output[1] = real[1], output[2] = imag[1]
        //   output[3] = real[2], output[4] = imag[2]
        //   ...
        //   output[2k-1] = real[k], output[2k] = imag[k]  for 1 <= k < N/2
        //   output[N-1] = Nyquist real  (imag always 0)
        float real, imag;
        if (bin == 0)
        {
            real = fftScratch_[0];
            imag = 0.0f;
        }
        else if (bin == numBins_ - 1)
        {
            real = fftScratch_[static_cast<size_t>(fftSize_ - 1)];
            imag = 0.0f;
        }
        else
        {
            real = fftScratch_[static_cast<size_t>(2 * bin - 1)];
            imag = fftScratch_[static_cast<size_t>(2 * bin)];
        }

        // AUDIT-FIX (A3): divide by (N * coherentGain) so a full-scale single
        // tone reports its true amplitude in magnitudeDB. Previously the /N-only
        // division left an uncorrected Hann coherent gain of 0.5, biasing every
        // absolute dB reading by ~-6 dB. Ratio-based features (centroid, rolloff,
        // flux, tilt) are invariant to this scale, so they are unaffected.
        const float mag = std::sqrt(real * real + imag * imag)
                        / (static_cast<float>(fftSize_) * windowCoherentGain_);
        const float energy = mag * mag;
        const float freq = static_cast<float>(bin) * static_cast<float>(sampleRate_) / static_cast<float>(fftSize_);
        rawMagnitude_[static_cast<size_t>(bin)] = mag;
        energySum += energy;
        weightedFreqSum += freq * energy;

        // AUDIT-FIX (H1): spectral flux in the dB domain, one-sided (onset-only).
        // The previous linear-magnitude form scaled 1/N with FFT size and was
        // level-dependent — a louder signal reported higher flux for an identical
        // spectral-change rate. dB-domain flux is level-invariant and matches the
        // onset-detection convention. Only positive deltas (rising energy) count.
        const float magDb = amplitudeToDB(mag);
        const float deltaDb = magDb - previousMagnitudeDb_[static_cast<size_t>(bin)];
        if (deltaDb > 0.0f)
        {
            fluxSum += deltaDb * deltaDb;
            ++fluxCount;
        }
    }

    snapshot.spectralCentroid = energySum > kEps ? weightedFreqSum / energySum : 0.0f;
    // RMS-of-the-one-sided-dB-deltas: divide by the number of bins that actually
    // contributed a positive delta (not by numBins_, which would dilute it by the
    // many stationary bins). Yields a meaningful, level-invariant flux magnitude.
    snapshot.spectralFlux = fluxCount > 0
        ? std::sqrt(fluxSum / static_cast<float>(fluxCount))
        : 0.0f;

    const double rms = std::sqrt(rmsSum / static_cast<double>(std::max(1, fftSize_)));
    snapshot.crestFactor = rms > kEps ? peak / static_cast<float>(rms) : 0.0f;

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

    // AUDIT-FIX (H1): persist the dB-domain magnitudes for next frame's flux
    // delta. The previous code stored linear magnitudes and differenced those,
    // which coupled flux to signal level; now both sides of the delta are in dB.
    for (int bin = 0; bin < numBins_; ++bin)
        previousMagnitudeDb_[static_cast<size_t>(bin)] =
            amplitudeToDB(rawMagnitude_[static_cast<size_t>(bin)]);
    publishSnapshot(snapshot);
}

void RealtimeSpectrumAnalyzer::publishSnapshot(const SpectrumSnapshot& snapshot) noexcept
{
    // AUDIT-FIX (A4): mirror the C2-FIX pattern from StereoFieldAnalyzer. A plain
    // release store does NOT prevent the snapshot copy from being hoisted above
    // the odd-marker store on weakly-ordered CPUs (ARM/Apple Silicon), which
    // lets a reader observe a torn snapshot through an even version. The
    // explicit thread_fence(release) guarantees the publisher's prior writes are
    // visible before the version flips odd->even.
    const auto before = version_.load(std::memory_order_relaxed);
    version_.store(before + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    publishedSnapshot_ = snapshot;
    std::atomic_thread_fence(std::memory_order_release);
    version_.store(before + 2u, std::memory_order_relaxed);
}

void RealtimeSpectrumAnalyzer::computeHannWindow() noexcept
{
    if (window_.empty())
        return;

    const float denom = static_cast<float>(std::max(1, fftSize_ - 1));
    double sumW = 0.0;
    double sumWSq = 0.0;
    for (int i = 0; i < fftSize_; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / denom));
        window_[static_cast<size_t>(i)] = w;
        sumW += w;
        sumWSq += static_cast<double>(w) * w;
    }
    // AUDIT-FIX (A3): coherent gain = Σw/N, energy gain = Σw²/N. Accumulate in
    // double for precision; the Hann theoretical values are 0.5 and 0.375.
    const double n = static_cast<double>(fftSize_);
    windowCoherentGain_ = (n > 0.0) ? static_cast<float>(sumW / n) : 1.0f;
    windowEnergyGain_   = (n > 0.0) ? static_cast<float>(sumWSq / n) : 1.0f;
    if (windowCoherentGain_ < 1e-6f) windowCoherentGain_ = 1.0f;  // guard against degenerate window
}

} // namespace more_phi
