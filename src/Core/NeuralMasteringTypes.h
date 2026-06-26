#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace more_phi {

inline constexpr std::uint32_t kNeuralMasteringSchemaVersion = 1;
inline constexpr std::uint32_t kNeuralMasteringFeatureSchemaVersion = 1;
inline constexpr std::uint32_t kNeuralMasteringPlanSchemaVersion = 1;
inline constexpr std::size_t kNeuralMasteringEqTargetCount = 32;
inline constexpr std::size_t kNeuralMasteringDynamicsTargetCount = 8;
inline constexpr std::size_t kNeuralMasteringStereoTargetCount = 8;
inline constexpr std::size_t kNeuralMasteringHarmonicTargetCount = 8;
inline constexpr std::size_t kNeuralMasteringLimiterTargetCount = 8;
inline constexpr std::size_t kNeuralMasteringLoudnessTargetCount = 8;
inline constexpr std::size_t kNeuralMasteringSpectralBandCount = 32;
inline constexpr std::size_t kNeuralMasteringStereoBandCount = 8;
inline constexpr std::size_t kNeuralMasteringGateCount = 10;
inline constexpr std::size_t kNeuralMasteringIssueCapacity = 16;

// AUDIT-2.1: number of compressor bands the neural mastering decision emits
// (masteringbrainv2 contract). Declared in Core (not the AI decoder header) so
// the ValidatedNeuralMasteringPlan sidecar below can carry the full per-band
// param set without Core depending on AI.
inline constexpr std::size_t kNeuralMasteringCompBandCount = 3;

// AUDIT-2.1: full per-band compressor params, in real units. The 44-float
// SonicMaster decision carries all six (threshold,ratio,attack,release,makeup,
// knee) per band, but the normalized MasteringTargetVector.dynamics array holds
// only 2/band in [-1,1] delta space. The other four travel in this sidecar.
struct NeuralMasteringCompBand
{
    float thresholdDb = -20.0f;
    float ratio       =   2.5f;
    float attackMs    =  15.0f;
    float releaseMs   = 150.0f;
    float makeupDb    =   0.0f;
    float kneeDb      =   2.0f;
};

enum class NeuralMasteringRuntimeMode : std::uint8_t
{
    Offline,
    Preview,
    Background,
    MessageThread,
    QueuedLowRateControl,
    AudioCallbackProhibited
};

enum class NeuralMasteringEvidenceLevel : std::uint8_t
{
    Planning,
    ResearchEstimate,
    PrototypeMeasured,
    ProductionMeasured
};

enum class NeuralMasteringFallbackMode : std::uint8_t
{
    None,
    LastSafeHold,
    DeterministicBaseline,
    ReviewOnly,
    TransparentBypass,
    Reject
};

enum class NeuralMasteringGateId : std::uint8_t
{
    G01,
    G02,
    G03,
    G04,
    G05,
    G06,
    G07,
    G08,
    G09,
    G10
};

enum class NeuralMasteringGateStatus : std::uint8_t
{
    Pass,
    Fail,
    NotMeasured,
    NotApplicable
};

enum class NeuralMasteringDecision : std::uint8_t
{
    Proceed,
    ReviewOnly,
    FallbackOnly,
    NoGo,
    ReleaseApproved
};

enum class NeuralMasteringLayout : std::uint8_t
{
    Mono,
    Stereo,
    Wider,
    Unsupported
};

enum class NeuralMasteringValidationIssue : std::uint8_t
{
    None,
    SchemaVersionMismatch,
    AudioCallbackRuntime,
    InvalidTimestamp,
    StalePlan,
    LowConfidence,
    Abstain,
    ReviewOnly,
    UnsupportedLayout,
    NonFiniteValue,
    TargetOutOfRange,
    DeltaOutOfRange,
    IllegalMask,
    HighRiskMask,
    MaxDeltaProjected
};

// DIAGNOSTIC (2026-06-26): enum→string mapper for NeuralMasteringValidationIssue.
// Until now no stringification existed anywhere, so a safety-policy rejection
// surfaced only as a coarse "safety_rejected" with the specific issue discarded
// (SonicMasterAnalysisEngine.cpp requestDecisionNow dropped verdict.issues[]).
// This mapper lets the on-demand path report WHICH gate tripped — e.g.
// LowConfidence (model out-of-distribution on a -7 LUFS input) vs TargetOutOfRange
// (decoded EQ/loudness outside sanity bounds) vs NonFiniteValue (NaN from the
// model) — so the assistant stops confabulating "transient, try again" and can
// give an honest next step. snake_case keys for JSON; keep in sync with the enum.
inline const char* neuralMasteringIssueKey(NeuralMasteringValidationIssue issue) noexcept
{
    switch (issue)
    {
        case NeuralMasteringValidationIssue::None:                  return "none";
        case NeuralMasteringValidationIssue::SchemaVersionMismatch: return "schema_version_mismatch";
        case NeuralMasteringValidationIssue::AudioCallbackRuntime:  return "audio_callback_runtime";
        case NeuralMasteringValidationIssue::InvalidTimestamp:      return "invalid_timestamp";
        case NeuralMasteringValidationIssue::StalePlan:             return "stale_plan";
        case NeuralMasteringValidationIssue::LowConfidence:         return "low_confidence";
        case NeuralMasteringValidationIssue::Abstain:               return "abstain";
        case NeuralMasteringValidationIssue::ReviewOnly:            return "review_only";
        case NeuralMasteringValidationIssue::UnsupportedLayout:     return "unsupported_layout";
        case NeuralMasteringValidationIssue::NonFiniteValue:        return "non_finite_value";
        case NeuralMasteringValidationIssue::TargetOutOfRange:      return "target_out_of_range";
        case NeuralMasteringValidationIssue::DeltaOutOfRange:       return "delta_out_of_range";
        case NeuralMasteringValidationIssue::IllegalMask:           return "illegal_mask";
        case NeuralMasteringValidationIssue::HighRiskMask:          return "high_risk_mask";
        case NeuralMasteringValidationIssue::MaxDeltaProjected:     return "max_delta_projected";
    }
    return "unknown";
}

// DIAGNOSTIC (2026-06-26): is this issue a HARD reject? Mirrors the file-static
// isHardRejectIssue() in NeuralMasteringSafetyPolicy.cpp, exposed publicly so
// the on-demand path can tell the assistant "this is a hard reject (retrying
// won't help until the audio/plan changes)" vs "soft hold (retryable)." The
// authoritative list lives in the policy .cpp; this public mirror must stay in
// sync with it. Soft issues: StalePlan, LowConfidence, Abstain, ReviewOnly,
// MaxDeltaProjected (informational). Everything else is hard.
inline bool isHardRejectNeuralMasteringIssue(NeuralMasteringValidationIssue issue) noexcept
{
    switch (issue)
    {
        case NeuralMasteringValidationIssue::None:
        case NeuralMasteringValidationIssue::StalePlan:
        case NeuralMasteringValidationIssue::LowConfidence:
        case NeuralMasteringValidationIssue::Abstain:
        case NeuralMasteringValidationIssue::ReviewOnly:
        case NeuralMasteringValidationIssue::MaxDeltaProjected:
            return false;
        case NeuralMasteringValidationIssue::SchemaVersionMismatch:
        case NeuralMasteringValidationIssue::AudioCallbackRuntime:
        case NeuralMasteringValidationIssue::InvalidTimestamp:
        case NeuralMasteringValidationIssue::UnsupportedLayout:
        case NeuralMasteringValidationIssue::NonFiniteValue:
        case NeuralMasteringValidationIssue::TargetOutOfRange:
        case NeuralMasteringValidationIssue::DeltaOutOfRange:
        case NeuralMasteringValidationIssue::IllegalMask:
        case NeuralMasteringValidationIssue::HighRiskMask:
            return true;
    }
    return true;  // unknown enum value → treat as hard (fail-closed)
}

struct MasteringTargetVector
{
    std::array<float, kNeuralMasteringEqTargetCount> eq {};
    std::array<float, kNeuralMasteringDynamicsTargetCount> dynamics {};
    std::array<float, kNeuralMasteringStereoTargetCount> stereo {};
    std::array<float, kNeuralMasteringHarmonicTargetCount> harmonic {};
    std::array<float, kNeuralMasteringLimiterTargetCount> limiter {};
    std::array<float, kNeuralMasteringLoudnessTargetCount> loudness {};
};

struct MasteringControlMask
{
    bool eq = false;
    bool dynamics = false;
    bool stereo = false;
    bool harmonic = false;
    bool limiter = false;
    bool loudness = false;
    // IMPACT (Phase 3, 2026-06-26): transient shaper. Off by default; a genre
    // profile or future decode slot can raise it. Sits in the mask so the safety
    // policy and applyValidatedPlan treat it uniformly with the other stages.
    bool impact = false;

    [[nodiscard]] bool any() const noexcept
    {
        return eq || dynamics || stereo || harmonic || limiter || loudness || impact;
    }
};

struct NeuralMasteringGateResult
{
    NeuralMasteringGateId gateId = NeuralMasteringGateId::G01;
    NeuralMasteringGateStatus status = NeuralMasteringGateStatus::NotApplicable;
    NeuralMasteringEvidenceLevel evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    NeuralMasteringDecision decision = NeuralMasteringDecision::ReviewOnly;
};

struct NeuralMasteringFeatureFrame
{
    std::uint32_t schemaVersion = kNeuralMasteringFeatureSchemaVersion;
    double sampleRate = 0.0;
    int channelCount = 0;
    int blockSize = 0;
    std::uint64_t frameIndex = 0;
    float integratedLUFS = 0.0f;
    float shortTermLUFS = 0.0f;
    float momentaryLUFS = 0.0f;
    float loudnessRange = 0.0f;
    float truePeakDbTp = 0.0f;
    float crestFactorDb = 0.0f;
    float spectralTilt = 0.0f;
    std::array<float, kNeuralMasteringSpectralBandCount> spectralBands {};
    std::array<float, kNeuralMasteringStereoBandCount> stereoCorrelation {};
    std::array<float, kNeuralMasteringStereoBandCount> midSideRatio {};
    float monoFoldDownDeltaDb = 0.0f;
    float transientDensity = 0.0f;
    float harmonicRisk = 0.0f;
    float sourceQualityScore = 0.0f;
};

struct NeuralMasteringRuntimeState
{
    std::uint64_t currentFrame = 0;
    double sampleRate = 48000.0;
    int channelCount = 2;
    NeuralMasteringLayout layout = NeuralMasteringLayout::Stereo;
    bool overload = false;
};

struct NeuralMasteringPlanCandidate
{
    std::uint32_t schemaVersion = kNeuralMasteringPlanSchemaVersion;
    std::uint64_t planId = 0;
    NeuralMasteringRuntimeMode runtimeMode = NeuralMasteringRuntimeMode::Background;
    std::uint64_t producedAtFrame = 0;
    std::uint64_t expiresAfterFrame = 0;
    float confidence = 1.0f;
    bool abstain = false;
    bool reviewOnly = false;
    NeuralMasteringEvidenceLevel evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    MasteringTargetVector targets {};
    MasteringTargetVector deltas {};
    MasteringControlMask editableMask {};
    std::array<NeuralMasteringGateResult, kNeuralMasteringGateCount> gateResults {};
    NeuralMasteringFallbackMode requestedFallbackMode = NeuralMasteringFallbackMode::None;

    // AUDIT-FIX: carry the full compressor sidecar through the safety policy
    // so the verdict preserves the model's attack/release/makeup/knee values.
    // Previously these were dropped because the candidate had no slot for them.
    std::array<NeuralMasteringCompBand, kNeuralMasteringCompBandCount> compParams {};
    bool hasCompParams = false;

    // Staleness guard: capture instant (steady_clock ns) when the audio window
    // was captured. Plans applied much later can be discarded.
    std::uint64_t capturedAtSteadyClockNs = 0;
};

struct ValidatedNeuralMasteringPlan
{
    std::uint64_t sourcePlanId = 0;
    MasteringTargetVector projectedTargets {};
    MasteringControlMask appliedMask {};
    NeuralMasteringFallbackMode fallbackMode = NeuralMasteringFallbackMode::None;
    std::array<NeuralMasteringGateResult, kNeuralMasteringGateCount> gateResults {};
    NeuralMasteringEvidenceLevel evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    bool valid = false;
    bool projected = false;

    // AUDIT-IX-8: steady-clock nanoseconds at which the audio window feeding this
    // plan was captured. Set by SonicMasterAnalysisEngine at capture time; checked
    // at apply time so a plan older than the staleness budget is discarded rather
    // than applied against audio it no longer describes. 0 = untimestamped (legacy
    // producers), which skips the check.
    std::uint64_t capturedAtSteadyClockNs = 0;

    // AUDIT-2.1: full per-band compressor params. Set by the SonicMaster decoder
    // (it has all six from the decision vector); other plan producers leave this
    // false and applyValidatedPlan falls back to the normalized dynamics pair.
    std::array<NeuralMasteringCompBand, kNeuralMasteringCompBandCount> compParams {};
    bool hasCompParams = false;

    // AUDIT-FIX (H2): semantic guard. projectedTargets.loudness is a TARGET the
    // model was asked to reach (the SonicMaster input is peak-normalized, so the
    // model cannot measure absolute input LUFS — see SonicMasterAnalysisEngine
    // AUDIT-7). It must NEVER be read as a measurement of the input. Genuine
    // measurements live in SonicMasterMeasurementSnapshot (BS.1770-4 / true-peak).
    // This flag is set false by every plan producer and asserted at decode time;
    // any future path that genuinely measures input loudness must set it true.
    bool loudnessIsMeasurement = false;

    // AUDIT: opt-in flag. When true, applyValidatedPlan honours the limiter ceiling
    // from the decoded SonicMaster decision, hard-clamped to [-3, -0.1] dB TP. The
    // decoder leaves appliedMask.limiter OFF by default; callers (SonicMasterAnalysisEngine,
    // MCP sonicmaster_decision tool) set this true when they want the ceiling applied.
    bool applyLimiterCeiling = false;
};

} // namespace more_phi
