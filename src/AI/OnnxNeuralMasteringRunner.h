/*
 * More-Phi — AI/OnnxNeuralMasteringRunner.h
 *
 * Concrete INeuralMasteringModelRunner backed by an ONNX Runtime session.
 *
 * STATUS (v3.3.0): SEAM ONLY.
 *   The ONNX Runtime library is intentionally NOT linked into the build yet
 *   (see CMakeLists.txt — no onnxruntime dependency). Until it is, loadModel()
 *   returns false and isAvailable() reports false, so wiring this runner into
 *   a NeuralMasteringController is behaviourally identical to the Null runner:
 *   it abstains on every frame and the controller falls back to the
 *   DeterministicBaselineNeuralMasteringRunner. No audio-thread or
 *   distribution surface changes.
 *
 * DESIGN GOALS:
 *   1. Real-time safety: loadModel()/unloadModel() run on the message thread
 *      only. proposePlan() (called by NeuralMasteringController on the message
 *      thread) never allocates after a successful load — input/output tensor
 *      buffers are pre-sized in loadModel() and reused.
 *   2. No ONNX header dependency at compile time for the rest of the codebase:
 *      the Ort::Session is held behind a forward-declared pimpl
 *      (OnnxSessionHandle) so this header stays dependency-free.
 *   3. Testable I/O contract without ONNX: the feature-frame → input tensor
 *      and output tensor → plan-candidate transforms are exposed as pure,
 *      noexcept free functions (serializeFeatureFrame / buildPlanCandidate /
 *      sanitizePlanCandidate) and are unit-tested directly. When ONNX is
 *      eventually linked, only loadModel() and the internal inference step
 *      change; the I/O plumbing below is the stable, validated contract.
 *   4. Defence in depth: even a model that emits NaN/Inf or out-of-range
 *      deltas cannot reach the DSP — sanitizePlanCandidate() coerces the
 *      candidate to finite, in-range values, and the NeuralMasteringSafety
 *      Policy clamps the final projection regardless.
 *
 * I/O SCHEMA (feature schema v1 / plan schema v1):
 *   Input tensor  : kOnnxInputFeatureCount floats  (see serializeFeatureFrame)
 *   Output tensor : kOnnxOutputDeltaCount  floats  (see buildPlanCandidate)
 *   The model is trained to predict per-control DELTAS in [-1, 1]; the safety
 *   policy projects previous + delta and clamps per maxDeltaPerPlan.
 */
#pragma once

#include "AI/NeuralMasteringModelMetadata.h"
#include "AI/NeuralMasteringModelRunner.h"
#include "Core/NeuralMasteringTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace more_phi {

// ── Forward declaration: opaque ONNX session handle (defined in .cpp) ───────
struct OnnxSessionHandle;

// ── I/O contract constants (feature schema v1 / plan schema v1) ─────────────
// Must match the training-side preprocessing exactly. Documented inline in
// serializeFeatureFrame() / buildPlanCandidate().

// Input: scalar features (11) + spectralBands (32) + stereoCorrelation (8)
// + midSideRatio (8) + layout/rate meta (4) = 63.
inline constexpr std::size_t kOnnxScalarFeatureCount      = 11;
inline constexpr std::size_t kOnnxInputFeatureCount =
    kOnnxScalarFeatureCount
    + kNeuralMasteringSpectralBandCount
    + kNeuralMasteringStereoBandCount
    + kNeuralMasteringStereoBandCount
    + 4; // sampleRate, channelCount, blockSize, frameIndex (as float)

// Output: per-control deltas. eq(32)+dynamics(8)+stereo(8)+harmonic(8)
// +limiter(8)+loudness(8) = 72.
inline constexpr std::size_t kOnnxOutputDeltaCount =
    kNeuralMasteringEqTargetCount
    + kNeuralMasteringDynamicsTargetCount
    + kNeuralMasteringStereoTargetCount
    + kNeuralMasteringHarmonicTargetCount
    + kNeuralMasteringLimiterTargetCount
    + kNeuralMasteringLoudnessTargetCount;

// ── Pure, noexcept, ONNX-free I/O transforms (unit-tested directly) ─────────

/**
 * Pack a NeuralMasteringFeatureFrame into a dense float input tensor in the
 * exact order the model was trained against. Order:
 *   [0]  integratedLUFS       [1]  shortTermLUFS      [2]  momentaryLUFS
 *   [3]  loudnessRange        [4]  truePeakDbTp       [5]  crestFactorDb
 *   [6]  spectralTilt         [7]  monoFoldDownDeltaDb [8] transientDensity
 *   [9]  harmonicRisk         [10] sourceQualityScore
 *   [11..42] spectralBands[32]
 *   [43..50] stereoCorrelation[8]
 *   [51..58] midSideRatio[8]
 *   [59] sampleRate (as float) [60] channelCount [61] blockSize
 *   [62] frameIndex (low 32 bits as float)
 * Non-finite scalars are coerced to 0.0f so a corrupt frame cannot poison the
 * model input. Requires out.size() >= kOnnxInputFeatureCount.
 */
void serializeFeatureFrame(const NeuralMasteringFeatureFrame& frame,
                           float* outTensor,
                           std::size_t capacity) noexcept;

/**
 * Map a model output delta tensor back into a NeuralMasteringPlanCandidate.
 * The candidate's `targets` and `deltas` are both set to the supplied deltas
 * (matching DeterministicBaselineNeuralMasteringRunner's convention; the
 * safety policy treats `deltas` as authoritative for projection). The
 * editableMask is whatever the caller passes (typically the runner's
 * configured safe-mask). Confidence, evidence level, and timestamps are set
 * from the feature frame so the candidate is self-describing.
 */
NeuralMasteringPlanCandidate buildPlanCandidate(const float* deltaTensor,
                                                std::size_t count,
                                                const NeuralMasteringFeatureFrame& frame,
                                                float confidence,
                                                NeuralMasteringEvidenceLevel evidence,
                                                MasteringControlMask editableMask) noexcept;

/**
 * Coerce a candidate to a safe, finite, in-range form. Any non-finite target
 * or delta is replaced with 0.0f; any delta outside [-1, 1] is clamped.
 * Returns true if any coercion occurred (useful for telemetry / tests).
 */
bool sanitizePlanCandidate(NeuralMasteringPlanCandidate& candidate) noexcept;

// ── The runner itself ────────────────────────────────────────────────────────

/**
 * ONNX-backed neural mastering runner. See file header for status/seam notes.
 *
 * Thread safety:
 *   loadModel()/unloadModel()/setEditableMask() — message thread only.
 *   proposePlan() — message thread only (invoked by NeuralMasteringController).
 *   isAvailable()/usesExternalInference() — any thread (atomic reads).
 *
 * Memory:
 *   On a successful loadModel(), input/output tensor buffers are reserved to
 *   kOnnxInputFeatureCount / kOnnxOutputDeltaCount and reused on every
 *   proposePlan() call — zero per-inference allocation.
 */
class OnnxNeuralMasteringRunner final : public INeuralMasteringModelRunner
{
public:
    OnnxNeuralMasteringRunner() noexcept;
    ~OnnxNeuralMasteringRunner() override;

    OnnxNeuralMasteringRunner(const OnnxNeuralMasteringRunner&) = delete;
    OnnxNeuralMasteringRunner& operator=(const OnnxNeuralMasteringRunner&) = delete;

    // ── Model lifecycle (message thread only) ────────────────────────────────

    /**
     * Load + validate an ONNX model file. Returns true only if a session was
     * created AND its I/O shapes match the v1 feature/plan schema.
     *
     * CURRENT IMPLEMENTATION: returns false and logs that onnxruntime is not
     * linked. This is the seam — the body is filled in when ONNX Runtime is
     * added to CMakeLists.txt. modelId/checksum are still captured so the
     * metadata plumbing can be exercised in tests.
     */
    bool loadModel(std::string_view absolutePath, std::string_view modelId = {}, std::string_view checksum = {});
    void unloadModel() noexcept;

    /** Configure which control groups the model is allowed to move.
     *  Defaults to the non-high-risk set (eq/dynamics/stereo/loudness),
     *  matching DeterministicBaseline. Harmonic/limiter are high-risk per
     *  NeuralMasteringSafetyPolicy::defaultConfig() and are off by default. */
    void setEditableMask(MasteringControlMask mask) noexcept;

    // ── Accessors (any thread) ────────────────────────────────────────────────

    [[nodiscard]] bool isAvailable() const noexcept override;
    // Reports true only when a session is actually live — so an unloaded
    // runner wired into a controller reports modelInvoked == false, exactly
    // like the Null runner.
    [[nodiscard]] bool usesExternalInference() const noexcept override;

    [[nodiscard]] NeuralMasteringModelMetadata metadata() const noexcept;

    // ── INeuralMasteringModelRunner (message thread) ──────────────────────────

    [[nodiscard]] NeuralMasteringPlannerResult proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept override;

private:
    // pimpl — defined in the .cpp; no ONNX types leak into this header.
    std::unique_ptr<OnnxSessionHandle> session_;
    MasteringControlMask editableMask_ {};
    std::vector<float> inputBuffer_;   // sized in loadModel(), reused
    std::vector<float> outputBuffer_;  // sized in loadModel(), reused
    bool available_ = false;
};

} // namespace more_phi
