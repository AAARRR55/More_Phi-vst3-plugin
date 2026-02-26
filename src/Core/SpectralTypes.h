/*
 * MorphSnap — Core/SpectralTypes.h
 * Shared type definitions for spectral and granular processing modes.
 *
 * These are pure configuration structs — no heap allocation, no JUCE types.
 * They are safe to copy on any thread and to embed in pre-allocated objects
 * used by the audio thread.
 */
#pragma once

namespace morphsnap {

// ---------------------------------------------------------------------------
// FFT size
// ---------------------------------------------------------------------------

/**
 * Supported FFT window sizes for spectral processing.
 *
 * Larger sizes give better frequency resolution at the cost of higher
 * latency (latency = fftSize / 2 samples for a 50% overlap scheme).
 *
 * Thread safety: read-only enum, safe everywhere.
 */
enum class FFTSize : int
{
    FFT_512  = 512,
    FFT_1024 = 1024,
    FFT_2048 = 2048,
    FFT_4096 = 4096
};

// ---------------------------------------------------------------------------
// Morph domain
// ---------------------------------------------------------------------------

/**
 * Selects which morphing domain(s) are active.
 *
 *   Parameter  — classical parameter-value interpolation (V1 behaviour)
 *   Spectral   — STFT phase-vocoder crossfade between audio streams
 *   Granular   — grain-cloud crossfade between audio streams
 *   Hybrid     — weighted blend of two or more domains (see HybridBlendWeights)
 *
 * Thread safety: read-only enum, safe everywhere.
 */
enum class MorphDomain : int
{
    Parameter = 0,
    Spectral,
    Granular,
    Hybrid
};

// ---------------------------------------------------------------------------
// Spectral (STFT) configuration
// ---------------------------------------------------------------------------

/**
 * Configuration for the STFT-based spectral morph engine.
 *
 * hopSize is normally fftSize / (1 - overlapRatio). The default values
 * (FFT_2048, hop 512, overlap 0.75) give 75% overlap which is standard
 * for high-quality phase-vocoder operation.
 *
 * transientPreserve: detect and restore transient peaks to reduce
 * phasiness on percussive material. May add one additional analysis frame
 * of latency.
 *
 * formantPreserve: apply true-envelope normalisation before crossfade
 * and re-apply after, preserving the formant structure of vocals/speech
 * regardless of the spectral interpolation ratio.
 *
 * Thread safety: SpectralConfig is a plain-old-data struct. It is safe to
 * copy on any thread. The engine reads it only at prepare() time except
 * for transientPreserve and formantPreserve which use atomics internally.
 */
struct SpectralConfig
{
    FFTSize fftSize         = FFTSize::FFT_2048;
    int     hopSize         = 512;         // fftSize / 4 at default overlap
    float   overlapRatio    = 0.75f;       // 0 < overlapRatio < 1
    bool    transientPreserve = true;
    bool    formantPreserve   = false;
};

// ---------------------------------------------------------------------------
// Granular configuration
// ---------------------------------------------------------------------------

/**
 * Configuration for the granular synthesis morph engine.
 *
 * Grain size and density interact: a large grain with high density will
 * produce thick, smeared textures; a small grain with low density gives
 * a more sparse, scattered result.
 *
 * pitchRandomization is measured in semitones and applied per-grain.
 * positionRandomization is a fraction of the read-pointer window; 0 gives
 * synchronous grain emission (no scatter), 1 fully randomises read position.
 *
 * maxGrains caps the simultaneously active grain count to bound CPU usage.
 *
 * Thread safety: GranularConfig is a plain-old-data struct, safe to copy
 * on any thread.
 */
struct GranularConfig
{
    float grainSizeMs           = 50.0f;   // [20, 200] ms
    float grainDensity          = 20.0f;   // grains per second
    float pitchRandomization    = 0.0f;    // [0, 1] semitones
    float positionRandomization = 0.0f;    // [0, 1] fraction of buffer window
    int   maxGrains             = 128;
};

// ---------------------------------------------------------------------------
// Hybrid blend weights
// ---------------------------------------------------------------------------

/**
 * Per-domain blend weights for MorphDomain::Hybrid.
 *
 * The three weights must sum to 1.0; call normalize() after modifying
 * any weight to enforce this invariant. If all three are 0.0, normalize()
 * falls back to 100% parameter morphing.
 *
 * Thread safety: HybridBlendWeights is a plain-old-data struct. normalize()
 * is noexcept — safe to call on the audio thread if the struct is
 * pre-allocated.
 */
struct HybridBlendWeights
{
    float parameter = 1.0f;  // Weight for parameter-domain morph
    float spectral  = 0.0f;  // Weight for spectral morph
    float granular  = 0.0f;  // Weight for granular morph

    /**
     * Normalises the three weights so they sum to 1.0.
     * Falls back to pure parameter morphing if all weights are zero.
     *
     * noexcept: Pure arithmetic on primitives, no allocations.
     */
    void normalize() noexcept
    {
        float sum = parameter + spectral + granular;
        if (sum > 0.0f)
        {
            parameter /= sum;
            spectral  /= sum;
            granular  /= sum;
        }
        else
        {
            parameter = 1.0f;
            spectral  = 0.0f;
            granular  = 0.0f;
        }
    }
};

} // namespace morphsnap
