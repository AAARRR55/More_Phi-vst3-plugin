/*
 * More-Phi — Core/HybridBlend.h
 *
 * Equal-power mixing of parameter-domain, spectral, and granular morph
 * outputs into a single output buffer.
 *
 * Design constraints:
 *   - Header-only (all methods are static).
 *   - Zero heap allocations — operates on pre-allocated AudioBuffers.
 *   - All methods are noexcept.
 *   - Equal-power gain: each weight w → sqrt(w) so that when two paths
 *     share weight 0.5 each, total perceived loudness stays constant.
 *   - Channel count mismatches are handled by clamping the source channel
 *     index to the last available channel of each buffer.
 *   - Sample count mismatches are handled: only the minimum of all buffer
 *     sizes is processed; trailing output samples are zeroed.
 *
 * Threading:
 *   blend() and blendTwo() are static methods — audio-thread safe, noexcept,
 *   zero allocations. All weight parameters are passed by value (already
 *   loaded from atomics by the caller).
 *
 * Usage — three-way blend:
 *   HybridBlend::blend(outputBuf,
 *                      paramBuf,    spectralBuf, granularBuf,
 *                      paramWeight, spectralWeight, granularWeight);
 *
 * Usage — two-way blend (parameter + one other domain):
 *   HybridBlend::blendTwo(outputBuf, bufA, bufB, alphaB);
 *
 * Note on weight normalisation:
 *   blend() accepts unnormalised weights and normalises them internally.
 *   blendTwo() interprets alphaB directly as a fractional blend in [0, 1].
 *   A helper normaliseWeights() is provided for callers that need to
 *   normalise weights before passing them to other uses.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

namespace more_phi {

/**
 * HybridBlend
 *
 * Static utility class — never instantiated.
 *
 * Blends parameter-domain, spectral, and granular outputs into a single
 * output buffer using per-stream equal-power weights. The output is
 * written into `output` in place.
 */
class HybridBlend
{
public:
    HybridBlend()                              = delete;  // All-static utility class
    HybridBlend(const HybridBlend&)            = delete;
    HybridBlend& operator=(const HybridBlend&) = delete;

    // ─── Three-way equal-power blend ──────────────────────────────────────────

    /**
     * Blend three audio domain outputs into `output` using equal-power gains.
     *
     * Weights are normalised internally so their sum equals 1.0 before the
     * sqrt (equal-power) conversion is applied. If all weights are zero,
     * the output is cleared to silence.
     *
     * gain_x = sqrt(weight_x / totalWeight)
     *
     * @param output         Output buffer (written in place).
     * @param paramOut       Parameter-domain morph result (Plugin A processed).
     * @param spectralOut    SpectralMorphEngine output.
     * @param granularOut    GranularMorphEngine output.
     * @param paramWeight    Linear weight for the parameter path (>= 0).
     * @param spectralWeight Linear weight for the spectral path (>= 0).
     * @param granularWeight Linear weight for the granular path (>= 0).
     */
    static void blend(juce::AudioBuffer<float>& output,
                      const juce::AudioBuffer<float>& paramOut,
                      const juce::AudioBuffer<float>& spectralOut,
                      const juce::AudioBuffer<float>& granularOut,
                      float paramWeight,
                      float spectralWeight,
                      float granularWeight) noexcept
    {
        // Clamp negative weights.
        paramWeight    = std::max(0.0f, paramWeight);
        spectralWeight = std::max(0.0f, spectralWeight);
        granularWeight = std::max(0.0f, granularWeight);

        const float totalWeight = paramWeight + spectralWeight + granularWeight;

        // If all weights are zero, output silence to avoid undefined behaviour.
        if (totalWeight < 1.0e-6f)
        {
            output.clear();
            return;
        }

        // Normalise weights then apply equal-power (sqrt) conversion.
        const float invTotal = 1.0f / totalWeight;
        const float pGain    = std::sqrt(paramWeight    * invTotal);
        const float sGain    = std::sqrt(spectralWeight * invTotal);
        const float gGain    = std::sqrt(granularWeight * invTotal);

        const int numChannels = output.getNumChannels();
        const int numSamples  = output.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dst = output.getWritePointer(ch);

            // Clamp channel indices to valid source buffer ranges.
            const int pCh = std::min(ch, paramOut.getNumChannels()   - 1);
            const int sCh = std::min(ch, spectralOut.getNumChannels() - 1);
            const int gCh = std::min(ch, granularOut.getNumChannels() - 1);

            const float* pSrc = paramOut   .getReadPointer(pCh);
            const float* sSrc = spectralOut.getReadPointer(sCh);
            const float* gSrc = granularOut.getReadPointer(gCh);

            // Only process up to the minimum valid sample count across all buffers.
            const int n = std::min(numSamples,
                          std::min(paramOut.getNumSamples(),
                          std::min(spectralOut.getNumSamples(),
                                   granularOut.getNumSamples())));

            // PERF-5: Use FloatVectorOperations for SIMD-accelerated blending.
            // Three-way blend: clear, then accumulate each scaled contribution.
            // addWithMultiply handles the multiply-add in SIMD batches internally.
            juce::FloatVectorOperations::clear(dst, n);
            juce::FloatVectorOperations::addWithMultiply(dst, pSrc, pGain, n);
            juce::FloatVectorOperations::addWithMultiply(dst, sSrc, sGain, n);
            juce::FloatVectorOperations::addWithMultiply(dst, gSrc, gGain, n);

            // Zero any trailing samples if output is longer than sources.
            for (int i = n; i < numSamples; ++i)
                dst[i] = 0.0f;
        }
    }

    // ─── Two-way equal-power blend ────────────────────────────────────────────

    /**
     * Equal-power crossfade between two buffers.
     *
     * Intended for situations where only the parameter path and one additional
     * domain (spectral or granular) are active simultaneously.
     *
     * gainA = sqrt(1 - alphaB)
     * gainB = sqrt(alphaB)
     *
     * @param output   Destination buffer — must be pre-allocated.
     * @param bufA     First source (e.g. parameter-domain output).
     * @param bufB     Second source (e.g. spectral or granular output).
     * @param alphaB   Blend factor: 0.0 = all bufA, 1.0 = all bufB.
     *                 Clamped to [0, 1] internally.
     */
    static void blendTwo(juce::AudioBuffer<float>& output,
                         const juce::AudioBuffer<float>& bufA,
                         const juce::AudioBuffer<float>& bufB,
                         float alphaB) noexcept
    {
        // Clamp alpha to [0, 1].
        const float a     = alphaB < 0.0f ? 0.0f
                          : alphaB > 1.0f ? 1.0f
                          : alphaB;

        const float gainA = std::sqrt(1.0f - a);
        const float gainB = std::sqrt(a);

        const int numChannels = output.getNumChannels();
        const int numSamples  = output.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dst = output.getWritePointer(ch);

            const int aCh = std::min(ch, bufA.getNumChannels() - 1);
            const int bCh = std::min(ch, bufB.getNumChannels() - 1);

            const float* aSrc = bufA.getReadPointer(aCh);
            const float* bSrc = bufB.getReadPointer(bCh);

            const int n = std::min(numSamples,
                          std::min(bufA.getNumSamples(), bufB.getNumSamples()));

            // PERF-5: Use FloatVectorOperations for SIMD-accelerated two-way blend.
            juce::FloatVectorOperations::clear(dst, n);
            juce::FloatVectorOperations::addWithMultiply(dst, aSrc, gainA, n);
            juce::FloatVectorOperations::addWithMultiply(dst, bSrc, gainB, n);

            for (int i = n; i < numSamples; ++i)
                dst[i] = 0.0f;
        }
    }

    // ─── Weight utility ───────────────────────────────────────────────────────

    /**
     * Normalise three weights in-place so they sum to 1.0.
     *
     * Negative values are clamped to 0. If all weights are zero,
     * paramWeight is set to 1.0 as a safe fallback — the parameter-domain
     * morph is the baseline path and should never produce silence.
     *
     * @param paramWeight    In/out: parameter-domain weight.
     * @param spectralWeight In/out: spectral weight.
     * @param granularWeight In/out: granular weight.
     */
    static void normaliseWeights(float& paramWeight,
                                 float& spectralWeight,
                                 float& granularWeight) noexcept
    {
        paramWeight    = std::max(0.0f, paramWeight);
        spectralWeight = std::max(0.0f, spectralWeight);
        granularWeight = std::max(0.0f, granularWeight);

        const float total = paramWeight + spectralWeight + granularWeight;

        if (total > 1.0e-6f)
        {
            const float invTotal = 1.0f / total;
            paramWeight    *= invTotal;
            spectralWeight *= invTotal;
            granularWeight *= invTotal;
        }
        else
        {
            // Safe fallback: 100% parameter domain.
            paramWeight    = 1.0f;
            spectralWeight = 0.0f;
            granularWeight = 0.0f;
        }
    }
};

} // namespace more_phi
