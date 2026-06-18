/*
 * More-Phi — Core/SpectralMorphEngine.cpp
 *
 * Full STFT-based phase-vocoder spectral morph implementation.
 *
 * Overlap-add architecture (per channel, per processBlock call):
 *
 *   Input is written into a circular ring buffer of length fftSize_.
 *   Every hopSize_ input samples, processFrame() is invoked:
 *     1. De-rotate the ring buffer into a linear analysis window.
 *     2. Apply Hann window to both A and B analysis windows.
 *     3. Forward FFT → complex spectra for A and B.
 *     4. Compute magnitude and phase for each stream.
 *     5. Transient detection on ch 0 (adjusts alpha).
 *     6. Geometric-mean magnitude interpolation.
 *     7. Instantaneous-frequency phase interpolation.
 *     8. Reconstruct complex spectrum: real[k] = mag*cos(phi), imag[k] = mag*sin(phi).
 *     9. Inverse FFT → windowed time-domain frame.
 *    10. Overlap-add the fftSize_ output samples into outputBuffer_.
 *
 *   processBlock() drains exactly numSamples from the front of outputBuffer_
 *   into the output pointer. The output buffer is kept as a flat array of
 *   (fftSize_ + maxBlockSize_) samples. After draining, the buffer is shifted
 *   left by numSamples. This avoids modular arithmetic in the hot path.
 *
 * Hann window OLA normalisation:
 *   Analysis:  w_a[n] = Hann
 *   Synthesis: w_s[n] = Hann
 *   Combined:  w_a[n]^2 accumulated with 75% overlap sums to N/2.
 *   JUCE's performRealOnlyInverseTransform normalises by 1/N.
 *   Net factor = (N/2) * (1/N) = 0.5  → multiply by 2 to restore unity.
 *
 * De-rotation of circular buffer (writePos):
 *   After writing hopSize_ samples, writePos has advanced. The oldest sample
 *   in the analysis window is at writePos (mod fftSize_). We copy the ring
 *   buffer in two memcpy chunks into a temporary linearisation buffer (one of
 *   the pre-allocated fftScratch arrays — first half used for linearisation,
 *   second half for JUCE interleaved complex format).
 */
#include "SpectralMorphEngine.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cassert>

namespace more_phi {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr float kSMTwoPi = 6.28318530717958647692f;
static constexpr float kSMPi    = 3.14159265358979323846f;
static constexpr float kSMEps   = 1e-6f;

// ─── Construction / destruction ───────────────────────────────────────────────

SpectralMorphEngine::SpectralMorphEngine()  = default;
SpectralMorphEngine::~SpectralMorphEngine() = default;

// ─── prepare() ───────────────────────────────────────────────────────────────

void SpectralMorphEngine::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;

    // Validate pending FFT size (set before prepare via setFFTSize)
    fftSize_ = pendingFFTSize_.load(std::memory_order_relaxed);

    // Clamp to supported values if state was corrupted
    const int validSizes[] = { 512, 1024, 2048, 4096 };
    bool valid = false;
    for (int s : validSizes)
        if (fftSize_ == s) { valid = true; break; }
    if (!valid)
    {
        fftSize_ = 2048;
        pendingFFTSize_.store(fftSize_, std::memory_order_relaxed);
    }

    hopSize_        = fftSize_ / 4;
    numBins_        = fftSize_ / 2 + 1;
    // Latency = fftSize_ + hopSize_ samples.
    // The theoretical minimum overlap-add latency for a hop-size-H vocoder is H
    // samples (one hop of look-ahead). We report fftSize_ + hopSize_ because the
    // output buffer must be pre-filled with one full FFT frame (fftSize_ samples)
    // before useful output is available, plus one hop of ring-buffer fill time
    // (hopSize_) before the first processFrame() fires. This correctly compensates
    // the DAW's PDC (Plugin Delay Compensation) so the morphed signal aligns with
    // the dry signal in the session timeline.
    latencySamples_ = fftSize_ + hopSize_;

    // JUCE FFT: fftSize = 2^order
    const int order = static_cast<int>(std::round(std::log2(static_cast<float>(fftSize_))));
    fft_ = std::make_unique<juce::dsp::FFT>(order);

    // Hann window
    window_.resize(static_cast<size_t>(fftSize_));
    computeHannWindow();

    // Per-channel buffers
    for (auto& ch : channels_)
    {
        ch.inputBufferA.assign(static_cast<size_t>(fftSize_), 0.0f);
        ch.inputBufferB.assign(static_cast<size_t>(fftSize_), 0.0f);

        // Output buffer: fftSize + enough room for one extra block without re-shift.
        // FIX 2.3: Must also hold every frame written within a single block at
        // increasing offsets (up to ceil(maxBlockSize/hopSize)+1 frames × hopSize_)
        // plus one full fftSize_ tail, so multi-hop blocks never overflow.
        const int maxHopsPerBlock = (maxBlockSize / hopSize_) + 2;
        const int outLen = fftSize_ + std::max(hopSize_, maxBlockSize) + maxHopsPerBlock * hopSize_;
        ch.outputBuffer.assign(static_cast<size_t>(outLen), 0.0f);

        ch.fftRealA.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.fftImagA.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.fftRealB.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.fftImagB.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.fftRealOut.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.fftImagOut.assign(static_cast<size_t>(numBins_), 0.0f);

        ch.prevPhaseA.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.prevPhaseB.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.synthPhase.assign(static_cast<size_t>(numBins_), 0.0f);

        ch.magA.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.magB.assign(static_cast<size_t>(numBins_), 0.0f);
        ch.magOut.assign(static_cast<size_t>(numBins_), 0.0f);

        // fftScratch layout:
        //   [0 .. fftSize-1]         : linearisation temp (de-rotated time domain)
        //   [fftSize .. 3*fftSize-1] : JUCE interleaved complex (fftSize * 2 floats)
        ch.fftScratch.assign(static_cast<size_t>(fftSize_ * 3), 0.0f);

        ch.writePos = 0;
        ch.hopCount = 0;
        ch.outWritePos = 0;   // FIX 2.3: reset overlap-add write head
    }

    // Size blockAlphas_ to support the maximum number of hops in a block.
    // Minimum 128 elements to prevent reallocation in the audio path.
    const int maxHops = std::max(128, (maxBlockSize / hopSize_) + 2);
    blockAlphas_.assign(static_cast<size_t>(maxHops), 0.0f);

    for (auto& det : transientDetectors_)
        det.prepare(static_cast<float>(sampleRate_), hopSize_);
}

// ─── setFFTSize() ─────────────────────────────────────────────────────────────

void SpectralMorphEngine::setFFTSize(int size)
{
    const int validSizes[] = { 512, 1024, 2048, 4096 };
    for (int s : validSizes)
        if (size == s) { pendingFFTSize_.store(size, std::memory_order_relaxed); return; }
    // Silently ignore invalid values
}

// ─── Setters ──────────────────────────────────────────────────────────────────

void SpectralMorphEngine::setTransientPreserve(bool enabled) noexcept
{
    transientPreserve_.store(enabled, std::memory_order_relaxed);
}

void SpectralMorphEngine::setFormantPreserve(bool enabled) noexcept
{
    formantPreserve_.store(enabled, std::memory_order_relaxed);
}

void SpectralMorphEngine::setActive(bool active) noexcept
{
    active_.store(active, std::memory_order_relaxed);
}

// ─── reset() ──────────────────────────────────────────────────────────────────

void SpectralMorphEngine::reset() noexcept
{
    for (auto& ch : channels_)
    {
        std::fill(ch.inputBufferA.begin(),  ch.inputBufferA.end(),  0.0f);
        std::fill(ch.inputBufferB.begin(),  ch.inputBufferB.end(),  0.0f);
        std::fill(ch.outputBuffer.begin(),  ch.outputBuffer.end(),  0.0f);
        std::fill(ch.prevPhaseA.begin(),    ch.prevPhaseA.end(),    0.0f);
        std::fill(ch.prevPhaseB.begin(),    ch.prevPhaseB.end(),    0.0f);
        std::fill(ch.synthPhase.begin(),    ch.synthPhase.end(),    0.0f);
        std::fill(ch.fftScratch.begin(),    ch.fftScratch.end(),    0.0f);
        ch.writePos = 0;
        ch.hopCount = 0;
        ch.outWritePos = 0;   // FIX 2.3: reset overlap-add write head
    }
    for (auto& det : transientDetectors_)
        det.reset();
}

// ─── Queries ──────────────────────────────────────────────────────────────────

int SpectralMorphEngine::getLatencyInSamples() const noexcept
{
    return latencySamples_;
}

bool SpectralMorphEngine::isActive() const noexcept
{
    return active_.load(std::memory_order_relaxed);
}

// ─── processBlock() ───────────────────────────────────────────────────────────

void SpectralMorphEngine::processBlock(juce::AudioBuffer<float>& bufA,
                                       const juce::AudioBuffer<float>& bufB,
                                       float alpha) noexcept
{
    if (!active_.load(std::memory_order_relaxed))
        return;

    if (numBins_ == 0 || fft_ == nullptr)
        return;

    alpha = std::max(0.0f, std::min(1.0f, alpha));

    const int numChannels = std::min(bufA.getNumChannels(),
                                     static_cast<int>(kMaxChannels));
    const int numSamples  = bufA.getNumSamples();

    if (numSamples <= 0)
        return;

    for (int c = 0; c < numChannels; ++c)
    {
        ChannelState& ch = channels_[static_cast<size_t>(c)];

        // Plugin B channel mirroring
        const int chB    = (bufB.getNumChannels() > 0)
                           ? std::min(c, bufB.getNumChannels() - 1) : -1;
        const float* inA = bufA.getReadPointer(c);
        const float* inB = (chB >= 0) ? bufB.getReadPointer(chB) : nullptr;

        int hopIndex = 0;

        // ── Feed input into circular ring buffers and trigger frames ──────────
        for (int i = 0; i < numSamples; ++i)
        {
            ch.inputBufferA[static_cast<size_t>(ch.writePos)] = inA[i];
            ch.inputBufferB[static_cast<size_t>(ch.writePos)] =
                (inB != nullptr) ? inB[i] : 0.0f;

            ch.writePos = (ch.writePos + 1) % fftSize_;
            ++ch.hopCount;

            if (ch.hopCount >= hopSize_)
            {
                ch.hopCount = 0;

                float currentAlpha = alpha;
                if (transientPreserve_.load(std::memory_order_relaxed))
                {
                    if (c == 0)
                    {
                        // Transient detection uses previous frame's magA (stale by one
                        // hop, which is acceptable — detection latency = one hop = ~12ms).
                        currentAlpha = transientDetectors_[0].process(
                            ch.magA.data(), numBins_, alpha);
                        
                        // Store the alpha for subsequent channels
                        if (hopIndex < static_cast<int>(blockAlphas_.size()))
                            blockAlphas_[static_cast<size_t>(hopIndex)] = currentAlpha;
                    }
                    else
                    {
                        // Reuse the stored alpha from channel 0 for perfect stereo coherence
                        if (hopIndex < static_cast<int>(blockAlphas_.size()))
                            currentAlpha = blockAlphas_[static_cast<size_t>(hopIndex)];
                    }
                }

                processFrame(ch, currentAlpha);
                ++hopIndex;
            }
        }

        // ── Drain numSamples from front of output buffer into bufA ────────────
        float* out = bufA.getWritePointer(c);
        const int outBufLen = static_cast<int>(ch.outputBuffer.size());
        const int drainLen  = std::min(numSamples, outBufLen);

        for (int i = 0; i < drainLen; ++i)
            out[i] = ch.outputBuffer[static_cast<size_t>(i)];
        // Zero-pad if drain window exceeds output buffer (shouldn't happen)
        for (int i = drainLen; i < numSamples; ++i)
            out[i] = 0.0f;

        // Shift output buffer left by numSamples
        const int remaining = outBufLen - drainLen;
        if (remaining > 0)
        {
            std::memmove(ch.outputBuffer.data(),
                         ch.outputBuffer.data() + drainLen,
                         static_cast<size_t>(remaining) * sizeof(float));
        }
        std::fill(ch.outputBuffer.begin() + remaining,
                  ch.outputBuffer.end(), 0.0f);

        // FIX 2.3: Advance the overlap-add write head with the drain so the
        // next block's first frame resumes at the correct absolute offset.
        // Clamp at 0 — underflow would mean we drained more than was written,
        // which can only happen on the priming blocks before steady state.
        ch.outWritePos -= drainLen;
        if (ch.outWritePos < 0) ch.outWritePos = 0;
    }
}

// ─── processFrame() ───────────────────────────────────────────────────────────

void SpectralMorphEngine::processFrame(ChannelState& ch, float alpha) noexcept
{
    // fftScratch layout:
    //   [0         .. fftSize-1]     : linearisation temp
    //   [fftSize   .. 3*fftSize-1]   : JUCE interleaved complex scratch (fftSize*2)
    float* linBuf  = ch.fftScratch.data();                     // length = fftSize_
    float* scratch = ch.fftScratch.data() + fftSize_;          // length = fftSize_ * 2

    // ── 1. De-rotate ring buffer A into linear frame, then forward FFT ────────
    //
    // The oldest sample is at writePos; the ring buffer wraps at fftSize_.
    // We copy in two segments: [writePos .. fftSize-1] then [0 .. writePos-1].
    {
        const int tail = fftSize_ - ch.writePos;  // samples from writePos to end
        const int head = ch.writePos;             // samples from 0 to writePos-1

        std::memcpy(linBuf,
                    ch.inputBufferA.data() + ch.writePos,
                    static_cast<size_t>(tail) * sizeof(float));
        if (head > 0)
            std::memcpy(linBuf + tail,
                        ch.inputBufferA.data(),
                        static_cast<size_t>(head) * sizeof(float));
    }
    forwardFFT(linBuf, ch.fftRealA.data(), ch.fftImagA.data(), scratch);

    // ── 2. De-rotate ring buffer B and forward FFT ────────────────────────────
    {
        const int tail = fftSize_ - ch.writePos;
        const int head = ch.writePos;

        std::memcpy(linBuf,
                    ch.inputBufferB.data() + ch.writePos,
                    static_cast<size_t>(tail) * sizeof(float));
        if (head > 0)
            std::memcpy(linBuf + tail,
                        ch.inputBufferB.data(),
                        static_cast<size_t>(head) * sizeof(float));
    }
    forwardFFT(linBuf, ch.fftRealB.data(), ch.fftImagB.data(), scratch);

    // ── 3. Magnitude + phase ──────────────────────────────────────────────────
    //   Use fftRealOut / fftImagOut as temporary phase storage for this frame.
    float* phaseA = ch.fftRealOut.data();
    float* phaseB = ch.fftImagOut.data();

    computeMagnitudePhase(ch.fftRealA.data(), ch.fftImagA.data(),
                          ch.magA.data(), phaseA, numBins_);
    computeMagnitudePhase(ch.fftRealB.data(), ch.fftImagB.data(),
                          ch.magB.data(), phaseB, numBins_);

    // ── 4. Interpolate magnitude (log-magnitude geometric mean) ──────────────
    interpolateMagnitude(ch.magA.data(), ch.magB.data(),
                         ch.magOut.data(), numBins_, alpha);

    // ── 5. Interpolate phase (IF vocoder) ─────────────────────────────────────
    interpolatePhase(phaseA, phaseB,
                     ch.prevPhaseA.data(), ch.prevPhaseB.data(),
                     ch.synthPhase.data(),
                     numBins_, alpha);

    // ── 6. Reconstruct complex spectrum from interpolated mag + synth phase ───
    for (int k = 0; k < numBins_; ++k)
    {
        const float mag = ch.magOut[static_cast<size_t>(k)];
        const float phi = ch.synthPhase[static_cast<size_t>(k)];
        ch.fftRealOut[static_cast<size_t>(k)] = mag * std::cos(phi);
        ch.fftImagOut[static_cast<size_t>(k)] = mag * std::sin(phi);
    }

    // ── 7. Inverse FFT → windowed time-domain frame in linBuf ────────────────
    inverseFFT(ch.fftRealOut.data(), ch.fftImagOut.data(), linBuf, scratch);

    // ── 8. Overlap-add into output buffer ─────────────────────────────────────
    //   FIX 2.3: Each frame is added at the channel's overlap-add write head,
    //   which advances by hopSize_ per frame. This is what gives correct 75%
    //   overlap reconstruction: consecutive windowed frames must be offset by
    //   exactly hopSize_ so the four overlapping Hann² envelopes sum to the
    //   COLA constant. Adding every frame at offset 0 (the old behavior) broke
    //   reconstruction whenever more than one hop fired in a single block.
    const int outBufLen = static_cast<int>(ch.outputBuffer.size());
    const int addLen    = std::min(fftSize_, outBufLen - ch.outWritePos);
    for (int n = 0; n < addLen; ++n)
        ch.outputBuffer[static_cast<size_t>(ch.outWritePos + n)] += linBuf[n];
    ch.outWritePos += hopSize_;
}

// ─── computeHannWindow() ─────────────────────────────────────────────────────

void SpectralMorphEngine::computeHannWindow() noexcept
{
    // Periodic Hann: w[n] = 0.5 * (1 - cos(2π*n / N))
    // Periodic form (N denominator, not N-1) gives perfect reconstruction
    // with 75% overlap (hop = N/4).
    const float N = static_cast<float>(fftSize_);
    for (int n = 0; n < fftSize_; ++n)
    {
        window_[static_cast<size_t>(n)] =
            0.5f * (1.0f - std::cos(kSMTwoPi * static_cast<float>(n) / N));
    }
}

// ─── forwardFFT() ────────────────────────────────────────────────────────────
//
// input:   linearly ordered (already de-rotated) samples, length = fftSize_
// real:    output real part, length = numBins_
// imag:    output imag part, length = numBins_
// scratch: JUCE interleaved complex buffer, length = fftSize_ * 2

void SpectralMorphEngine::forwardFFT(const float* input,
                                     float* real, float* imag,
                                     float* scratch) noexcept
{
    // Build interleaved windowed input for JUCE FFT
    for (int n = 0; n < fftSize_; ++n)
    {
        const float w = window_[static_cast<size_t>(n)];
        scratch[static_cast<size_t>(n * 2)]     = input[n] * w;
        scratch[static_cast<size_t>(n * 2 + 1)] = 0.0f;
    }

    fft_->performRealOnlyForwardTransform(scratch, true);

    // Deinterleave positive frequencies
    for (int k = 0; k < numBins_; ++k)
    {
        real[static_cast<size_t>(k)] = scratch[static_cast<size_t>(k * 2)];
        imag[static_cast<size_t>(k)] = scratch[static_cast<size_t>(k * 2 + 1)];
    }
}

// ─── inverseFFT() ─────────────────────────────────────────────────────────────
//
// real:   input real part, length = numBins_
// imag:   input imag part, length = numBins_
// output: windowed time-domain result, length = fftSize_
// scratch: JUCE interleaved complex buffer, length = fftSize_ * 2

void SpectralMorphEngine::inverseFFT(const float* real, const float* imag,
                                     float* output,
                                     float* scratch) noexcept
{
    const int N = fftSize_;

    // Fill positive frequencies
    for (int k = 0; k < numBins_; ++k)
    {
        scratch[static_cast<size_t>(k * 2)]     = real[k];
        scratch[static_cast<size_t>(k * 2 + 1)] = imag[k];
    }

    // Conjugate mirror for negative frequencies (required by JUCE's real IFFT)
    for (int k = 1; k < numBins_ - 1; ++k)
    {
        const int m = N - k;
        scratch[static_cast<size_t>(m * 2)]     =  real[k];
        scratch[static_cast<size_t>(m * 2 + 1)] = -imag[k];
    }

    fft_->performRealOnlyInverseTransform(scratch);

    // Extract real part, apply synthesis Hann window and OLA gain correction.
    // JUCE's IFFT normalises by 1/N. With periodic Hann at 75% overlap
    // (hop = N/4), the overlap-add of the analysis × synthesis window pairs
    // sums to N/2 per sample. So the net OLA gain = (N/2) * (1/N) = 0.5.
    // Multiply by 2 to restore unity.
    static constexpr float kScale = 2.0f;
    for (int n = 0; n < N; ++n)
    {
        const float w = window_[static_cast<size_t>(n)];
        output[static_cast<size_t>(n)] =
            scratch[static_cast<size_t>(n * 2)] * w * kScale;
    }
}

// ─── computeMagnitudePhase() ─────────────────────────────────────────────────

void SpectralMorphEngine::computeMagnitudePhase(const float* real, const float* imag,
                                                 float* mag, float* phase,
                                                 int numBins) noexcept
{
    for (int k = 0; k < numBins; ++k)
    {
        const float r = real[k];
        const float i = imag[k];
        mag[k]   = std::sqrt(r * r + i * i);
        phase[k] = std::atan2(i, r);
    }
}

// ─── interpolateMagnitude() ───────────────────────────────────────────────────

void SpectralMorphEngine::interpolateMagnitude(const float* magA, const float* magB,
                                                float* magOut,
                                                int numBins, float alpha) noexcept
{
    // Geometric mean in log domain:
    //   magOut[k] = exp((1-α)*log(magA[k]) + α*log(magB[k]))
    const float oneMinusAlpha = 1.0f - alpha;
    for (int k = 0; k < numBins; ++k)
    {
        const float logA = std::log(magA[k] + kSMEps);
        const float logB = std::log(magB[k] + kSMEps);
        magOut[k] = std::exp(oneMinusAlpha * logA + alpha * logB);
    }
}

// ─── interpolatePhase() ───────────────────────────────────────────────────────

void SpectralMorphEngine::interpolatePhase(const float* phaseA, const float* phaseB,
                                            float* prevPhaseA, float* prevPhaseB,
                                            float* synthPhase,
                                            int numBins, float alpha) noexcept
{
    // Instantaneous frequency vocoder:
    //
    //   deltaA[k] = wrap(phaseA[k] - prevPhaseA[k])
    //   deltaB[k] = wrap(phaseB[k] - prevPhaseB[k])
    //   IF_morph  = (1-α)*deltaA + α*deltaB
    //   synthPhase[k] += IF_morph[k]
    //
    // wrap() maps the phase difference into [-π, π].

    const float oneMinusAlpha = 1.0f - alpha;

    for (int k = 0; k < numBins; ++k)
    {
        float deltaA = phaseA[k] - prevPhaseA[k];
        float deltaB = phaseB[k] - prevPhaseB[k];

        // Phase unwrap into [-π, π].
        // std::remainder(x, 2π) is equivalent to the while-loop unwrap but
        // handles large differences in a single step with no loop overhead.
        deltaA = std::remainder(deltaA, kSMTwoPi);
        deltaB = std::remainder(deltaB, kSMTwoPi);

        const float ifMorph = oneMinusAlpha * deltaA + alpha * deltaB;
        synthPhase[k] += ifMorph;
        // Normalize synthPhase to [-π, π] to prevent float precision loss
        // after long playback sessions (~10 h at 86 hops/s the accumulator
        // would reach ~3 million radians, where 32-bit float mantissa bits
        // are exhausted and phase increments become indistinguishable from 0).
        if (synthPhase[k] > kSMPi || synthPhase[k] < -kSMPi)
            synthPhase[k] = std::remainder(synthPhase[k], kSMTwoPi);

        prevPhaseA[k] = phaseA[k];
        prevPhaseB[k] = phaseB[k];
    }
}

} // namespace more_phi
