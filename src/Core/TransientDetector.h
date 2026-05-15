/*
 * More-Phi — Core/TransientDetector.h
 *
 * Header-only spectral flux transient detector used by SpectralMorphEngine.
 * Computes the per-hop increase in spectral energy and uses a soft threshold
 * to snap the morph alpha toward 0 or 1 during transient events, preserving
 * attack character rather than smearing it through phase-vocoder interpolation.
 *
 * Design constraints:
 *   - Zero heap allocations: prevMag is a fixed std::array<float, kMaxBins>.
 *   - All methods noexcept — called directly from the audio thread.
 *   - Release is exponential: transientMix decays at kReleaseMs after detection.
 *   - Threshold is a ratio (kThreshold = 3.0): flux must be 3× the running mean
 *     for the frame to be flagged as a transient.
 *
 * Usage:
 *   TransientDetector det;
 *   det.prepare(44100.0f, 512);   // call once in prepare()
 *
 *   // Audio thread — once per hop:
 *   float adjustedAlpha = det.process(magA, numBins, rawAlpha);
 */
#pragma once

#include <array>
#include <cmath>
#include <algorithm>

namespace more_phi {

/**
 * Spectral-flux transient detector.
 *
 * Thread safety:
 *   prepare() and reset() must be called from the message thread.
 *   process() is audio-thread-only and noexcept.
 */
struct TransientDetector
{
    // ── Constants ────────────────────────────────────────────────────────────

    /** Flux ratio above running mean required to trigger transient detection. */
    static constexpr float kThreshold  = 3.0f;

    /** Post-transient hold time in milliseconds before alpha returns to target. */
    static constexpr float kReleaseMs  = 50.0f;

    /** Maximum supported FFT bin count (half of max FFT size 8192 + 1). */
    static constexpr int   kMaxBins    = 4096;

    // ── State ─────────────────────────────────────────────────────────────────

    /** Magnitude spectrum from the previous hop (for flux computation). */
    std::array<float, kMaxBins> prevMag{};

    /**
     * One-pole release coefficient.
     * Computed in prepare() as exp(-1 / (releaseMs * hopsPerSecond)).
     * A value near 1.0 gives a long release; 0.0 = instantaneous.
     */
    float releaseCoeff = 0.0f;

    /**
     * Current transient mix factor in [0, 1].
     * 0 = no transient influence (alpha passes through unchanged).
     * 1 = full transient snap (alpha is hard-switched to nearest 0 or 1).
     * Ramps to 1 on detection, then decays exponentially via releaseCoeff.
     */
    float transientMix = 0.0f;

    // ── Running spectral mean (single-pole IIR, τ ≈ 200ms) ──────────────────
    float runningFluxMean_ = 0.0f;

    // ── Interface ─────────────────────────────────────────────────────────────

    /**
     * Initialise the release envelope.
     *
     * @param sampleRate  Host sample rate (Hz).
     * @param hopSize     STFT hop size in samples (determines hops-per-second).
     */
    void prepare(float sampleRate, int hopSize) noexcept
    {
        const float hopsPerSecond  = sampleRate / static_cast<float>(hopSize);
        const float releaseHops    = (kReleaseMs / 1000.0f) * hopsPerSecond;

        // exp(-1/τ) one-pole coefficient; clamp to avoid NaN on degenerate input
        if (releaseHops > 0.0f)
            releaseCoeff = std::exp(-1.0f / releaseHops);
        else
            releaseCoeff = 0.0f;

        reset();
    }

    /**
     * Process one STFT frame.
     *
     * Computes half-wave-rectified spectral flux across numBins, compares
     * against a running mean, and adjusts alpha via a hard-snap blend.
     *
     * @param mag      Magnitude spectrum of the current frame (length >= numBins).
     * @param numBins  Number of positive-frequency bins (fftSize/2 + 1).
     * @param alpha    Raw morph position in [0, 1] (0 = A, 1 = B).
     * @returns        Modified alpha: snaps to 0.0 or 1.0 during transients.
     */
    float process(const float* mag, int numBins, float alpha) noexcept
    {
        // ── 1. Compute half-wave-rectified spectral flux ─────────────────────
        //   flux = Σ max(0, |M[k]| - |M_prev[k]|) for k in [0, numBins)
        float flux = 0.0f;
        const int n = std::min(numBins, kMaxBins);
        for (int k = 0; k < n; ++k)
        {
            const float diff = mag[k] - prevMag[k];
            if (diff > 0.0f)
                flux += diff;
        }

        // Store current magnitude for next hop
        for (int k = 0; k < n; ++k)
            prevMag[k] = mag[k];

        // ── 2. Update running mean (single-pole IIR, α ≈ 0.02) ──────────────
        //   Mean time constant ≈ 50 hops ≈ 200ms at 44.1kHz / 512-sample hop
        static constexpr float kMeanCoeff = 0.98f;
        runningFluxMean_ = kMeanCoeff * runningFluxMean_ + (1.0f - kMeanCoeff) * flux;

        // ── 3. Detect transient ───────────────────────────────────────────────
        const float threshold = kThreshold * (runningFluxMean_ + 1e-10f);
        if (flux > threshold)
        {
            // Transient detected: snap mix to 1 immediately
            transientMix = 1.0f;
        }
        else
        {
            // No transient: decay the hold envelope
            transientMix *= releaseCoeff;
        }

        // ── 4. Blend alpha toward nearest hard boundary ───────────────────────
        //   snapAlpha: round to 0 or 1 (whichever is closer to current alpha)
        const float snapAlpha = (alpha >= 0.5f) ? 1.0f : 0.0f;

        // Mix between raw alpha and hard-snapped alpha
        return alpha + transientMix * (snapAlpha - alpha);
    }

    /** Clear all history. Call from releaseResources or on flush. */
    void reset() noexcept
    {
        prevMag.fill(0.0f);
        transientMix    = 0.0f;
        runningFluxMean_ = 0.0f;
    }
};

} // namespace more_phi
