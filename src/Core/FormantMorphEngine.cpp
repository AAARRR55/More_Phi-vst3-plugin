/*
 * MorphSnap — Core/FormantMorphEngine.cpp
 *
 * Cepstral-liftering formant preservation engine implementation.
 *
 * The cepstral approach to spectral envelope extraction:
 *
 *   Given a speech/instrument signal x[n], the short-time log spectrum can be
 *   decomposed as:
 *
 *     log|X[k]| = log|E[k]| + log|P[k]|
 *
 *   where E[k] is the slowly-varying spectral envelope (formants, timbral
 *   shape) and P[k] is the fast-varying excitation (pitch harmonics, noise).
 *
 *   The IFFT of log|X[k]| gives the real cepstrum c[n]. Low quefrency (small n)
 *   coefficients correspond to E[k]; high quefrency coefficients to P[k].
 *
 *   The cepstral lifter applies a rectangular window to c[n] with cutoff at
 *   lifterOrder_ quefrency bins, then FFTs back to obtain a smooth log envelope.
 *
 *   This separates the formant envelope from pitch structure cleanly for
 *   lifterOrder values in the range [20, 60] at 44.1kHz with N=2048.
 *
 * Formant transplant:
 *   After extracting the source envelope (from plugin A's pre-morph signal
 *   via captureFormants()) and the current frame's envelope, the ratio:
 *
 *     G[k] = sourceEnv[k] / currentEnv[k]
 *
 *   is applied as a spectral shaping filter to the morphed spectrum. The
 *   preservationAmount_ parameter blends G[k] toward unity (no correction).
 *
 * Threading note:
 *   captureFormants() writes to ChannelState::sourceEnvelope. This is safe
 *   only if called from the audio thread (same thread as processBlock). The
 *   public API documents this constraint.
 */
#include "FormantMorphEngine.h"

#include <cmath>
#include <algorithm>
#include <cassert>

namespace morphsnap {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr float kFmTwoPi = 6.28318530717958647692f;
static constexpr float kFmEps   = 1e-18f;

// ─── Construction / destruction ───────────────────────────────────────────────

FormantMorphEngine::FormantMorphEngine()  = default;
FormantMorphEngine::~FormantMorphEngine() = default;

// ─── prepare() ───────────────────────────────────────────────────────────────

void FormantMorphEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    hopSize_    = fftSize_ / 4;
    numBins_    = fftSize_ / 2 + 1;

    const int order = static_cast<int>(std::round(std::log2(static_cast<float>(fftSize_))));
    fft_ = std::make_unique<juce::dsp::FFT>(order);

    window_.resize(static_cast<size_t>(fftSize_));
    computeHannWindow();

    for (auto& ch : channels_)
    {
        ch.inputBuffer.assign(static_cast<size_t>(fftSize_), 0.0f);
        ch.outputBuffer.assign(static_cast<size_t>(fftSize_ + hopSize_), 0.0f);
        ch.sourceEnvelope.assign(static_cast<size_t>(numBins_), 1.0f);
        ch.cepstrumBuffer.assign(static_cast<size_t>(fftSize_), 0.0f);
        ch.currentEnvelope.assign(static_cast<size_t>(numBins_), 1.0f);
        ch.fftScratch.assign(static_cast<size_t>(fftSize_ * 2), 0.0f);
        ch.ifftOut.assign(static_cast<size_t>(fftSize_), 0.0f);
        ch.specReal.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.specImag.assign(static_cast<size_t>(numBins_), 0.0f);

        ch.writePos          = 0;
        ch.hopCount          = 0;
        ch.hasSourceEnvelope = false;
    }
}

// ─── reset() ──────────────────────────────────────────────────────────────────

void FormantMorphEngine::reset() noexcept
{
    for (auto& ch : channels_)
    {
        std::fill(ch.inputBuffer.begin(),    ch.inputBuffer.end(),    0.0f);
        std::fill(ch.outputBuffer.begin(),   ch.outputBuffer.end(),   0.0f);
        std::fill(ch.sourceEnvelope.begin(), ch.sourceEnvelope.end(), 1.0f);
        std::fill(ch.cepstrumBuffer.begin(), ch.cepstrumBuffer.end(), 0.0f);
        std::fill(ch.currentEnvelope.begin(),ch.currentEnvelope.end(),1.0f);
        std::fill(ch.fftScratch.begin(),     ch.fftScratch.end(),     0.0f);
        std::fill(ch.ifftOut.begin(),        ch.ifftOut.end(),        0.0f);
        std::fill(ch.specReal.begin(),       ch.specReal.end(),       0.0f);
        std::fill(ch.specImag.begin(),       ch.specImag.end(),       0.0f);
        ch.writePos          = 0;
        ch.hopCount          = 0;
        ch.hasSourceEnvelope = false;
    }
}

// ─── setActive() / isActive() ─────────────────────────────────────────────────

void FormantMorphEngine::setActive(bool active) noexcept
{
    active_.store(active, std::memory_order_relaxed);
}

bool FormantMorphEngine::isActive() const noexcept
{
    return active_.load(std::memory_order_relaxed);
}

// ─── setPreservationAmount() / getPreservationAmount() ───────────────────────

void FormantMorphEngine::setPreservationAmount(float amount) noexcept
{
    const float clamped = std::max(0.0f, std::min(1.0f, amount));
    preservationAmount_.store(clamped, std::memory_order_relaxed);
}

float FormantMorphEngine::getPreservationAmount() const noexcept
{
    return preservationAmount_.load(std::memory_order_relaxed);
}

// ─── captureFormants() ────────────────────────────────────────────────────────

void FormantMorphEngine::captureFormants(const juce::AudioBuffer<float>& reference) noexcept
{
    if (numBins_ == 0 || fft_ == nullptr)
        return;

    const int numChannels = std::min(reference.getNumChannels(),
                                     static_cast<int>(kMaxChannels));

    for (int c = 0; c < numChannels; ++c)
    {
        auto& ch = channels_[static_cast<size_t>(c)];

        const float* src     = reference.getReadPointer(c);
        const int    nSamples = reference.getNumSamples();
        const int    copyLen  = std::min(nSamples, fftSize_);

        // Copy reference samples into a fresh analysis window
        // Use cepstrumBuffer as temporary storage (it's fftSize_ long)
        std::fill(ch.cepstrumBuffer.begin(), ch.cepstrumBuffer.end(), 0.0f);
        for (int n = 0; n < copyLen; ++n)
            ch.cepstrumBuffer[static_cast<size_t>(n)] = src[n];

        // Forward FFT to get spectrum
        forwardFFT(ch.cepstrumBuffer.data(),
                   ch.specReal.data(), ch.specImag.data(),
                   ch.fftScratch.data());

        // Extract formant envelope into sourceEnvelope
        extractFormantEnvelope(ch.specReal.data(), ch.specImag.data(),
                               ch.sourceEnvelope.data(),
                               ch.cepstrumBuffer.data(),
                               ch.fftScratch.data());

        ch.hasSourceEnvelope = true;
    }
}

// ─── processBlock() ───────────────────────────────────────────────────────────

void FormantMorphEngine::processBlock(juce::AudioBuffer<float>& buffer) noexcept
{
    if (!active_.load(std::memory_order_relaxed))
        return;

    const float amount = preservationAmount_.load(std::memory_order_relaxed);
    if (amount < kFmEps)
        return;

    if (numBins_ == 0 || fft_ == nullptr)
        return;

    const int numChannels = std::min(buffer.getNumChannels(),
                                     static_cast<int>(kMaxChannels));
    const int numSamples  = buffer.getNumSamples();

    for (int c = 0; c < numChannels; ++c)
    {
        auto& ch = channels_[static_cast<size_t>(c)];

        // If we have no captured source formant, use the first frame's envelope
        // as a self-reference (identity transplant that still provides smoothing).

        float* outPtr      = buffer.getWritePointer(c);
        const float* inPtr = buffer.getReadPointer(c);

        for (int i = 0; i < numSamples; ++i)
        {
            ch.inputBuffer[static_cast<size_t>(ch.writePos)] = inPtr[i];
            ch.writePos = (ch.writePos + 1) % fftSize_;
            ++ch.hopCount;

            if (ch.hopCount >= hopSize_)
            {
                ch.hopCount = 0;
                processFrame(ch, amount);
            }

            // Drain output
            outPtr[i] = ch.outputBuffer[static_cast<size_t>(i % (fftSize_ + hopSize_))];
        }

        // Shift output buffer left
        const int outBufLen = fftSize_ + hopSize_;
        const int shift     = std::min(numSamples, outBufLen);
        for (int k = 0; k < outBufLen - shift; ++k)
            ch.outputBuffer[static_cast<size_t>(k)] =
                ch.outputBuffer[static_cast<size_t>(k + shift)];
        for (int k = outBufLen - shift; k < outBufLen; ++k)
            ch.outputBuffer[static_cast<size_t>(k)] = 0.0f;
    }
}

// ─── processFrame() ───────────────────────────────────────────────────────────

void FormantMorphEngine::processFrame(ChannelState& ch, float amount) noexcept
{
    // 1. Forward FFT of current frame
    forwardFFT(ch.inputBuffer.data(),
               ch.specReal.data(), ch.specImag.data(),
               ch.fftScratch.data());

    // 2. Extract current frame's spectral envelope
    extractFormantEnvelope(ch.specReal.data(), ch.specImag.data(),
                           ch.currentEnvelope.data(),
                           ch.cepstrumBuffer.data(),
                           ch.fftScratch.data());

    // 3. If no source envelope captured yet, bootstrap with current frame's
    if (!ch.hasSourceEnvelope)
    {
        std::copy(ch.currentEnvelope.begin(), ch.currentEnvelope.end(),
                  ch.sourceEnvelope.begin());
        ch.hasSourceEnvelope = true;
    }

    // 4. Apply formant transplant to complex spectrum
    applyFormantEnvelope(ch.specReal.data(), ch.specImag.data(),
                         ch.sourceEnvelope.data(),
                         ch.currentEnvelope.data(),
                         amount);

    // 5. Inverse FFT back to time domain
    inverseFFT(ch.specReal.data(), ch.specImag.data(),
               ch.ifftOut.data(),
               ch.fftScratch.data());

    // 6. Overlap-add into output buffer
    for (int n = 0; n < fftSize_; ++n)
        ch.outputBuffer[static_cast<size_t>(n)] += ch.ifftOut[n];
}

// ─── computeHannWindow() ─────────────────────────────────────────────────────

void FormantMorphEngine::computeHannWindow() noexcept
{
    const float N = static_cast<float>(fftSize_);
    for (int n = 0; n < fftSize_; ++n)
    {
        window_[static_cast<size_t>(n)] =
            0.5f * (1.0f - std::cos(kFmTwoPi * static_cast<float>(n) / (N - 1.0f)));
    }
}

// ─── extractFormantEnvelope() ────────────────────────────────────────────────

void FormantMorphEngine::extractFormantEnvelope(const float* real, const float* imag,
                                                 float* envelope,
                                                 float* cepBuf,
                                                 float* scratch) noexcept
{
    // ── Step 1: log magnitude spectrum into scratch (real part of interleaved) ──
    // We reuse scratch[0..2*numBins-1] for log magnitude, then zero-pad to fftSize.
    for (int k = 0; k < numBins_; ++k)
    {
        const float mag = std::sqrt(real[k] * real[k] + imag[k] * imag[k]);
        scratch[static_cast<size_t>(k * 2)]     = std::log(mag + kFmEps);
        scratch[static_cast<size_t>(k * 2 + 1)] = 0.0f;
    }
    // Fill negative frequencies with conjugate (for real IFFT symmetry)
    for (int k = 1; k < numBins_ - 1; ++k)
    {
        const int mirrorIdx = fftSize_ - k;
        scratch[static_cast<size_t>(mirrorIdx * 2)]     = scratch[static_cast<size_t>(k * 2)];
        scratch[static_cast<size_t>(mirrorIdx * 2 + 1)] = 0.0f;
    }

    // ── Step 2: IFFT of log spectrum → real cepstrum in cepBuf ──────────────
    fft_->performRealOnlyInverseTransform(scratch);
    for (int n = 0; n < fftSize_; ++n)
        cepBuf[n] = scratch[static_cast<size_t>(n * 2)];

    // ── Step 3: Cepstral lifter (rectangular window, keep [0, lifterOrder_]) ─
    // Zero quefrency coefficients above lifterOrder_:
    //   cepBuf[0]            = DC (keep)
    //   cepBuf[1..lifter]    = slow envelope (keep)
    //   cepBuf[lifter+1..]   = pitch & noise (zero)
    for (int n = lifterOrder_ + 1; n < fftSize_ - lifterOrder_; ++n)
        cepBuf[n] = 0.0f;

    // ── Step 4: FFT of liftered cepstrum → smooth log envelope ───────────────
    for (int n = 0; n < fftSize_; ++n)
    {
        scratch[static_cast<size_t>(n * 2)]     = cepBuf[n];
        scratch[static_cast<size_t>(n * 2 + 1)] = 0.0f;
    }
    fft_->performRealOnlyForwardTransform(scratch, true);

    // ── Step 5: exp() → linear spectral envelope ─────────────────────────────
    for (int k = 0; k < numBins_; ++k)
    {
        // Real part of forward FFT gives us the log envelope (imaginary ≈ 0)
        envelope[k] = std::exp(scratch[static_cast<size_t>(k * 2)]);
    }
}

// ─── applyFormantEnvelope() ──────────────────────────────────────────────────

void FormantMorphEngine::applyFormantEnvelope(float* real, float* imag,
                                               const float* sourceEnv,
                                               const float* currentEnv,
                                               float amount) noexcept
{
    for (int k = 0; k < numBins_; ++k)
    {
        // Ratio of source to current envelope: G[k] = src / cur
        const float cur   = currentEnv[k] + kFmEps;
        const float ratio = sourceEnv[k] / cur;

        // Blend ratio toward 1.0 by preservation amount
        //   amount=1: full source envelope transplant
        //   amount=0: passthrough (ratio=1, no change)
        const float gain = 1.0f + amount * (ratio - 1.0f);

        real[k] *= gain;
        imag[k] *= gain;
    }
}

// ─── forwardFFT() ────────────────────────────────────────────────────────────

void FormantMorphEngine::forwardFFT(const float* input,
                                     float* real, float* imag,
                                     float* scratch) noexcept
{
    for (int n = 0; n < fftSize_; ++n)
    {
        const float w = window_[static_cast<size_t>(n)];
        scratch[static_cast<size_t>(n * 2)]     = input[n] * w;
        scratch[static_cast<size_t>(n * 2 + 1)] = 0.0f;
    }

    fft_->performRealOnlyForwardTransform(scratch, true);

    for (int k = 0; k < numBins_; ++k)
    {
        real[k] = scratch[static_cast<size_t>(k * 2)];
        imag[k] = scratch[static_cast<size_t>(k * 2 + 1)];
    }
}

// ─── inverseFFT() ─────────────────────────────────────────────────────────────

void FormantMorphEngine::inverseFFT(const float* real, const float* imag,
                                     float* output,
                                     float* scratch) noexcept
{
    const int N = fftSize_;

    for (int k = 0; k < numBins_; ++k)
    {
        scratch[static_cast<size_t>(k * 2)]     = real[k];
        scratch[static_cast<size_t>(k * 2 + 1)] = imag[k];
    }
    // Conjugate mirror for negative frequencies
    for (int k = 1; k < numBins_ - 1; ++k)
    {
        const int mirrorIdx = N - k;
        scratch[static_cast<size_t>(mirrorIdx * 2)]     =  real[k];
        scratch[static_cast<size_t>(mirrorIdx * 2 + 1)] = -imag[k];
    }

    fft_->performRealOnlyInverseTransform(scratch);

    const float scale = 2.0f;
    for (int n = 0; n < N; ++n)
    {
        const float w = window_[static_cast<size_t>(n)];
        output[n] = scratch[static_cast<size_t>(n * 2)] * w * scale;
    }
}

} // namespace morphsnap
