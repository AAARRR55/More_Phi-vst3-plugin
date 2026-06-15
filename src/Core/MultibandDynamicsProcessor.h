/*
 * More-Phi — Core/MultibandDynamicsProcessor.h
 *
 * Per-band feedforward VCA compressor for the 4-band mastering chain.
 *
 * Each band uses the existing EnvelopeFollower for RMS detection, then
 * applies a soft-knee compressor gain computation.  Gain is applied via
 * juce::FloatVectorOperations for SIMD efficiency.
 *
 * Heuristic defaults (used when NeuralCompressor is not loaded):
 *   Band 0 (Sub):  ratio 1.5:1, attack 50ms,  release 200ms, thresh -18 dBFS
 *   Band 1 (Low):  ratio 2.5:1, attack 15ms,  release 150ms, thresh -20 dBFS
 *   Band 2 (Mid):  ratio 3.0:1, attack  8ms,  release 120ms, thresh -22 dBFS
 *   Band 3 (High): ratio 2.0:1, attack  3ms,  release  80ms, thresh -18 dBFS
 *
 * Transient preservation: TransientDetector gate holds attack at ≥15ms when
 * a transient is detected in the input (prevents punch loss).
 *
 * Thread safety:
 *   processBlock() — audio thread only, noexcept.
 *   setBandParams() — atomic writes, any thread.
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>
#include "EnvelopeFollower.h"

namespace more_phi {

class MultibandDynamicsProcessor
{
public:
    static constexpr int kNumBands    = 4;
    static constexpr int kMaxChannels = 2;

    struct BandParams
    {
        float thresholdDB = -20.0f;
        float ratio       =   2.5f;
        float attackMs    =  15.0f;
        float releaseMs   = 150.0f;
        float makeupDB    =   0.0f;
        float kneeDB      =   2.0f;  // soft-knee width in dB
    };

    MultibandDynamicsProcessor();

    // ── Configuration (any thread — atomic) ───────────────────────────────────

    /** Set compressor parameters for band [0..3]. */
    void setBandParams(int band, const BandParams& p) noexcept;

    /** Get current parameters for a band. */
    BandParams getBandParams(int band) const noexcept;

    /** Enable / bypass a specific band. */
    void setBandEnabled(int band, bool enabled) noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Process all 4 bands in-place.
     * Each bands[i] must have at least numSamples per channel.
     * noexcept — EnvelopeFollower::process() is noexcept.
     */
    void processBlock(juce::AudioBuffer<float> bands[kNumBands]) noexcept;

    // ── Metering ──────────────────────────────────────────────────────────────

    [[nodiscard]] float getGainReductionDB(int band) const noexcept
    {
        return grDB_[band].load(std::memory_order_relaxed);
    }

private:
    struct BandState
    {
        // Atomic param copies (audio thread reads these)
        std::atomic<float> thresholdLinear { std::pow(10.f, -20.f / 20.f) };
        std::atomic<float> ratio           { 2.5f };
        std::atomic<float> attackMs        { 15.f };
        std::atomic<float> releaseMs       { 150.f };
        std::atomic<float> makeupLinear    { 1.0f };
        std::atomic<float> kneeDB          { 2.0f };
        std::atomic<bool>  enabled         { true };

        // Per-channel envelope followers
        EnvelopeFollower followers[kMaxChannels];

        // Smoothed gain (audio thread only)
        float gainSmoothed[kMaxChannels] = { 1.0f, 1.0f };

        // MULTIBAND-2/3: single stereo-linked, per-sample detector envelope.
        float envLinked = 0.0f;
    };

    std::array<BandState, kNumBands> bands_{};
    std::array<std::atomic<float>, kNumBands> grDB_{};

    double sampleRate_ = 48000.0;

    float computeCompressorGain(float rmsLinear,
                                 float threshLinear,
                                 float ratio,
                                 float kneeDB,
                                 float makeup) const noexcept;

    void applyDefaults() noexcept;
};

} // namespace more_phi
