// src/AI/SonicMasterDecisionDecoder.h
#pragma once

#include "Core/NeuralMasteringTypes.h"

#include <cstddef>

namespace more_phi {

// Forward decl: MasteringTargetCurves.h includes this header (for the EQ freq
// constants), so we can't include it back here. The decoder only needs a
// pointer to the curve, so a forward declaration breaks the cycle.
struct MasteringTargetCurve;
// FULL_CHAIN_REGRESSION_WIDTH in mastering_decision_adapter.py (= num_eq_bands
// + 2 + 1 + 18 + 1 + 4 + 1 + 4 + 1 + 1 + character_logits).
inline constexpr std::size_t kSonicMasterDecisionWidth = 44;

// Slice offsets into the 44-float decision vector. Copied verbatim from
// mastering_decision_adapter.decode_mastering_decision() so C++ and Python
// agree byte-for-byte. Do not reorder without re-exporting the model.
inline constexpr std::size_t kSonicMasterEqGainCount   = 8;
inline constexpr std::size_t kSonicMasterEqGainOffset  = 0;
inline constexpr std::size_t kSonicMasterTargetLufsIdx = 8;
inline constexpr std::size_t kSonicMasterTruePeakIdx   = 9;
inline constexpr std::size_t kSonicMasterCompGateIdx   = 10;
inline constexpr std::size_t kSonicMasterCompOffset    = 11; // 18 floats: 3 x (thr,ratio,atk,rel,makeup,knee)
inline constexpr std::size_t kSonicMasterCompBandCount = 3;
inline constexpr std::size_t kSonicMasterCompBandWidth = 6;

// AUDIT-FIX-3: the agreed compressor-ratio range, shared by the decoder
// (SonicMasterDecisionDecoder.cpp) and the engine (AutoMasteringEngine.cpp
// applyValidatedPlan). Both must clamp to the same range or the decoded plan
// and the applied DSP disagree by up to 3.3x. Raise BOTH together (and verify
// MultibandDynamicsProcessor supports the wider range) before widening.
inline constexpr float kSonicMasterCompRatioMin = 1.0f;
	// AUDIT-FIX (A6, P5 2026-06-27): tightened from 6.0 to 4.0 in the original A6
	// fix so the safety policy's normalized [-1, +1] dynamics bound could represent
	// every value. Widened back to 6.0 in P5 after confirming MultibandDynamicsProcessor
	// supports any positive ratio with no internal clamp. The safety policy's
	// maxDeltaPerPlan.dynamics was also widened (0.12 -> 0.20) to accommodate
	// the wider range. Raise ALL THREE bounds together before widening further.
	inline constexpr float kSonicMasterCompRatioMax = 6.0f;

// AUDIT-2/3: the model emits these counts; AutoMasteringEngine applies ONLY
// these many bands from a SonicMaster plan and leaves the rest to the genre
// translator / heuristic warm-start. (The engine's own DSP modules expose more
// bands — 32 EQ, 4 dynamics, 4 stereo regions — but the model decides fewer.)
inline constexpr std::size_t kSonicMasterStereoRegionCount = 2;  // decision[35..36] = width region 0,1

// AUDIT-3: dynamics slot layout in projectedTargets.dynamics.
//   dynamics[2*band + 0] = threshold value (forward map: -20 + v*8 dB)
//   dynamics[2*band + 1] = ratio value     (forward map:  2.5 + v*1.5)
inline constexpr std::size_t kSonicMasterDynamicsSlotsPerBand = 2;
inline constexpr std::size_t kSonicMasterExciterGateIdx = 29;
inline constexpr std::size_t kSonicMasterSatOffset     = 30; // 4 floats: 2 x (drive,mix)
inline constexpr std::size_t kSonicMasterStereoGateIdx = 34;
inline constexpr std::size_t kSonicMasterStereoOffset  = 35; // 4 floats: 2 x (width,sideGain)
inline constexpr std::size_t kSonicMasterAggrIdx       = 39;
inline constexpr std::size_t kSonicMasterGainRedIdx    = 40;
inline constexpr std::size_t kSonicMasterCharOffset    = 41; // 3 logits -> transparent/balanced/aggressive
inline constexpr std::size_t kSonicMasterCharCount     = 3;

// EQ band centre frequencies used by the model (EQ_DECISION_FREQUENCIES_HZ).
// Bands 0..7 of AdaptiveEQ receive these freqs; gains come from the decision.
inline constexpr float kSonicMasterEqFrequenciesHz[kSonicMasterEqGainCount] = {
    60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f
};

	// Default neutral Q for the decoded EQ bands (matches EQ_DECISION_Q).
	// AUDIT-FIX (P6, 2026-06-27): Q and filter type are FIXED per-band because
	// the 44-float decision vector carries only gain per band (no Q, no type).
	// Q=0.707 is a reasonable broadband mastering EQ default (gentle enough
	// for surgical use, not so wide it rings). A future model export that adds
	// Q and type slots per band would make these per-band configurable.
	// Until then, all 8 bands are bell/peak filters at Q=0.707.
	inline constexpr float kSonicMasterEqDefaultQ = 0.707f;

// Mirrors AdaptiveEQ::kMaxGainDB (Core/AdaptiveEQ.h). Re-stated here so the
// decoder stays DSP-header-free and the tests can assert against it without
// pulling AdaptiveEQ.h. The test in Task 2 pins the value to 12.0.
inline constexpr float kAdaptiveEqMaxGainDb = 12.0f;

// Confidence attached to every decoded plan. Above the safety policy's
// default minConfidence (0.75) so plausible frames clear the gate, below 1.0
// to leave headroom for the safety re-clamp. See design §5.2.
inline constexpr float kSonicMasterDefaultConfidence = 0.85f;

/**
 * Decode a 44-float masteringbrainv2 decision into a ValidatedNeuralMasteringPlan
 * whose projectedTargets / appliedMask are consumed directly by
 * AutoMasteringEngine::applyValidatedPlan(). Pure, noexcept, allocation-free,
 * ONNX-free. Non-finite inputs are coerced; every target is clamped into the
 * range the receiving DSP module expects so the safety policy is a second line,
 * not the first.
 *
 * On success returns true and fills `out`. Returns false only if `decision` is
 * null or `sampleRate <= 0` (the caller treats false as "skip this cycle").
 *
 * Stage A (2026-06-26, Ozone-like apply): an optional @p callerTargetLufs
 * overrides the model's decoded loudness TARGET. The ONNX graph takes only the
 * waveform (1 input) and cannot condition on a target during inference, so the
 * model's loudness slot is a recommendation, not a measurement. When a caller
 * passes a finite target_lufs (e.g. from a profile like Streaming/CD, or a
 * closed-loop correction), the decoded loudness[0..2] is recomputed from that
 * value (same clamp + inverse map as the model path) and the apply honors it.
 * Pass kUseModelTargetLufs (the default) to use the model's own recommendation
 * — "apply the recommendation" semantics.
 */
inline constexpr float kUseModelTargetLufs = -1000.0f; // sentinel: outside any real LUFS range

// GENRE PRIOR (Stage 2, Ozone §3.2 tonal-balance matching): the SonicMaster
// model emits an EQ recommendation blind to a target curve. When a caller
// supplies a MasteringTargetCurve AND the measured per-band tonal balance, the
// decoder blends a bounded residual correction into each EQ band BEFORE the
// existing ±12 dB clamp. The residual is the level-invariant shape gap
// (target − measured), scaled by residualBlend in [0,1] and pre-clamped to ±6 dB
// so a single band can never dominate. Default args (nullptr / 0) preserve the
// original decode byte-for-byte — the residual is opt-in.
struct GenreEqPrior
{
    const MasteringTargetCurve* curve = nullptr;   // nullptr → no curve prior
    const float* measuredBandDb = nullptr;         // 8 bands, level-invariant; nullptr → no measurement
    float residualBlend = 0.0f;                    // [0,1], 0 = no blend (default)
};

bool decodeSonicMasterDecision(const float* decision,
                               std::size_t decisionCount,
                               double sampleRate,
                               ValidatedNeuralMasteringPlan& out,
                               float callerTargetLufs = kUseModelTargetLufs,
                               GenreEqPrior eqPrior = {}) noexcept;

} // namespace more_phi
