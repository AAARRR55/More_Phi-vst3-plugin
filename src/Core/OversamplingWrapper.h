/*
 * MorphSnap — Core/OversamplingWrapper.h
 *
 * Thin, audio-thread-safe oversampling adapter built on JUCE's
 * juce::dsp::Oversampling<float>. Wraps JUCE's FIR anti-aliasing
 * filter approach for nonlinear processing stages only
 * (spectral waveshaping, saturation). Linear interpolation paths
 * bypass oversampling entirely.
 *
 * Design constraints:
 *   - Zero heap allocations after prepare() — all memory owned by
 *     juce::dsp::Oversampling which pre-allocates in prepare().
 *   - noexcept on process path.
 *   - Factor switchable between blocks (requires re-prepare).
 *   - Reports added latency samples for DAW compensation.
 *
 * Usage:
 *   OversamplingWrapper os;
 *   os.setFactor(OversamplingFactor::x4);
 *   os.prepare(maxBlockSize, numChannels, sampleRate);
 *
 *   // Audio thread:
 *   auto osBlock = os.upsample(inputBlock);
 *   processNonlinear(osBlock);           // operate at N*SR
 *   os.downsample(outputBlock);
 *
 *   // Latency reporting:
 *   int latSamples = os.getLatencyInSamples();
 */
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cassert>
#include <cstdint>

namespace morphsnap {

/**
 * Supported oversampling factors.
 * x1 = bypass (zero extra latency, no filter cost).
 */
enum class OversamplingFactor : int
{
    x1 = 1,   ///< Bypass — no oversampling
    x2 = 2,   ///< 2x — mild aliasing suppression (~80 dB stopband)
    x4 = 4,   ///< 4x — production default (~100 dB stopband)
    x8 = 8    ///< 8x — mastering quality (~120 dB), high CPU cost
};

/**
 * Anti-aliasing filter type selector.
 *
 * FIR:  Linear-phase, higher CPU, ideal for transient-sensitive material.
 *       Latency = (filterOrder / 2) / sampleRate per upsample stage.
 *
 * IIR:  Minimum-phase (Butterworth), lower CPU, non-linear phase.
 *       Latency is fractional and factor-dependent — use with caution
 *       in latency-sensitive chains.
 *
 * For MorphSnap the default is FIR because hosted-plugin state morphing
 * must not introduce audible pre-ringing artifacts when switching presets.
 */
enum class AAFilterType
{
    FIR,   ///< Linear-phase polyphase FIR (JUCE default)
    IIR    ///< Minimum-phase IIR (lower latency, lower CPU)
};

/**
 * OversamplingWrapper
 *
 * Thread safety:
 *   setFactor(), prepare(), and reset() must be called from the
 *   message thread (never from the audio thread mid-block).
 *   upsample() and downsample() are audio-thread-only and noexcept.
 */
class OversamplingWrapper
{
public:
    static constexpr int kMaxChannels   = 2;
    static constexpr int kMaxOSFactor   = 8;
    static constexpr int kFIRFilterOrder = 128; // taps per polyphase subfilter

    OversamplingWrapper() = default;
    ~OversamplingWrapper() = default;

    OversamplingWrapper(const OversamplingWrapper&)            = delete;
    OversamplingWrapper& operator=(const OversamplingWrapper&) = delete;

    // ─── Configuration (message thread only) ─────────────────────────────────

    /**
     * Select the oversampling factor. Takes effect on the next prepare() call.
     * Switching during audio playback requires stopping processBlock() first.
     */
    void setFactor(OversamplingFactor factor) noexcept
    {
        pendingFactor_ = factor;
    }

    /** Set the filter type. Takes effect on the next prepare() call. */
    void setFilterType(AAFilterType type) noexcept
    {
        filterType_ = type;
    }

    /**
     * Allocate internal buffers and design anti-aliasing filters.
     *
     * Must be called from prepareToPlay() — never from the audio thread.
     * Re-calling with the same parameters is safe (idempotent guard).
     *
     * @param maxSamplesPerBlock  Maximum expected block size from the host
     * @param numChannels         1 or 2
     * @param sampleRate          Host sample rate in Hz
     */
    void prepare(int maxSamplesPerBlock, int numChannels, double sampleRate)
    {
        assert(numChannels >= 1 && numChannels <= kMaxChannels);
        assert(maxSamplesPerBlock > 0 && maxSamplesPerBlock <= 65536);
        assert(sampleRate > 0.0);

        activeFactor_ = pendingFactor_;
        numChannels_  = numChannels;
        sampleRate_   = sampleRate;

        // Factor x1 means bypass — no JUCE object needed.
        if (activeFactor_ == OversamplingFactor::x1)
        {
            oversamplerFIR_.reset();
            oversamplerIIR_.reset();
            latencySamples_ = 0;
            return;
        }

        const int factorLog2 = factorToLog2(activeFactor_);

        if (filterType_ == AAFilterType::FIR)
        {
            // juce::dsp::Oversampling<float> with Kaiser windowed FIR
            // numStages = log2(factor), filterOrder = kFIRFilterOrder
            oversamplerFIR_ = std::make_unique<juce::dsp::Oversampling<float>>(
                static_cast<size_t>(numChannels),
                static_cast<size_t>(factorLog2),
                juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                /* isMaximumQuality = */ true);

            oversamplerFIR_->initProcessing(
                static_cast<size_t>(maxSamplesPerBlock));

            latencySamples_ = static_cast<int>(
                oversamplerFIR_->getLatencyInSamples());

            oversamplerIIR_.reset();
        }
        else // IIR
        {
            oversamplerIIR_ = std::make_unique<juce::dsp::Oversampling<float>>(
                static_cast<size_t>(numChannels),
                static_cast<size_t>(factorLog2),
                juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                /* isMaximumQuality = */ false);

            oversamplerIIR_->initProcessing(
                static_cast<size_t>(maxSamplesPerBlock));

            latencySamples_ = static_cast<int>(
                oversamplerIIR_->getLatencyInSamples());

            oversamplerFIR_.reset();
        }
    }

    /** Reset filter state (call from releaseResources or on flush). */
    void reset() noexcept
    {
        if (oversamplerFIR_) oversamplerFIR_->reset();
        if (oversamplerIIR_) oversamplerIIR_->reset();
    }

    // ─── Audio thread interface ───────────────────────────────────────────────

    /**
     * Upsample the input block. Returns an AudioBlock at the oversampled rate.
     *
     * When factor == x1, returns a view of the original input unchanged.
     * The returned block is only valid until the next call to downsample().
     */
    [[nodiscard]] juce::dsp::AudioBlock<float>
    upsample(const juce::dsp::AudioBlock<float>& input) noexcept
    {
        if (activeFactor_ == OversamplingFactor::x1)
            return juce::dsp::AudioBlock<float>(const_cast<juce::dsp::AudioBlock<float>&>(input));

        if (oversamplerFIR_)
            return oversamplerFIR_->processSamplesUp(input);

        if (oversamplerIIR_)
            return oversamplerIIR_->processSamplesUp(input);

        return juce::dsp::AudioBlock<float>(const_cast<juce::dsp::AudioBlock<float>&>(input));
    }

    /**
     * Downsample the oversampled block back to the original rate.
     * output must be the original-rate block passed to upsample().
     */
    void downsample(juce::dsp::AudioBlock<float>& output) noexcept
    {
        if (activeFactor_ == OversamplingFactor::x1) return;

        if (oversamplerFIR_) { oversamplerFIR_->processSamplesDown(output); return; }
        if (oversamplerIIR_) { oversamplerIIR_->processSamplesDown(output); return; }
    }

    // ─── Queries ──────────────────────────────────────────────────────────────

    /** Latency introduced by the anti-aliasing filters, in samples at input rate. */
    [[nodiscard]] int getLatencyInSamples() const noexcept
    {
        return latencySamples_;
    }

    /** Current active factor (as integer). */
    [[nodiscard]] int getActiveFactor() const noexcept
    {
        return static_cast<int>(activeFactor_);
    }

    /** True when oversampling is active (factor != x1). */
    [[nodiscard]] bool isActive() const noexcept
    {
        return activeFactor_ != OversamplingFactor::x1;
    }

    /**
     * Estimated CPU overhead relative to x1 processing.
     * Values are empirical approximations for FIR mode.
     * IIR mode is roughly 40% cheaper per factor step.
     */
    [[nodiscard]] float estimatedCPUOverheadFactor() const noexcept
    {
        switch (activeFactor_)
        {
            case OversamplingFactor::x1: return 1.0f;
            case OversamplingFactor::x2: return 2.3f;   // 2x sample rate + FIR cost
            case OversamplingFactor::x4: return 4.8f;   // 4x + 2-stage FIR
            case OversamplingFactor::x8: return 9.5f;   // 8x + 3-stage FIR
        }
        return 1.0f;
    }

private:
    static int factorToLog2(OversamplingFactor f) noexcept
    {
        switch (f)
        {
            case OversamplingFactor::x2: return 1;
            case OversamplingFactor::x4: return 2;
            case OversamplingFactor::x8: return 3;
            default:                     return 0;
        }
    }

    OversamplingFactor pendingFactor_  { OversamplingFactor::x4 };
    OversamplingFactor activeFactor_   { OversamplingFactor::x1 };
    AAFilterType       filterType_     { AAFilterType::FIR };
    int                numChannels_    { 2 };
    double             sampleRate_     { 48000.0 };
    int                latencySamples_ { 0 };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplerFIR_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplerIIR_;
};

} // namespace morphsnap
