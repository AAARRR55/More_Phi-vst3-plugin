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
    {
        const float targetLufs = clamp(decision[kSonicMasterTargetLufsIdx], -30.0f, -6.0f);
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
        const float ceiling = clamp(decision[kSonicMasterTruePeakIdx], -6.0f, -0.1f);
        const float value = (ceiling + 1.0f) / 0.5f;
        out.projectedTargets.limiter[0] = value;
        out.projectedTargets.limiter[1] = value;
        out.appliedMask.limiter = false;
    }

    // ── Dynamics: 3 x (threshold,ratio,attack,release,makeup,knee).
    //    applyValidatedPlan maps dynamics[band] -> threshold = -20 + v*8,
    //    ratio = 2.5 + v*1.5. Encode the band's threshold via the inverted
    //    forward map, clamped to [-1, 1] so the safety policy's delta range
    //    holds. (Ratio/attack/release/makeup/knee are projected through the
    //    same single scalar per band the engine uses.)
    for (std::size_t band = 0; band < kSonicMasterCompBandCount; ++band)
    {
        const std::size_t o = kSonicMasterCompOffset + band * kSonicMasterCompBandWidth;
        const float thresholdDb = clamp(decision[o + 0], -40.0f, -6.0f);
        const float v = (thresholdDb + 20.0f) / 8.0f;
        out.projectedTargets.dynamics[band] = clamp(v, -1.0f, 1.0f);
    }
    out.appliedMask.dynamics = true;

    // ── Stereo: 2 x (width, sideGain). applyValidatedPlan maps width_out =
    //    1 + stereo[region], clamped [0,2]. Decode decision[35] (width band 0)
    //    directly as the offset for the regions the engine exposes.
    {
        const float width0 = clamp(decision[kSonicMasterStereoOffset + 0], -1.0f, 1.0f);
        out.projectedTargets.stereo[0] = width0;
        out.projectedTargets.stereo[1] = width0;
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
