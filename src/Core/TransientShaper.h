/*
 * More-Phi — Core/TransientShaper.h
 *
 * Single-band transient/impact shaper for the native mastering chain. The one
 * Ozone-style module (doc "Impact") that had no native counterpart — only a
 * morph-side TransientDetector existed. This fills that gap minimally.
 *
 * Algorithm: two RMS envelope followers per channel — a FAST one (short attack/
 * release, tracks transient peaks) and a SLOW one (long release, tracks sustain
 * body). Per sample, the gain is:
 *
 *   g = clamp(1 + amount * (envFast / envSlow − 1), gMin, gMax)
 *
 * amount > 0 → transient emphasis (attacks louder vs sustain), Ozone "Impact"
 * up. amount < 0 → sustain emphasis (smooth/transient reduction). amount 0 →
 * unity (bypass). Operates on the full-band buffer in place.
 *
 * ponytail: single-band, not per-band under MultibandSplitter. Per-band is the
 * upgrade path if a profile needs frequency-selective transient work; one band
 * covers the Impact use case for v1.
 *
 * Real-time: noexcept, no allocations after prepare(). Two EnvelopeFollower
 * pairs (fast/slow) per channel, state pre-allocated in prepare().
 *
 * NOTE: the native chain in AutoMasteringEngine is dormant today
 * (intelligenceActive_=false in the shipped plugin); this module only runs if/
 * when intelligence is enabled. Dropping it is one chain-stage removal.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class TransientShaper
{
public:
    static constexpr int kMaxChannels = 2;

    TransientShaper();

    // ── Lifecycle (message thread) ─────────────────────────────────────────────
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;

    // ── Audio thread ───────────────────────────────────────────────────────────
    // Processes the buffer in place. noexcept, no allocations.
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

    // ── Parameters (atomic, any thread) ────────────────────────────────────────
    // amount in [-1, +1]: +1 = max transient emphasis, -1 = max sustain, 0 = unity.
    void setAmount(float amount) noexcept
    { amount_.store(std::clamp(amount, -1.0f, 1.0f), std::memory_order_relaxed); }
    [[nodiscard]] float getAmount() const noexcept { return amount_.load(std::memory_order_relaxed); }

    // Output gain in dB [-12, +12], applied after shaping.
    void setOutputGainDb(float dB) noexcept
    { outputGain_.store(std::pow(10.0f, dB / 20.0f), std::memory_order_relaxed); }
    [[nodiscard]] float getOutputGainDb() const noexcept
    { return 20.0f * std::log10(outputGain_.load(std::memory_order_relaxed)); }

    // Enable/disable (amount=0 also effectively bypasses, but this skips the work).
    void setEnabled(bool e) noexcept { enabled_.store(e, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

private:
    // The envelope math runs inline in processBlock (two one-pole followers per
    // channel) so we avoid the per-sample virtual-call overhead of two
    // EnvelopeFollower objects. State is per-channel fast/slow squared-envelope.
    struct ChannelState
    {
        float fastEnv = 0.0f;   // short-time mean-square (transient tracker)
        float slowEnv = 0.0f;   // long-time mean-square (sustain tracker)
    };

    std::array<ChannelState, kMaxChannels> channels_ {};
    double sampleRate_ = 48000.0;

    // One-pole coefficients: aFast responds ~3 ms, aSlow ~150 ms (sustain body).
    // Pre-computed in prepare() so processBlock does no transcendental math.
    float fastCoeff_ = 0.0f;
    float slowCoeff_ = 0.0f;

    std::atomic<float> amount_      { 0.0f };
    std::atomic<float> outputGain_  { 1.0f };
    std::atomic<bool>  enabled_     { false };

    // Hard limits on the shaping gain so a noisy frame can't explode.
    static constexpr float kGainMin = 0.1f;   // -20 dB
    static constexpr float kGainMax = 4.0f;   // +12 dB
    // Floor for the slow envelope to avoid divide-by-near-zero on silence.
    static constexpr float kEnvFloor = 1e-10f;
};

} // namespace more_phi
