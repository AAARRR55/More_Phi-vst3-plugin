// src/AI/SonicMasterDecisionDecoder.h
#pragma once

#include "Core/NeuralMasteringTypes.h"

#include <cstddef>

namespace more_phi {

// Width of the masteringbrainv2 full-chain-v2 decision vector. Must match
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
 */
bool decodeSonicMasterDecision(const float* decision,
                               std::size_t decisionCount,
                               double sampleRate,
                               ValidatedNeuralMasteringPlan& out) noexcept;

} // namespace more_phi
