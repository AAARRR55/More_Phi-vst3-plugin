// src/AI/SonicMasterDecisionDecoder.cpp
#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/MasteringTargetCurves.h"   // MasteringTargetCurve full def + gainAt

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
                               ValidatedNeuralMasteringPlan& out,
                               float callerTargetLufs,
                               GenreEqPrior eqPrior) noexcept
{
    if (decision == nullptr
        || decisionCount < kSonicMasterDecisionWidth
        || sampleRate <= 0.0)
        return false;

    out = {}; // start neutral

    // ── EQ: gains (dB) -> eq[i] = gain / kMaxGainDB, clamped to [-1, 1] ───────
    // GENRE PRIOR (Stage 2): when a target curve + measured tonal balance are
    // supplied, blend a bounded residual correction into each band before the
    // ±12 dB clamp. residual = (targetGainAtFreq − measuredBandDb), pre-clamped
    // to ±6 dB so one band can't dominate, scaled by residualBlend. Level-
    // invariant because measuredBandDb is already mean-subtracted (see
    // TonalBalanceExtractor) and the curve is relative to 1 kHz neutral.
    const bool useEqPrior = eqPrior.curve != nullptr
                            && eqPrior.measuredBandDb != nullptr
                            && eqPrior.residualBlend > 0.0f;
    const float blend = useEqPrior ? std::clamp(eqPrior.residualBlend, 0.0f, 1.0f) : 0.0f;
    for (std::size_t i = 0; i < kSonicMasterEqGainCount; ++i)
    {
        float gainDb = clamp(decision[kSonicMasterEqGainOffset + i],
                             -kAdaptiveEqMaxGainDb, kAdaptiveEqMaxGainDb);
        if (useEqPrior)
        {
            const float targetDb = eqPrior.curve->gainsDb[i]
                                 + eqPrior.curve->gainAt(kSonicMasterEqFrequenciesHz[i]);
            const float measuredDb = finiteOr(eqPrior.measuredBandDb[i], 0.0f);
            const float residual = std::clamp(targetDb - measuredDb, -6.0f, 6.0f) * blend;
            gainDb = std::clamp(gainDb + residual,
                                -kAdaptiveEqMaxGainDb, kAdaptiveEqMaxGainDb);
        }
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
        // Stage A (2026-06-26): a caller may override the model's loudness
        // recommendation (e.g. a profile target like Streaming -14 LUFS, or a
        // closed-loop correction from Stage D). The ONNX graph can't condition
        // on a target during inference, so this is the decode-side hook that
        // makes an explicit target actually reach the apply. Default
        // (kUseModelTargetLufs) honors the model's own recommendation.
        const bool useCaller = std::isfinite(callerTargetLufs)
                               && callerTargetLufs != kUseModelTargetLufs;
        const float modelLufs = clamp(decision[kSonicMasterTargetLufsIdx], -23.0f, -8.0f);
        const float targetLufs = useCaller ? clamp(callerTargetLufs, -23.0f, -8.0f) : modelLufs;
        const float value = (targetLufs + 14.0f) / 6.0f;
        out.projectedTargets.loudness[0] = value;
        out.projectedTargets.loudness[1] = value;
        out.projectedTargets.loudness[2] = value;
        out.appliedMask.loudness = true;
        // AUDIT-FIX (H2): the SonicMaster input is peak-normalized before
        // inference, so the model cannot measure absolute input LUFS. This
        // loudness slot is the mastering TARGET the caller/decoder supplied,
        // not a measurement. Lock the semantic flag so no downstream consumer
        // can read it as an input-loudness measurement by accident.
        out.loudnessIsMeasurement = false;
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

    // AUDIT-FIX-3: decoder and engine must clamp ratio to the SAME range or
    // telemetry lies by up to 3.3x. applyValidatedPlan re-clamps via the same
    // kSonicMasterCompRatioMax constant (AutoMasteringEngine.cpp). The model may
    // emit up to 20, but the DSP cannot honour it; clamping at decode time keeps
    // the decoded plan byte-for-byte consistent with what reaches the compressor.
    // Upgrade path: raise both bounds together once MultibandDynamicsProcessor is
    // verified to support ratio > 6.

    // ── Dynamics: 3 x (threshold,ratio,attack,release,makeup,knee).
    //    AUDIT-2.1: the model emits all six params per band. Decode them into the
    //    compParams sidecar in real units (the full set), AND mirror threshold +
    //    ratio into the normalized projectedTargets.dynamics array so the safety
    //    policy's [-1,1] delta validation still runs. Attack/release/makeup/knee
    //    travel ONLY in the sidecar — applyValidatedPlan reads them when
    //    hasCompParams is set, otherwise falls back to the threshold/ratio pair
    //    and the DSP's existing attack/release/makeup/knee values.
    //    AUDIT-FIX (L1-6, 2026-06-29): replaced magic numbers with named
    //    constants from CompNorm (NeuralMasteringTypes.h) so normalization
    //    parameters are auditable in one place.
    for (std::size_t band = 0; band < kSonicMasterCompBandCount; ++band)
    {
        const std::size_t o = kSonicMasterCompOffset + band * kSonicMasterCompBandWidth;
        const float thresholdDb = clamp(decision[o + 0], CompNorm::kThresholdMinDb, CompNorm::kThresholdMaxDb);
        const float ratioRaw    = clamp(decision[o + 1], kSonicMasterCompRatioMin, kSonicMasterCompRatioMax);
        const float attackMs    = clamp(decision[o + 2], CompNorm::kAttackMinMs, CompNorm::kAttackMaxMs);
        const float releaseMs   = clamp(decision[o + 3], CompNorm::kReleaseMinMs, CompNorm::kReleaseMaxMs);
        const float makeupDb    = clamp(decision[o + 4], CompNorm::kMakeupMinDb, CompNorm::kMakeupMaxDb);
        const float kneeDb      = clamp(decision[o + 5], CompNorm::kKneeMinDb, CompNorm::kKneeMaxDb);

            // Normalized pair for the safety policy's delta math. The
            // normalization is (value - center) / halfRange → [-1, 1].
            // Ratio: center=3.5, halfRange=2.5; threshold: center=-20, halfRange=8.
            out.projectedTargets.dynamics[2 * band + 0] = clamp(
                (thresholdDb - CompNorm::kThresholdCenterDb) / CompNorm::kThresholdHalfRangeDb,
                -1.0f, 1.0f);
            out.projectedTargets.dynamics[2 * band + 1] = clamp(
                (ratioRaw - CompNorm::kRatioCenter) / CompNorm::kRatioHalfRange,
                -1.0f, 1.0f);

        // Full real-unit sidecar for the DSP.
        auto& cp = out.compParams[band];
        cp.thresholdDb = thresholdDb;
        cp.ratio       = ratioRaw;
        cp.attackMs    = attackMs;
        cp.releaseMs   = releaseMs;
        cp.makeupDb    = makeupDb;
        cp.kneeDb      = kneeDb;
    }
    out.hasCompParams     = true;
    out.appliedMask.dynamics = true;

	    // ── Stereo: 2 x (width, sideGain). AUDIT-2: the model emits two width
	    //    regions — store both. AUDIT-FIX (P7, 2026-06-27): extend the second
	    //    model region (region 1, typically Mid band) to regions 2-3 (Mid-High
	    //    and High) so the internal chain's 4-region stereo imager receives
	    //    consistent width across all bands rather than leaving the upper bands
	    //    at their heuristic defaults.
	    {
	        out.projectedTargets.stereo[0] = clamp(decision[kSonicMasterStereoOffset + 0], -1.0f, 1.0f);
	        out.projectedTargets.stereo[1] = clamp(decision[kSonicMasterStereoOffset + 1], -1.0f, 1.0f);
	        out.projectedTargets.stereo[2] = out.projectedTargets.stereo[1]; // extend region 1 → region 2
	        out.projectedTargets.stereo[3] = out.projectedTargets.stereo[1]; // extend region 1 → region 3
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
