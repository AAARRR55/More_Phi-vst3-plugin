/*
 * More-Phi — Core/GranularMorphEngine.cpp
 *
 * Full implementation of the granular morph engine.
 *
 * Key design decisions:
 *
 *   Circular source buffers:
 *     Both source A and source B are captured as mono mixdowns into
 *     kCircularBufferSize-sample ring buffers.  Grains read from any
 *     historical position within those buffers, which gives the engine
 *     access to recently produced audio for position randomisation.
 *
 *   Grain scheduling:
 *     An accumulator (schedulerAccum_) counts fractional inter-grain
 *     intervals.  Every time it reaches 1.0 we emit a grain.  The
 *     inter-grain interval in samples = sampleRate_ / grainDensity_.
 *     The accumulator is advanced by blockSize / intervalSamples per block.
 *
 *   Source selection per grain:
 *     We use morph alpha to probabilistically choose source A or B.
 *     A random value in [0, 1) is compared to alpha:
 *       r < alpha  → source B grain   (more B the higher alpha is)
 *       r >= alpha → source A grain
 *     Amplitude is always 1.0 — individual grain volume is modulated
 *     only by the Hann envelope.
 *
 *   Pitch randomisation:
 *     pitchRatio = 2^(semitoneOffset / 12)
 *     semitoneOffset ∈ [-pitchRandom_, +pitchRandom_]
 *     Grain playback advances by pitchRatio samples per output sample
 *     (linear interpolation for sub-sample positions).
 *
 *   Position randomisation:
 *     The grain start offset is chosen randomly within a window of
 *     positionRandom_ * kCircularBufferSize samples behind the current
 *     write pointer.  At positionRandom_=0 the grain starts exactly at
 *     the most recently written samples.
 *
 *   Output:
 *     All active grains are summed into a mono mix buffer, which is then
 *     copied into every channel of the output (bufA).
 *     The result IS normalised by grain count (H5 FIX) — amplitude is
 *     divided by sqrt(activeCount * 0.5 + 1) to prevent buildup at high
 *     density.  No external makeup gain is required.
 */
#include "GranularMorphEngine.h"

#include <algorithm>
#include <cstring>

namespace more_phi {

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

GranularMorphEngine::GranularMorphEngine()
{
    for (auto& sb : sourceBuffers_)
    {
        sb.data.fill(0.0f);
        sb.writePos = 0;
    }
}

GranularMorphEngine::~GranularMorphEngine() = default;

void GranularMorphEngine::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;

    // Pre-allocate the mono mix buffer — exactly maxBlockSize floats.
    mixBuffer_.assign(static_cast<size_t>(maxBlockSize), 0.0f);

    // H-2 FIX: Pre-compute pitch ratio LUT: 2^(n/12) for n in [-12..+12]
    for (int i = 0; i < kPitchLUTSize; ++i)
        pitchLUT_[static_cast<size_t>(i)] = std::pow(2.0f, static_cast<float>(i - 12) / 12.0f);

    // Compute maximum grain length in samples from the maximum grain size (200 ms).
    const int maxGrainSamples = static_cast<int>(
        kGrainSizeMaxMs * 0.001 * sampleRate);

    // Prepare the grain pool: pre-compute the Hann envelope.
    pool_.prepare(maxGrainSamples);

    // Zero circular buffers and deactivate grains.
    reset();
}

void GranularMorphEngine::reset() noexcept
{
    pool_.reset();

    for (auto& sb : sourceBuffers_)
    {
        sb.data.fill(0.0f);
        sb.writePos = 0;
    }

    schedulerAccum_ = 0.0f;
    rngState_       = 12345u;

    // Zero the mix buffer without reallocating.
    std::fill(mixBuffer_.begin(), mixBuffer_.end(), 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::setGrainSize(float ms) noexcept
{
    grainSizeMs_ = ms < kGrainSizeMinMs ? kGrainSizeMinMs
                 : ms > kGrainSizeMaxMs ? kGrainSizeMaxMs
                 : ms;
}

void GranularMorphEngine::setGrainDensity(float grainsPerSec) noexcept
{
    grainDensity_ = grainsPerSec < kGrainDensityMin ? kGrainDensityMin
                  : grainsPerSec > kGrainDensityMax ? kGrainDensityMax
                  : grainsPerSec;
}

void GranularMorphEngine::setPitchRandomization(float semitones) noexcept
{
    pitchRandom_ = semitones < 0.0f                     ? 0.0f
                 : semitones > kPitchRandomMaxSemitones ? kPitchRandomMaxSemitones
                 : semitones;
}

void GranularMorphEngine::setPositionRandomization(float amount) noexcept
{
    positionRandom_ = amount < 0.0f ? 0.0f
                    : amount > 1.0f ? 1.0f
                    : amount;
}

void GranularMorphEngine::setActive(bool active) noexcept
{
    active_.store(active, std::memory_order_relaxed);
}

bool GranularMorphEngine::isActive() const noexcept
{
    return active_.load(std::memory_order_relaxed);
}

double GranularMorphEngine::getTailLengthSeconds() const noexcept
{
    // AUDIT-FIX: a grain already in flight continues to play out its full
    // grainSizeMs_ envelope after input stops. Report that as the tail so the
    // DAW compensates offline-bounce tails. Returns 0 when inactive.
    if (! isActive())
        return 0.0;
    return static_cast<double>(grainSizeMs_) / 1000.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread — main entry point
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::processBlock(juce::AudioBuffer<float>& bufA,
                                       const juce::AudioBuffer<float>& bufB,
                                       float alpha) noexcept
{
    if (!active_.load(std::memory_order_relaxed))
        return;

    const int numChannelsA = bufA.getNumChannels();
    const int numChannelsB = bufB.getNumChannels();
    const int numSamples   = bufA.getNumSamples();

    if (numSamples <= 0 || numChannelsA <= 0)
        return;

    // Clamp alpha to [0, 1].
    const float clampedAlpha = alpha < 0.0f ? 0.0f
                             : alpha > 1.0f ? 1.0f
                             : alpha;

    // --- 1. Feed source audio into circular buffers (mono mixdown) -----------

    // Build temporary channel pointer arrays on the stack — no heap.
    const float* chPtrsA[8]{};
    const float* chPtrsB[8]{};

    const int nA = numChannelsA < 8 ? numChannelsA : 8;
    const int nB = numChannelsB < 8 ? numChannelsB : 8;

    for (int ch = 0; ch < nA; ++ch)
        chPtrsA[ch] = bufA.getReadPointer(ch);
    for (int ch = 0; ch < nB; ++ch)
        chPtrsB[ch] = bufB.getReadPointer(ch);

    feedSourceBuffer(0, chPtrsA, nA, numSamples);
    feedSourceBuffer(1, chPtrsB, nB, numSamples);

    // --- 2. Schedule new grains ----------------------------------------------

    scheduleGrains(clampedAlpha, numSamples);

    // --- 3. Render active grains into mono mix buffer -----------------------

    // Zero the portion of mixBuffer_ we will use.
    const int usedSamples = numSamples < static_cast<int>(mixBuffer_.size())
                                ? numSamples
                                : static_cast<int>(mixBuffer_.size());

    std::fill(mixBuffer_.begin(), mixBuffer_.begin() + usedSamples, 0.0f);

    renderGrains(mixBuffer_.data(), usedSamples);

    // H5 FIX: Normalize the grain cloud so output power stays bounded as the
    // active grain count grows.
    //
    // Derivation (replaces the prior heuristic 1/sqrt(N/2+1)):
    //   Each grain reads the mono source through a Hann window w[n] and is
    //   summed into the mix. A Hann window's *time-averaged* mean-square value
    //   (the continuous / large-N limit) is
    //       <w^2> = integral_0^1 [0.5(1-cos 2pi t)]^2 dt = 3/8 = 0.375
    //   For N grains carrying the same source variance sigma^2 and treated as
    //   uncorrelated (the design intent: grains read from randomized offsets
    //   and pitches), the *time-averaged* summed-output variance is
    //       <Var(out)> = N * <w^2> * sigma^2
    //   so the output RMS scales as sqrt(N * 0.375) * sigma_rms on average.
    //   Dividing by 1 / sqrt(N * <w^2>) = 1 / sqrt(0.375 * N) keeps the cloud's
    //   AVERAGE RMS roughly independent of N — which is exactly the "no buildup
    //   at high density" behaviour the original H5 fix wanted, but with a
    //   constant derived from the window rather than a fudge factor.
    //
    //   IMPORTANT LIMITATION: 0.375 is the time-averaged <w^2>, not the
    //   instantaneous power at any one output sample. The instantaneous
    //   summed power at output sample m is sum_grain env_grain(m)^2, which
    //   varies as grains slide through their Hann envelopes (peaks when many
    //   grains are centred, dips at grain boundaries). This scalar therefore
    //   normalizes the AVERAGE level, not the instantaneous peak — short-term
    //   crest-factor variation remains. A truly peak-bounded cloud would need
    //   a per-sample normalization (look-ahead limiter), out of scope here.
    //
    //   Worst case (positionRandom_ -> 0): grains read near-coherently and
    //   amplitudes sum linearly, so peak can still grow ~N. That is
    //   fundamental to coherent grain overlap and cannot be removed by a
    //   single scalar; users who need hard bounding in that regime must lower
    //   density or raise positionRandom_. The peak-gain-vs-density unit test
    //   (TestGranularEngine.cpp) characterizes the actual curve.
    const int activeCount = pool_.getActiveCount();
    if (activeCount > 0)
    {
        constexpr float kHannMeanSquare = 0.375f;  // time-averaged <w^2> for a Hann window
        const float norm = 1.0f / std::sqrt(kHannMeanSquare * static_cast<float>(activeCount));
        for (int i = 0; i < usedSamples; ++i)
            mixBuffer_[static_cast<size_t>(i)] *= norm;
    }

    // --- 4. Add mono mix into all output channels of bufA ------------------
    // H-11 FIX: Use additive mixing (+=) instead of overwrite (=) so that
    // the granular engine can layer on top of spectral engine output.

    auto* ch0Out = bufA.getWritePointer(0);
    for (int i = 0; i < usedSamples; ++i)
        ch0Out[i] += mixBuffer_[static_cast<size_t>(i)];

    for (int ch = 1; ch < numChannelsA; ++ch)
    {
        auto* chOut = bufA.getWritePointer(ch);
        for (int i = 0; i < usedSamples; ++i)
            chOut[i] += mixBuffer_[static_cast<size_t>(i)];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PRNG — xorshift32
// ─────────────────────────────────────────────────────────────────────────────

float GranularMorphEngine::nextRandom() noexcept
{
    // Standard xorshift32 — Marsaglia 2003.
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;

    // Map uint32 → [0, 1)
    // Multiply by 1/2^32 (≈ 2.328e-10).
    return static_cast<float>(rngState_) * 2.3283064365386963e-10f;
}

// ─────────────────────────────────────────────────────────────────────────────
// feedSourceBuffer — capture audio into circular buffer (mono mixdown)
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::feedSourceBuffer(int sourceIndex,
                                            const float* const* channelData,
                                            int numChannels,
                                            int numSamples) noexcept
{
    auto& sb = sourceBuffers_[static_cast<size_t>(sourceIndex)];

    if (numChannels <= 0 || numSamples <= 0)
        return;

    // Clamp to circular buffer capacity to prevent write pointer from lapping
    numSamples = std::min(numSamples, kCircularBufferSize);

    const float normalise = 1.0f / static_cast<float>(numChannels);

    for (int s = 0; s < numSamples; ++s)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            mono += channelData[ch][s];

        mono *= normalise;

        sb.data[static_cast<size_t>(sb.writePos)] = mono;
        sb.writePos = (sb.writePos + 1) & (kCircularBufferSize - 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// scheduleGrains — emit new grains for this block
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::scheduleGrains(float alpha, int blockSize) noexcept
{
    // Inter-grain interval in samples (floating-point for accuracy).
    const float intervalSamples = static_cast<float>(sampleRate_) / grainDensity_;

    // Grain length in samples, clamped to valid range.
    const int grainLengthSamples =
        static_cast<int>(grainSizeMs_ * 0.001f * static_cast<float>(sampleRate_));

    // DEEP-DIVE FIX: save the fractional phase before the increment so we can
    // compute each grain's output start sample within the block. Without this,
    // all grains emitted in one block start at sample 0, causing amplitude
    // modulation at the block rate when blockSize > intervalSamples.
    const float startFraction = schedulerAccum_;

    // Increment accumulator by blockSize / interval.
    schedulerAccum_ += static_cast<float>(blockSize) / intervalSamples;

    // Emit one grain for every time the accumulator crosses 1.0.
    int grainIndex = 0;
    while (schedulerAccum_ >= 1.0f)
    {
        schedulerAccum_ -= 1.0f;

        // DEEP-DIVE FIX: compute the sample offset within this block where this
        // grain should start. The Nth grain fires when the total elapsed
        // intervals from block start reaches (1 - startFraction + N).
        // Samples = intervals * intervalSamples.
        const float offsetF = (1.0f - startFraction + static_cast<float>(grainIndex))
                            * intervalSamples;
        const int outputStartSample = std::max(0, std::min(blockSize - 1,
                                               static_cast<int>(offsetF)));
        ++grainIndex;

        // Choose source: random < alpha → source B, otherwise source A.
        const float r0 = nextRandom();
        const int srcIdx = (r0 < alpha) ? 1 : 0;

        // Amplitude = 1.0 — the Hann envelope provides the shaping.
        const float amplitude = 1.0f;

        // Position randomisation: read from up to positionRandom_ * bufferSize
        // samples behind the current write pointer.
        const int maxOffset = static_cast<int>(
            positionRandom_ * static_cast<float>(kCircularBufferSize - grainLengthSamples));

        int startOffset = 0;
        if (maxOffset > 0)
        {
            const float r1 = nextRandom();
            startOffset = static_cast<int>(r1 * static_cast<float>(maxOffset));
        }

        // Guard: skip grain if combined length + offset would exceed circular buffer
        // capacity.  Without this, the read pointer aliases into unwritten memory.
        if (grainLengthSamples + startOffset >= kCircularBufferSize)
            continue;   // Grain too long for buffer depth — skip rather than alias

        // Convert offset to an absolute read position in the circular buffer.
        // We go backwards from writePos so recent audio is at offset=0.
        const auto& sb = sourceBuffers_[static_cast<size_t>(srcIdx)];
        const int readStart = (sb.writePos - grainLengthSamples - startOffset
                               + kCircularBufferSize * 2) & (kCircularBufferSize - 1);

        // Pitch randomisation: random semitone deviation in [-pitch, +pitch].
        // H-2 FIX: Use pre-computed LUT instead of std::pow() on audio thread.
        float pitchRatio = 1.0f;
        if (pitchRandom_ > 0.0f)
        {
            const float r2     = nextRandom(); // [0, 1)
            const float offset = (r2 * 2.0f - 1.0f) * pitchRandom_; // [-max, +max] semitones
            const float lutIndex = offset + 12.0f;
            const int lo = std::clamp(static_cast<int>(lutIndex), 0, kPitchLUTSize - 2);
            const float frac = lutIndex - static_cast<float>(lo);
            pitchRatio = pitchLUT_[static_cast<size_t>(lo)] * (1.0f - frac)
                       + pitchLUT_[static_cast<size_t>(lo + 1)] * frac;
        }

        (void)pool_.activate(srcIdx, amplitude, readStart, grainLengthSamples,
                             pitchRatio, outputStartSample);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// renderGrains — advance and accumulate all active grains
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::renderGrains(float* output, int numSamples) noexcept
{
    pool_.forEachActive([&](Grain& g, int grainIdx) noexcept
    {
        const auto& sb = sourceBuffers_[static_cast<size_t>(g.sourceSelect)];

        for (int s = 0; s < numSamples; ++s)
        {
            // DEEP-DIVE FIX: skip output samples before this grain's start offset
            // so multiple grains emitted within a block are staggered in time.
            if (s < g.outputStartSample)
                continue;

            // Check if this grain has finished.
            if (g.currentPos >= g.grainLength)
            {
                pool_.deactivate(grainIdx);
                break;
            }

            // Hann envelope — normalised position in [0, 1].
            const float normPos =
                static_cast<float>(g.currentPos) / static_cast<float>(g.grainLength - 1);
            const float env = pool_.getEnvelope(normPos);

            // Read source sample with cubic Hermite interpolation for pitch
            // shift. Sub-sample position within the circular buffer.
            const float readPosF =
                static_cast<float>(g.startSample)
                + static_cast<float>(g.currentPos) * g.pitchRatio;

            // Integer and fractional parts.
            const int   readPosInt  = static_cast<int>(readPosF);
            const float readPosFrac = readPosF - static_cast<float>(readPosInt);

            // AUDIT-FIX (DSP): replaced 2-point linear interpolation with 4-point
            // cubic Hermite. Linear resampling has poor high-frequency response
            // and aliases on pitch shifts (no anti-imaging). Cubic Hermite
            // substantially reduces aliasing/imaging at ~3 extra multiplies per
            // output sample. The circular buffer is power-of-2 (masked below),
            // so (n - 1) wraps correctly under two's-complement masking.
            constexpr int mask = kCircularBufferSize - 1;
            const size_t idxm1 = static_cast<size_t>((readPosInt - 1) & mask);
            const size_t idx0  = static_cast<size_t>( readPosInt      & mask);
            const size_t idx1  = static_cast<size_t>((readPosInt + 1) & mask);
            const size_t idx2  = static_cast<size_t>((readPosInt + 2) & mask);

            const float xm1 = sb.data[idxm1];
            const float x0  = sb.data[idx0];
            const float x1  = sb.data[idx1];
            const float x2  = sb.data[idx2];

            const float c0 = x0;
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            const float sample = ((c3 * readPosFrac + c2) * readPosFrac + c1) * readPosFrac + c0;

            output[s] += sample * env * g.amplitude;

            ++g.currentPos;
        }
    });
}

} // namespace more_phi
