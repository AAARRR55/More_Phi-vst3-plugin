/*
 * MorphSnap — Core/GranularMorphEngine.cpp
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
 *     The result is NOT normalised by grain count — density + amplitude
 *     together determine the final level.  Callers should apply a makeup
 *     gain proportional to 1/sqrt(density) if needed.
 */
#include "GranularMorphEngine.h"

#include <algorithm>
#include <cstring>

namespace morphsnap {

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
    active_ = active;
}

bool GranularMorphEngine::isActive() const noexcept
{
    return active_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread — main entry point
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::processBlock(juce::AudioBuffer<float>& bufA,
                                       const juce::AudioBuffer<float>& bufB,
                                       float alpha) noexcept
{
    if (!active_)
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

    // --- 4. Copy mono mix into all output channels of bufA ------------------

    auto* ch0Out = bufA.getWritePointer(0);
    for (int i = 0; i < usedSamples; ++i)
        ch0Out[i] = mixBuffer_[static_cast<size_t>(i)];

    for (int ch = 1; ch < numChannelsA; ++ch)
    {
        auto* chOut = bufA.getWritePointer(ch);
        for (int i = 0; i < usedSamples; ++i)
            chOut[i] = mixBuffer_[static_cast<size_t>(i)];
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

    // Increment accumulator by blockSize / interval.
    schedulerAccum_ += static_cast<float>(blockSize) / intervalSamples;

    // Emit one grain for every time the accumulator crosses 1.0.
    while (schedulerAccum_ >= 1.0f)
    {
        schedulerAccum_ -= 1.0f;

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

        // Convert offset to an absolute read position in the circular buffer.
        // We go backwards from writePos so recent audio is at offset=0.
        const auto& sb = sourceBuffers_[static_cast<size_t>(srcIdx)];
        const int readStart = (sb.writePos - grainLengthSamples - startOffset
                               + kCircularBufferSize * 2) & (kCircularBufferSize - 1);

        // Pitch randomisation: random semitone deviation in [-pitch, +pitch].
        float pitchRatio = 1.0f;
        if (pitchRandom_ > 0.0f)
        {
            const float r2     = nextRandom(); // [0, 1)
            const float offset = (r2 * 2.0f - 1.0f) * pitchRandom_; // [-max, +max] semitones
            pitchRatio = std::pow(2.0f, offset / 12.0f);
        }

        pool_.activate(srcIdx, amplitude, readStart, grainLengthSamples, pitchRatio);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// renderGrains — advance and accumulate all active grains
// ─────────────────────────────────────────────────────────────────────────────

void GranularMorphEngine::renderGrains(float* output, int numSamples) noexcept
{
    pool_.forEachActive([&](Grain& g, int grainIdx)
    {
        const auto& sb = sourceBuffers_[static_cast<size_t>(g.sourceSelect)];

        for (int s = 0; s < numSamples; ++s)
        {
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

            // Read source sample with linear interpolation for pitch shift.
            // Sub-sample position within the circular buffer.
            const float readPosF =
                static_cast<float>(g.startSample)
                + static_cast<float>(g.currentPos) * g.pitchRatio;

            // Integer and fractional parts.
            const int   readPosInt  = static_cast<int>(readPosF);
            const float readPosFrac = readPosF - static_cast<float>(readPosInt);

            const int idx0 = readPosInt                     & (kCircularBufferSize - 1);
            const int idx1 = (readPosInt + 1)               & (kCircularBufferSize - 1);

            const float s0 = sb.data[static_cast<size_t>(idx0)];
            const float s1 = sb.data[static_cast<size_t>(idx1)];

            // Linear interpolation.
            const float sample = s0 + readPosFrac * (s1 - s0);

            output[s] += sample * env * g.amplitude;

            ++g.currentPos;
        }
    });
}

} // namespace morphsnap
