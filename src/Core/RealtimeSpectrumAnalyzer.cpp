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
    // AUDIT-FIX (A7): EMA coeff for program crest: alpha = 1 - exp(-hop/tau).
    // framesPerTau = sampleRate / hopSize gives the analysis frame rate; tau is
    // kCrestProgramTauSeconds (1 s). Computed once here, used every processFrame().
    {
        const double frameRate = sampleRate_ / static_cast<double>(hopSize_ > 0 ? hopSize_ : kDefaultHopSize);
        const double framesPerTau = std::max(1.0, frameRate * kCrestProgramTauSeconds);
        crestProgramAlpha_ = static_cast<float>(1.0 - std::exp(-1.0 / framesPerTau));
    }
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
    // AUDIT (crest/RMS): re-zero so the program-crest EMA re-gates after reset.
    samplesCaptured_ = 0;
    crestProgramEma_ = 0.0f;   // AUDIT-FIX (A7): reset program-crest EMA

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
        // AUDIT (crest/RMS): count captured samples so processFrame can skip the
        // program-crest EMA until the ring holds a full frame of real audio.
        if (samplesCaptured_ < static_cast<uint64_t>(fftSize_))
            ++samplesCaptured_;

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
    // AUDIT-FIX (DC-1, R5a 2026-07-16): remove the frame's DC offset before the
    // crest/RMS/peak metrics. A constant-DC signal otherwise inflates low-bin
    // energy and, via the start-of-frame step transient from the ring buffer's
    // initial zero state, makes the per-frame crest read ~1.6 instead of ~1.0.
    // Subtracting the mean makes a pure-DC frame report ~0 peak/RMS (→ crest 0,
    // flagged invalid downstream) and leaves AC content (sine, music) unaffected
    // to first order. (LUFS already RLB-filters DC; this brings the spectral
    // path in line.)
    double frameSum = 0.0;
    for (int i = 0; i < fftSize_; ++i)
        frameSum += static_cast<double>(rawFrame_[static_cast<size_t>(i)]);
    const float frameMean = static_cast<float>(frameSum / static_cast<double>(std::max(1, fftSize_)));

    // AUDIT-FIX (M2): peak / RMS computed over the UN-WINDOWED, DC-removed raw
    // frame. The previous code used linearFrame_ (= raw * Hann), which biases
    // the reported peak down by up to -6 dB whenever the true peak does not sit
    // at the window centre, and skews RMS by the window's mean gain.
    float peak = 0.0f;
    double rmsSum = 0.0;

    for (int i = 0; i < fftSize_; ++i)
    {
        const float s = rawFrame_[static_cast<size_t>(i)] - frameMean;
        const float a = std::abs(s);
        if (a > peak) peak = a;
        rmsSum += static_cast<double>(s) * static_cast<double>(s);
    }

    int fluxCount = 0;
    for (int bin = 0; bin < numBins_; ++bin)
    {
        // Standard complex-interleaved format (all JUCE FFT engines):
        //   real[bin] at fftScratch_[2*bin]
        //   imag[bin] at fftScratch_[2*bin+1]
        //   (DC imag = 0, Nyquist imag = 0 for real input)
        //
        // AUDIT-FIX (2026-07): previous code read with IPP packed-CCS offsets
        //   (2*bin-1 / 2*bin) which shifted all bins by one index. The FFTFallback
        //   engine used on MSVC always produces standard interleaved regardless
        //   of the ignoreHalf flag, so the old read swapped real/imag and shifted
        //   the entire spectrum — only THD/crest-factor tests caught it because
        //   the centroid test's 80 Hz margin masked the ~12 Hz drift.
        const auto idx = static_cast<size_t>(2 * bin);
        float real = fftScratch_[idx];
        float imag = fftScratch_[idx + 1];

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

    // AUDIT-FIX (A7): program-level crest factor = EMA of the per-frame crest
    // over ~1 s. Uses the classic one-pole form y += alpha*(x-y). Seeded to the
    // first valid per-frame crest so it doesn't climb from 0 for a full tau.
    //
    // AUDIT (crest/RMS, 2026-06-25): do NOT seed the EMA until the ring holds a
    // full frame of real audio. The ring starts zero-filled; the first frame
    // therefore straddles a 0→signal step whose RMS is transiently low (~0.35
    // for a full-scale sine vs the steady 0.707), seeding the EMA to ~2.8 crest
    // instead of ~1.4. The EMA then needs a full tau to unlearn it. Gating on
    // samplesCaptured_ >= fftSize_ makes the first reported crest correct.
    if (snapshot.crestFactor > 0.0f
        && samplesCaptured_ >= static_cast<uint64_t>(fftSize_))
    {
        if (crestProgramEma_ <= 0.0f)
            crestProgramEma_ = snapshot.crestFactor;
        else
            crestProgramEma_ += crestProgramAlpha_ * (snapshot.crestFactor - crestProgramEma_);
    }
    snapshot.crestFactorProgram = crestProgramEma_;

    // AUDIT-FIX (A7): Total Harmonic Distortion, H2..H5 / fundamental.
    // Fundamental = bin of max magnitude in (0, Nyquist). rawMagnitude_ is already
    // window-corrected (divided by N*coherentGain above), so the ratio is unbiased
    // by the Hann coherent gain. Capped at 100% to avoid blowups on near-silent
    // frames where the fundamental ≈ eps.
    int fundamentalBin = 1;
    float fundamentalMag = 0.0f;
    const int nyquistBin = numBins_ - 1;
    for (int bin = 1; bin < nyquistBin; ++bin)
    {
        const float m = rawMagnitude_[static_cast<size_t>(bin)];
        if (m > fundamentalMag)
        {
            fundamentalMag = m;
            fundamentalBin = bin;
        }
    }
    if (fundamentalBin > 0 && fundamentalMag > 1e-6f)
    {
        // AUDIT-F1.4 (2026-06-27): parabolic peak interpolation of the
        // fundamental magnitude. When the true fundamental frequency falls
        // between two FFT bins, the bin-pick max underestimates the real peak
        // (spectral leakage spreads energy into the skirts), which biases the
        // THD ratio (sqrt(harmonicEnergy)/fundamental) LOW. Interpolating the
        // peak from the fundamental bin and its two neighbours recovers a
        // closer estimate of the true magnitude, so THD on off-bin
        // fundamentals is no longer systematically under-reported. The classic
        // quadratic-interpolator formula: d = 0.5*(aL-aR)/(aL-2*aC+aR);
        // peak = aC - 0.25*(aL-aR)*d.
        float fundamentalInterp = fundamentalMag;
        if (fundamentalBin > 1 && fundamentalBin < nyquistBin - 1)
        {
            const float aL = rawMagnitude_[static_cast<size_t>(fundamentalBin - 1)];
            const float aC = fundamentalMag;
            const float aR = rawMagnitude_[static_cast<size_t>(fundamentalBin + 1)];
            const float denom = (aL - 2.0f * aC + aR);
            if (std::abs(denom) > 1e-12f)
            {
                const float d = 0.5f * (aL - aR) / denom;
                fundamentalInterp = aC - 0.25f * (aL - aR) * d;
                // The interpolant can dip slightly below the bin value on noisy
                // frames; clamp to the bin max so we never divide by a smaller
                // number than the un-interpolated path (which would inflate THD).
                if (fundamentalInterp < fundamentalMag)
                    fundamentalInterp = fundamentalMag;
            }
        }
        float harmonicEnergy = 0.0f;
        for (int h = 2; h <= 5; ++h)
        {
            const int harmonicBin = fundamentalBin * h;
            if (harmonicBin < nyquistBin)
            {
                const float hm = rawMagnitude_[static_cast<size_t>(harmonicBin)];
                harmonicEnergy += hm * hm;
            }
        }
        const float ratio = std::sqrt(harmonicEnergy) / fundamentalInterp;
        snapshot.thdPercent = std::min(ratio, 1.0f) * 100.0f;
    }
    else
    {
        snapshot.thdPercent = 0.0f;
    }

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
