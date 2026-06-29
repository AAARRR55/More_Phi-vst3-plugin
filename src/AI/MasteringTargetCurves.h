/*
 * More-Phi — AI/MasteringTargetCurves.h
 *
 * Fixed target spectral curves for the rule-based mastering resolver.
 *
 * Curves are stored as 8-band gain offsets aligned to
 * kSonicMasterEqFrequenciesHz (60/120/250/500/1k/2.5k/5k/10k Hz).  A curve
 * represents the *desired* dB response at those frequencies relative to a
 * flat 0 dB reference.  The resolver measures the incoming spectrum, subtracts
 * the chosen target, and the residual becomes the EQ correction (after clamps).
 *
 * No training data is required; these are hard-coded archetypes matching the
 * existing EQParameterTranslator genre warm-starts.
 */
#pragma once

#include "AI/SonicMasterDecisionDecoder.h"  // kSonicMasterEqFrequenciesHz / kSonicMasterEqGainCount
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace more_phi {

inline constexpr std::size_t kMasteringTargetCurveBandCount = kSonicMasterEqGainCount;

struct MasteringTargetCurve
{
    std::string_view name;
    // Gain in dB at each of the 8 SonicMaster EQ frequencies.
    std::array<float, kMasteringTargetCurveBandCount> gainsDb {};
    // Approximate slope in dB/octave applied on top of the per-band gains.
    float slopeDbPerOctave = 0.0f;

    // Total target gain (per-band table + slope contribution) at a frequency,
    // relative to the 1 kHz neutral point. Mirrors
    // RuleBasedMasteringResolver::gainAtFrequency + the per-band gain so a
    // header-only caller (the SonicMaster decoder) evaluates the curve without
    // linking the resolver. Non-constexpr (uses log2); the constexpr curve
    // table above is unaffected.
    // ponytail: same formula as the resolver static; duplicated ~3 lines so the
    // decoder stays header-only across the AI/Core boundary.
    [[nodiscard]] float gainAt(float freqHz) const noexcept
    {
        if (freqHz <= 20.0f) return 0.0f;
        const float octaves = std::log2(freqHz / 1000.0f);
        return slopeDbPerOctave * octaves;
    }
};

namespace target_curves {

// Flat / transparent: no intentional tonal tilt.
inline constexpr MasteringTargetCurve kNeutral {
    "neutral",
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    0.0f
};

// Pink-noise reference: -3 dB/octave high-frequency roll-off.
// Useful when the source is extremely bright and needs taming by default.
inline constexpr MasteringTargetCurve kPink {
    "pink",
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    -3.0f
};

// EDM / electronic: lifted subs, scooped mids, airy highs.
inline constexpr MasteringTargetCurve kElectronicDance {
    "electronic_dance",
    { 2.0f, 1.5f, -1.0f, -0.5f, 0.5f, 1.0f, 1.5f, 0.5f },
    0.0f
};

// Hip-hop / R&B: heavier sub-bass boost, mid scoop, crisp highs.
inline constexpr MasteringTargetCurve kHipHopRnB {
    "hip_hop_rnb",
    { 3.0f, 2.0f, -1.5f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f },
    0.0f
};

// Folk / acoustic: gentle low cut, forward mids, restrained highs.
inline constexpr MasteringTargetCurve kFolkAcoustic {
    "folk_acoustic",
    { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.0f, 0.5f, 0.0f },
    0.0f
};

// Streaming / safe default: subtle low-end warmth, slight high lift, conservative.
inline constexpr MasteringTargetCurve kStreaming {
    "streaming",
    { 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f },
    0.0f
};

} // namespace target_curves

// All selectable curves in a constexpr array for iteration / lookup.
inline constexpr std::array<MasteringTargetCurve, 6> kMasteringTargetCurves {
    target_curves::kNeutral,
    target_curves::kPink,
    target_curves::kElectronicDance,
    target_curves::kHipHopRnB,
    target_curves::kFolkAcoustic,
    target_curves::kStreaming
};

// Lookup by name; returns nullptr if not found.
[[nodiscard]] constexpr const MasteringTargetCurve* findTargetCurve(std::string_view name) noexcept
{
    for (const auto& c : kMasteringTargetCurves)
        if (c.name == name)
            return &c;
    return nullptr;
}

} // namespace more_phi
