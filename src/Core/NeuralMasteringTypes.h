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

    [[nodiscard]] bool any() const noexcept
    {
        return eq || dynamics || stereo || harmonic || limiter || loudness;
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
};

} // namespace more_phi
