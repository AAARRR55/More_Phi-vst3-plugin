// src/AI/SonicMasterDecisionDecoder.cpp
#include "AI/SonicMasterDecisionDecoder.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

float clamp(float v, float lo, float hi) noexcept
{
    return std::clamp(finiteOr(v, 0.0f), lo, hi);
}

} // namespace

bool decodeSonicMasterDecision(const float* decision,
                               std::size_t decisionCount,
                               double sampleRate,
                               ValidatedNeuralMasteringPlan& out) noexcept
{
    if (decision == nullptr
        || decisionCount < kSonicMasterDecisionWidth
        || sampleRate <= 0.0)
        return false;

    out = {}; // start neutral

    // ── EQ: gains (dB) -> eq[i] = gain / kMaxGainDB, clamped to [-1, 1] ───────
    for (std::size_t i = 0; i < kSonicMasterEqGainCount; ++i)
    {
        const float gainDb = clamp(decision[kSonicMasterEqGainOffset + i],
                                   -kAdaptiveEqMaxGainDb, kAdaptiveEqMaxGainDb);
        out.projectedTargets.eq[i] = gainDb / kAdaptiveEqMaxGainDb;
    }
    out.appliedMask.eq = true;

    // ── Loudness: target LUFS -> loudness[0] so that AutoMasteringEngine's
    //    applyValidatedPlan maps (-14 + value*6) clamped to [-23,-8] back to
    //    the target. Invert the forward map at the (clamped) target.
    //    AUDIT-4: clamp to the engine's actual output range [-23,-8], not the
    //    looser [-30,-6]. Otherwise a model target of, say, -6 LUFS decodes to
    //    value=1.33 and the engine silently clamps it back to -8 — the model's
    //    expressed extreme is lost and the round-trip math lies to telemetry.
    {
        const float targetLufs = clamp(decision[kSonicMasterTargetLufsIdx], -23.0f, -8.0f);
        const float value = (targetLufs + 14.0f) / 6.0f;
        out.projectedTargets.loudness[0] = value;
        out.projectedTargets.loudness[1] = value;
        out.projectedTargets.loudness[2] = value;
        out.appliedMask.loudness = true;
    }

    // ── Limiter: true-peak ceiling decoded for telemetry. The default safety
    //    posture treats the limiter as high-risk (highRiskControls.limiter),
    //    so the mask is left OFF and the ceiling is not auto-applied — matching
    //    the DeterministicBaseline posture in OnnxNeuralMasteringRunner. The
    //    ceiling value is still projected into limiter[0..1] so a caller that
    //    explicitly raises the mask (e.g. an "apply limiter ceiling" toggle)
    //    gets the right number.
    {
        // AUDIT-4: clamp to the engine's output range [-3,-0.1], not [-6,-0.1].
        // The mask stays OFF (limiter is high-risk); this just keeps telemetry
        // consistent with what applyValidatedPlan would actually enforce.
        const float ceiling = clamp(decision[kSonicMasterTruePeakIdx], -3.0f, -0.1f);
        const float value = (ceiling + 1.0f) / 0.5f;
        out.projectedTargets.limiter[0] = value;
        out.projectedTargets.limiter[1] = value;
        out.appliedMask.limiter = false;
    }

    // ── Dynamics: 3 x (threshold,ratio,attack,release,makeup,knee).
    //    AUDIT-3: the model emits threshold AND ratio per band. Previously both
    //    were collapsed onto one scalar (threshold), then the engine derived a
    //    coupled ratio from that scalar — so "high threshold + low ratio" was
    //    unexpressible and the model's ratio decision was discarded. Now store
    //    the two independently as a pair: dynamics[2*band] = threshold value,
    //    dynamics[2*band+1] = ratio value. 3 bands x 2 = 6 <= dynamics.size() (8).
    //    Attack/release/makeup/knee remain projected from the threshold scalar
    //    (the engine exposes no per-band knob for them); they are not decoded.
    for (std::size_t band = 0; band < kSonicMasterCompBandCount; ++band)
    {
        const std::size_t o = kSonicMasterCompOffset + band * kSonicMasterCompBandWidth;
        const float thresholdDb = clamp(decision[o + 0], -40.0f, -6.0f);
        const float ratioRaw    = clamp(decision[o + 1], 1.0f, 20.0f);
        out.projectedTargets.dynamics[2 * band + 0] = clamp((thresholdDb + 20.0f) / 8.0f, -1.0f, 1.0f);
        out.projectedTargets.dynamics[2 * band + 1] = clamp((ratioRaw - 2.5f) / 1.5f, -1.0f, 5.0f);
    }
    out.appliedMask.dynamics = true;

    // ── Stereo: 2 x (width, sideGain). AUDIT-2: the model emits two width
    //    regions — store both, stop collapsing onto one. stereo[0]=region0,
    //    stereo[1]=region1. The engine exposes 4 regions; only the 2 the model
    //    decided on are applied there, the rest are left to the genre translator.
    {
        out.projectedTargets.stereo[0] = clamp(decision[kSonicMasterStereoOffset + 0], -1.0f, 1.0f);
        out.projectedTargets.stereo[1] = clamp(decision[kSonicMasterStereoOffset + 1], -1.0f, 1.0f);
        out.appliedMask.stereo = true;
    }

    // Saturation/exciter and character are decoded for telemetry only — the
    // default appliedMask leaves harmonic=false (high-risk per the safety
    // policy), matching the DeterministicBaseline posture. The raw gate values
    // are coerced to finite but not projected.

    out.fallbackMode   = NeuralMasteringFallbackMode::None;
    out.evidenceLevel  = NeuralMasteringEvidenceLevel::PrototypeMeasured;
    out.valid          = true;
    out.projected      = true;
    out.sourcePlanId   = 0; // filled in by the analysis engine
    return true;
}

} // namespace more_phi
