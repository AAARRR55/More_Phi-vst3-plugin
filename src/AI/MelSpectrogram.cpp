/*
 * More-Phi — AI/MelSpectrogram.cpp
 */
#include "AI/MelSpectrogram.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {

// Slaney-style mel<->hz. hzToMel = 2595*log10(1 + hz/700).
double hzToMel(double hz) noexcept
{
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}
double melToHz(double mel) noexcept
{
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

} // namespace

MelSpectrogram::MelSpectrogram() = default;

void MelSpectrogram::prepare(double sampleRate, std::size_t inputSamples) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    inputSamples_ = inputSamples;

    // FFT order 11 → size 2048 (matches RealtimeSpectrumAnalyzer).
    if (!fft_ || fft_->getSize() != kFftSize)
        fft_ = std::make_unique<juce::dsp::FFT>(11);

    numBins_ = kFftSize / 2 + 1;

    window_.assign(static_cast<std::size_t>(kFftSize), 0.0f);
    frame_.assign(static_cast<std::size_t>(kFftSize), 0.0f);
    // performRealOnlyForwardTransform needs 2*N scratch (it writes complex out).
    fftScratch_.assign(static_cast<std::size_t>(2 * kFftSize), 0.0f);
    powerSpectrum_.assign(static_cast<std::size_t>(numBins_), 0.0f);

    computeHannWindow();
    computeMelFilterbank(sampleRate_);

    // Frame count from the standard STFT framing: hop by kHopSize, first frame
    // at sample 0. Frames fully inside the buffer; the final partial frame is
    // zero-padded by the caller contract (process zero-fills frame_ each call).
    if (inputSamples_ >= static_cast<std::size_t>(kFftSize))
        frameCount_ = 1u + (inputSamples_ - static_cast<std::size_t>(kFftSize)) / static_cast<std::size_t>(kHopSize);
    else
        frameCount_ = 0u;

    melOutput_.assign(kMelBins * frameCount_, 0.0f);
    reset();
}

void MelSpectrogram::reset() noexcept
{
    std::fill(melOutput_.begin(), melOutput_.end(), 0.0f);
    std::fill(frame_.begin(), frame_.end(), 0.0f);
}

void MelSpectrogram::computeHannWindow() noexcept
{
    const double n = static_cast<double>(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / n));
}

void MelSpectrogram::computeMelFilterbank(double sampleRate) noexcept
{
    melFilter_.assign(static_cast<std::size_t>(kMelBins) * static_cast<std::size_t>(numBins_), 0.0f);
    const double nyquist = sampleRate * 0.5;
    const double melLow = hzToMel(kMinFreqHz);
    const double melHigh = hzToMel(nyquist);

    // kMelBins + 2 evenly-spaced mel points → center frequencies for each of
    // the kMelBins triangle filters, plus the two outer edges.
    const int numPoints = kMelBins + 2;
    std::vector<double> melPoints(static_cast<std::size_t>(numPoints));
    std::vector<double> hzPoints(static_cast<std::size_t>(numPoints));
    std::vector<int>    binPoints(static_cast<std::size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i)
    {
        const double mel = melLow + (melHigh - melLow) * static_cast<double>(i) / static_cast<double>(kMelBins + 1);
        const double hz  = melToHz(mel);
        melPoints[static_cast<std::size_t>(i)] = mel;
        hzPoints[static_cast<std::size_t>(i)]  = hz;
        // FFT bin for this frequency: hz / hzPerBin.
        const double bin = hz * static_cast<double>(kFftSize) / sampleRate;
        binPoints[static_cast<std::size_t>(i)] = static_cast<int>(std::round(bin));
    }

    // Triangle filter for mel bin m (1..kMelBins): rises from binPoints[m-1] to
    // binPoints[m], falls to binPoints[m+1]. Stored dense; zeros outside.
    for (int m = 1; m <= kMelBins; ++m)
    {
        const int left   = binPoints[static_cast<std::size_t>(m - 1)];
        const int center = binPoints[static_cast<std::size_t>(m)];
        const int right  = binPoints[static_cast<std::size_t>(m + 1)];
        const double denomUp   = std::max(1, center - left);
        const double denomDown = std::max(1, right - center);
        float* row = melFilter_.data() + static_cast<std::size_t>(m - 1) * static_cast<std::size_t>(numBins_);
        for (int b = left; b <= right && b < numBins_; ++b)
        {
            if (b < 0) continue;
            double w = 0.0;
            if (b <= center)
                w = static_cast<double>(b - left) / denomUp;
            else
                w = static_cast<double>(right - b) / denomDown;
            if (w < 0.0) w = 0.0;
            if (w > 1.0) w = 1.0;
            row[static_cast<std::size_t>(b)] = static_cast<float>(w);
        }
    }
}

void MelSpectrogram::process(const float* mono, std::size_t numSamples) noexcept
{
    if (frameCount_ == 0 || mono == nullptr) return;
    const std::size_t usable = std::min(numSamples, inputSamples_);

    for (std::size_t f = 0; f < frameCount_; ++f)
    {
        const std::size_t start = f * static_cast<std::size_t>(kHopSize);

        // Window the frame (zero-padded if the buffer runs short).
        for (int i = 0; i < kFftSize; ++i)
        {
            const std::size_t idx = start + static_cast<std::size_t>(i);
            const float s = (idx < usable) ? mono[idx] : 0.0f;
            frame_[static_cast<std::size_t>(i)] = s * window_[static_cast<std::size_t>(i)];
        }

        // In-place real→complex FFT. performRealOnlyForwardTransform(data, true)
        // produces standard interleaved [re0, im0, re1, im1, ...] on MSVC's
        // FFTFallback engine (the layout RealtimeSpectrumAnalyzer reads — see
        // its AUDIT-FIX-2026-07 comment). We window the frame into the first
        // kFftSize slots; the FFT writes complex output over the full 2*kFftSize.
        std::copy_n(frame_.data(), kFftSize, fftScratch_.data());
        for (int i = kFftSize; i < 2 * kFftSize; ++i)
            fftScratch_[static_cast<std::size_t>(i)] = 0.0f;
        fft_->performRealOnlyForwardTransform(fftScratch_.data(), /*negativeFrequencyMirror=*/true);

        // Power spectrum |X(k)|^2 for k = 0..N/2, interleaved read (matches
        // RealtimeSpectrumAnalyzer's corrected bin access). DC and Nyquist have
        // zero imaginary part for real input, so re^2 alone suffices there, but
        // the uniform re^2+im^2 read is correct for all bins and avoids special cases.
        for (int k = 0; k < numBins_; ++k)
        {
            const float re = fftScratch_[static_cast<std::size_t>(2 * k)];
            const float im = fftScratch_[static_cast<std::size_t>(2 * k + 1)];
            powerSpectrum_[static_cast<std::size_t>(k)] = re * re + im * im;
        }

        // Mel filterbank matrix-vector product → log + normalize.
        float* melOut = melOutput_.data() + f;  // column f, rows strided by frameCount_
        const float* filt = melFilter_.data();
        const std::size_t stride = frameCount_;
        for (int m = 0; m < kMelBins; ++m)
        {
            const float* row = filt + static_cast<std::size_t>(m) * static_cast<std::size_t>(numBins_);
            double acc = 0.0;
            for (int b = 0; b < numBins_; ++b)
                acc += static_cast<double>(row[b]) * static_cast<double>(powerSpectrum_[static_cast<std::size_t>(b)]);
            const double db = 10.0 * std::log10(acc + kRefDbFloor);
            melOut[m * stride] = static_cast<float>(db);
        }
    }
}

} // namespace more_phi
