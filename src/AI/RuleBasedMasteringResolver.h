/*
 * More-Phi — AI/RuleBasedMasteringResolver.h
 *
 * Deterministic "intelligent DSP" layer that turns live audio measurements into
 * a concrete MultiEffectPlan.  Requires no labeled data and no ML model.
 *
 * The resolver is the iZotope-style parameter-estimation step:
 *   audio analysis (spectrum, stereo, loudness, dynamics)
 *     + user intent (target LUFS, intensity, style curve)
 *     → EQ / dynamics / stereo / limiter parameters
 *
 * It is intentionally conservative: every output is clamped to safe ranges,
 * and it defers to an existing neural plan when one is active.
 */
#pragma once

#include "AI/ChainPlanExecutor.h"        // MultiEffectPlan
#include "AI/MasteringTargetCurves.h"    // Target curve library
#include "Core/RealtimeSpectrumAnalyzer.h"
#include "Core/StereoFieldAnalyzer.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace more_phi {

struct RuleBasedMasteringInput
{
    // Live measurements from AutoMasteringEngine's meters.
    RealtimeSpectrumAnalyzer::SpectrumSnapshot spectrum;
    StereoFieldAnalyzer::StereoFieldSnapshot stereo;

    float lufsIntegrated = 0.0f;   // BS.1770-4 integrated LUFS
    float lra = 0.0f;              // loudness range (LU)
    float truePeakDbTp = 0.0f;     // true-peak dBTP
    float crestFactor = 0.0f;      // program crest factor (peak/RMS)

    // User intent.
    float intensity = 0.5f;        // 0 = gentle, 1 = aggressive
    float targetLufs = -14.0f;     // desired integrated loudness
    std::string_view targetCurveName = "streaming"; // "neutral", "pink", etc.
};

class RuleBasedMasteringResolver
{
public:
    RuleBasedMasteringResolver() = default;

    /**
     * Build a complete mastering plan from live measurements + user intent.
     * Runs on the message thread.  No allocations.  All outputs are clamped.
     *
     * @param in  Snapshot of current audio measurements and user choices.
     * @return    A valid MultiEffectPlan ready for AutoMasteringEngine::applyPlan.
     */
    [[nodiscard]] MultiEffectPlan resolve(const RuleBasedMasteringInput& in) const noexcept;

    /**
     * Build only the EQ prescription JSON from the measured spectrum vs. target.
     * Useful for preview / MCP tools that want to inspect EQ without a full plan.
     */
    [[nodiscard]] juce::String buildEQPrescription(const RuleBasedMasteringInput& in) const noexcept;

    // Individual section builders (exposed for unit testing).
    [[nodiscard]] float resolveCompressionNeed(const RuleBasedMasteringInput& in) const noexcept;
    [[nodiscard]] juce::String resolveEQ(const RuleBasedMasteringInput& in) const noexcept;
    [[nodiscard]] std::array<float, 4> resolveWidthCurve(const RuleBasedMasteringInput& in) const noexcept;
    [[nodiscard]] float resolveCeilingDBTP(const RuleBasedMasteringInput& in) const noexcept;
    [[nodiscard]] float resolveTargetLUFS(const RuleBasedMasteringInput& in) const noexcept;
    [[nodiscard]] bool resolveExciterEnabled(const RuleBasedMasteringInput& in) const noexcept;

private:
    // Clamp helpers.
    [[nodiscard]] static float dbToNearestBin(float freqHz, double sampleRate, int fftSize, int binCount) noexcept;
    [[nodiscard]] static float readSpectrumDb(const RealtimeSpectrumAnalyzer::SpectrumSnapshot& spectrum,
                                              float freqHz) noexcept;
    [[nodiscard]] static float gainAtFrequency(const MasteringTargetCurve& curve, float freqHz) noexcept;

    // Safety constants.
    static constexpr float kMinTargetLUFS = -23.0f;
    static constexpr float kMaxTargetLUFS = -8.0f;
    static constexpr float kMinCeilingDBTP = -3.0f;
    static constexpr float kMaxCeilingDBTP = -0.1f;
    static constexpr float kStreamingSafeCeilingDBTP = -1.0f;
    static constexpr float kMaxEqGainDb = 6.0f;        // per-band correction limit (gentler than AdaptiveEQ max)
    static constexpr float kMaxEqCutDb = -6.0f;
    static constexpr float kMinCompressionNeed = 0.0f;
    static constexpr float kMaxCompressionNeed = 1.0f;

    // 8-band EQ frequencies (same as SonicMaster model for consistency).
    static constexpr std::array<float, kMasteringTargetCurveBandCount> kEqFreqs {
        60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f
    };

    // Per-band Q values for the generated EQ prescription.
    static constexpr std::array<float, kMasteringTargetCurveBandCount> kEqQs {
        0.7f, 1.2f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f, 0.7f
    };

    static constexpr std::array<const char*, kMasteringTargetCurveBandCount> kEqTypes {
        "lowshelf", "peak", "peak", "peak", "peak", "peak", "peak", "highshelf"
    };
};

} // namespace more_phi
