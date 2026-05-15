/*
 * More-Phi — Core/LoudnessNormalizer.h
 *
 * Post-limiter integrated LUFS normalizer.
 *
 * The LoudnessNormalizer reads the integrated LUFS measurement from a
 * shared LUFSMeter, computes a correction gain to reach the target LUFS,
 * clamps it to a safe range, and applies it as a slow-ramping gain to
 * the audio buffer.
 *
 * The correction gain is updated on the message thread (or via a timer)
 * at ~10 Hz (every 100 ms).  The audio thread reads the current gain via
 * a single std::atomic<float> — zero latency, zero locks.
 *
 * Safety:
 *   - Correction clamped to [-12 dB, +6 dB] to prevent wild jumps.
 *   - Gain ramp: one-pole smoothing τ ≈ 1 s (prevents clicks on transitions).
 *   - Must NOT be enabled until at least 3 s of integrated LUFS is valid.
 *
 * Thread safety:
 *   processBlock() — audio thread only, noexcept.
 *   updateCorrectionGain() — call from message thread / timer callback.
 *   setTarget*() — atomic, any thread.
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include "LUFSMeter.h"

namespace more_phi {

class LoudnessNormalizer
{
public:
    static constexpr float kDefaultTargetLUFS = -14.0f;  // Spotify / Apple Music
    static constexpr float kMinCorrectionDB   = -12.0f;
    static constexpr float kMaxCorrectionDB   =   6.0f;

    LoudnessNormalizer();

    // ── Configuration (any thread — atomic) ───────────────────────────────────

    /** Set the target integrated LUFS. Common values: -9, -14, -16, -23. */
    void setTargetLUFS(float lufs) noexcept { targetLUFS_.store(lufs, std::memory_order_relaxed); }

    /** Enable / disable loudness normalization. When disabled, gain = 0 dB. */
    void setEnabled(bool enabled) noexcept  { enabled_.store(enabled, std::memory_order_relaxed); }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Configure the normalizer.
     * @param meter   Reference to the LUFSMeter measuring the chain output.
     *                Must outlive this normalizer.
     */
    void prepare(double sampleRate, LUFSMeter& meter) noexcept;

    /** Reset gain ramp to 1.0 (0 dB). */
    void reset() noexcept;

    // ── Message thread: update correction gain ────────────────────────────────

    /**
     * Compute correction gain from the meter's current integrated LUFS.
     * Call from a timer callback at ~10 Hz (every 100 ms).
     * Thread-safe via atomic store — audio thread reads atomically.
     */
    void updateCorrectionGain() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Apply normalization gain (with one-pole ramp) to buf in-place.
     * noexcept — reads one atomic<float>, applies ramp, multiplies buffer.
     */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

    // ── Metering (any thread) ─────────────────────────────────────────────────

    [[nodiscard]] float getCurrentCorrectionDB() const noexcept
    {
        return correctionDB_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float getTargetLUFS() const noexcept
    {
        return targetLUFS_.load(std::memory_order_relaxed);
    }

private:
    LUFSMeter* meter_ = nullptr;

    std::atomic<float> targetLUFS_    { kDefaultTargetLUFS };
    std::atomic<bool>  enabled_       { true };
    std::atomic<float> correctionDB_  { 0.0f };         // dB correction (message thread writes)
    std::atomic<float> correctionGain_{ 1.0f };         // linear (message thread writes, audio reads)

    // Audio-thread ramp state (audio thread only — no atomic needed)
    float gainRamped_  = 1.0f;  // current applied gain
    float rampCoeff_   = 0.0f;  // one-pole: τ ≈ 1 s

    double sampleRate_ = 48000.0;
};

} // namespace more_phi
