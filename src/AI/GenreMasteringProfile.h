/*
 * More-Phi — AI/GenreMasteringProfile.h
 *
 * Genre → adaptive mastering priors (Ozone §3.2 "Stage 1").
 *
 * The SonicMaster ONNX decision takes only the waveform as input — it cannot
 * condition on genre. This header is the message-thread bridge that feeds a
 * genre-derived target LUFS (and, in Phase 2, a tonal-balance curve) into the
 * decode path via the existing callerTargetLufs hook on decodeSonicMasterDecision.
 *
 * The 12 genre slots mirror GenreClassifier::kNumGenres. LUFS targets are
 * lifted verbatim from ChainPlanExecutor::kGenreLUFS[12] (the legacy rule-based
 * path's table). Each slot also pairs with a MasteringTargetCurve pointer so a
 * single lookup yields both halves of the Ozone "Stage 1" preset.
 *
 * ponytail: the LUFS values intentionally duplicate ChainPlanExecutor::kGenreLUFS
 * rather than #include it. That table is a private static on a class that pulls
 * in ChainPlanExecutor's full dependency graph; duplicating 12 floats with a
 * cross-reference comment is the shorter, lower-coupling diff. If you change one,
 * change both — the unit test pins them equal.
 */
#pragma once

#include "AI/GenreClassifier.h"          // kNumGenres, kGenreNames
#include "AI/MasteringTargetCurves.h"    // MasteringTargetCurve, findTargetCurve
#include "AI/SonicMasterDecisionDecoder.h"  // kUseModelTargetLufs

#include <array>
#include <cstddef>
#include <string_view>

namespace more_phi {

struct GenreMasteringProfile
{
    float targetLufs = kUseModelTargetLufs;                 // genre LUFS target
    const MasteringTargetCurve* targetCurve = nullptr;      // genre tonal-balance curve (may be null)
};

// ponytail: values mirror ChainPlanExecutor::kGenreLUFS[12] (ChainPlanExecutor.h:197).
// Order matches GenreClassifier::kGenreNames[12]. Index 10 = "Streaming (general)",
// which is the GenreClassifier's no-model default — so with no genre ONNX loaded
// the prior collapses to the Streaming profile (safe, non-destructive).
//
// Genre index → (target LUFS, target curve name). Curve names resolve against the
// 6 fixed archetypes in MasteringTargetCurves.h; unmapped genres get nullptr
// (the decode blend treats nullptr as "no curve prior").
inline constexpr std::array<GenreMasteringProfile, GenreClassifier::kNumGenres>
kGenreMasteringProfiles = []() consteval
{
    std::array<GenreMasteringProfile, GenreClassifier::kNumGenres> p {};
    // LUFS targets, byte-for-byte from ChainPlanExecutor::kGenreLUFS:
    constexpr std::array<float, GenreClassifier::kNumGenres> lufs = {
        -9.f, -9.f, -11.f, -13.f, -12.f, -16.f,
        -17.f, -20.f, -18.f, -10.f, -14.f, -23.f
    };
    // Closest-fit curve per slot (the 6 archetypes mapped onto 12 genres; the
    // rest stay null). Indices are positional into kGenreNames; if kGenreNames
    // changes order, re-audit this mapping.
    struct CurveMap { std::size_t idx; std::string_view name; };
    constexpr std::array<CurveMap, 5> curves = {{
        { 2, "electronic_dance" },  // EDM
        { 3, "hip_hop_rnb" },       // Hip-Hop/R&B
        { 4, "folk_acoustic" },     // Folk/Acoustic
        { 9, "neutral" },           // (closest neutral slot)
        { 10, "streaming" },        // Streaming (general) — the no-model default
    }};
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        p[i].targetLufs = lufs[i];
        for (const auto& c : curves)
            if (c.idx == i) { p[i].targetCurve = findTargetCurve(c.name); break; }
    }
    return p;
}();

// Lookup by genre index (0..kNumGenres-1). Out-of-range → default profile
// (Streaming target LUFS, no curve). noexcept, constexpr-friendly.
[[nodiscard]] inline GenreMasteringProfile getGenreMasteringProfile(int genreIndex) noexcept
{
    if (genreIndex < 0 || genreIndex >= GenreClassifier::kNumGenres)
        return kGenreMasteringProfiles[10];  // Streaming fallback
    return kGenreMasteringProfiles[genreIndex];
}

} // namespace more_phi
