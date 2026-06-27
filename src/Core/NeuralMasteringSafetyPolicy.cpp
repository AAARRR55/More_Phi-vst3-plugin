#include "NeuralMasteringSafetyPolicy.h"

#include <algorithm>
#include <cmath>

namespace more_phi {
namespace {

template <std::size_t N>
void fillArray(std::array<float, N>& values, float value) noexcept
{
    values.fill(value);
}

void fillTargets(MasteringTargetVector& targets, float value) noexcept
{
    fillArray(targets.eq, value);
    fillArray(targets.dynamics, value);
    fillArray(targets.stereo, value);
    fillArray(targets.harmonic, value);
    fillArray(targets.limiter, value);
    fillArray(targets.loudness, value);
}

std::array<NeuralMasteringGateResult, kNeuralMasteringGateCount> makeDefaultGates() noexcept
{
    std::array<NeuralMasteringGateResult, kNeuralMasteringGateCount> gates {};
    for (std::size_t i = 0; i < gates.size(); ++i)
    {
        gates[i].gateId = static_cast<NeuralMasteringGateId>(i);
        gates[i].status = NeuralMasteringGateStatus::NotApplicable;
        gates[i].evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
        gates[i].decision = NeuralMasteringDecision::ReviewOnly;
    }
    return gates;
}

void setGate(std::array<NeuralMasteringGateResult, kNeuralMasteringGateCount>& gates,
             NeuralMasteringGateId gateId,
             NeuralMasteringGateStatus status,
             NeuralMasteringDecision decision) noexcept
{
    auto& gate = gates[static_cast<std::size_t>(gateId)];
    gate.gateId = gateId;
    gate.status = status;
    gate.evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    gate.decision = decision;
}

template <std::size_t N>
bool allFinite(const std::array<float, N>& values) noexcept
{
    for (const auto value : values)
        if (!std::isfinite(value))
            return false;
    return true;
}

bool allFinite(const MasteringTargetVector& values) noexcept
{
    return allFinite(values.eq)
        && allFinite(values.dynamics)
        && allFinite(values.stereo)
        && allFinite(values.harmonic)
        && allFinite(values.limiter)
        && allFinite(values.loudness);
}

template <std::size_t N>
bool inRange(const std::array<float, N>& values,
             const std::array<float, N>& minimums,
             const std::array<float, N>& maximums) noexcept
{
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] < minimums[i] || values[i] > maximums[i])
            return false;
    return true;
}

bool inRange(const MasteringTargetVector& values,
             const MasteringTargetVector& minimums,
             const MasteringTargetVector& maximums) noexcept
{
    return inRange(values.eq, minimums.eq, maximums.eq)
        && inRange(values.dynamics, minimums.dynamics, maximums.dynamics)
        && inRange(values.stereo, minimums.stereo, maximums.stereo)
        && inRange(values.harmonic, minimums.harmonic, maximums.harmonic)
        && inRange(values.limiter, minimums.limiter, maximums.limiter)
        && inRange(values.loudness, minimums.loudness, maximums.loudness);
}

bool hasHighRiskMask(const MasteringControlMask& requested, const MasteringControlMask& highRisk) noexcept
{
    return (requested.eq && highRisk.eq)
        || (requested.dynamics && highRisk.dynamics)
        || (requested.stereo && highRisk.stereo)
        || (requested.harmonic && highRisk.harmonic)
        || (requested.limiter && highRisk.limiter)
        || (requested.loudness && highRisk.loudness);
}

bool layoutSupported(NeuralMasteringLayout layout, const NeuralMasteringSafetyPolicyConfig& config) noexcept
{
    switch (layout)
    {
        case NeuralMasteringLayout::Mono: return config.supportsMono;
        case NeuralMasteringLayout::Stereo: return config.supportsStereo;
        case NeuralMasteringLayout::Wider: return config.supportsWider;
        case NeuralMasteringLayout::Unsupported: return false;
    }
    return false;
}

bool runtimeModeAllowed(NeuralMasteringRuntimeMode mode) noexcept
{
    switch (mode)
    {
        case NeuralMasteringRuntimeMode::Offline:
        case NeuralMasteringRuntimeMode::Preview:
        case NeuralMasteringRuntimeMode::Background:
        case NeuralMasteringRuntimeMode::MessageThread:
        case NeuralMasteringRuntimeMode::QueuedLowRateControl:
            return true;
        case NeuralMasteringRuntimeMode::AudioCallbackProhibited:
            return false;
    }
    return false;
}

template <std::size_t N>
bool hasDeltaOutOfRange(const std::array<float, N>& values) noexcept
{
    for (const auto value : values)
        if (value < -1.0f || value > 1.0f)
            return true;
    return false;
}

bool hasDeltaOutOfRange(const MasteringTargetVector& deltas) noexcept
{
    return hasDeltaOutOfRange(deltas.eq)
        || hasDeltaOutOfRange(deltas.dynamics)
        || hasDeltaOutOfRange(deltas.stereo)
        || hasDeltaOutOfRange(deltas.harmonic)
        || hasDeltaOutOfRange(deltas.limiter)
        || hasDeltaOutOfRange(deltas.loudness);
}

template <std::size_t N>
void projectArray(std::array<float, N>& output,
                  const std::array<float, N>& previous,
                  const std::array<float, N>& deltas,
                  const std::array<float, N>& minimums,
                  const std::array<float, N>& maximums,
                  const std::array<float, N>& maxDelta,
                  bool& projected) noexcept
{
    for (std::size_t i = 0; i < output.size(); ++i)
    {
        const auto boundedDelta = std::clamp(deltas[i], -maxDelta[i], maxDelta[i]);
        if (boundedDelta != deltas[i])
            projected = true;
        output[i] = std::clamp(previous[i] + boundedDelta, minimums[i], maximums[i]);
    }
}

void projectMaskedTargets(MasteringTargetVector& output,
                          const MasteringTargetVector& previous,
                          const NeuralMasteringPlanCandidate& candidate,
                          const NeuralMasteringSafetyPolicyConfig& config,
                          bool& projected) noexcept
{
    output = previous;
    if (candidate.editableMask.eq)
        projectArray(output.eq, previous.eq, candidate.deltas.eq, config.minTargets.eq, config.maxTargets.eq, config.maxDeltaPerPlan.eq, projected);
    if (candidate.editableMask.dynamics)
        projectArray(output.dynamics, previous.dynamics, candidate.deltas.dynamics, config.minTargets.dynamics, config.maxTargets.dynamics, config.maxDeltaPerPlan.dynamics, projected);
    if (candidate.editableMask.stereo)
        projectArray(output.stereo, previous.stereo, candidate.deltas.stereo, config.minTargets.stereo, config.maxTargets.stereo, config.maxDeltaPerPlan.stereo, projected);
    if (candidate.editableMask.harmonic)
        projectArray(output.harmonic, previous.harmonic, candidate.deltas.harmonic, config.minTargets.harmonic, config.maxTargets.harmonic, config.maxDeltaPerPlan.harmonic, projected);
    if (candidate.editableMask.limiter)
        projectArray(output.limiter, previous.limiter, candidate.deltas.limiter, config.minTargets.limiter, config.maxTargets.limiter, config.maxDeltaPerPlan.limiter, projected);
    if (candidate.editableMask.loudness)
        projectArray(output.loudness, previous.loudness, candidate.deltas.loudness, config.minTargets.loudness, config.maxTargets.loudness, config.maxDeltaPerPlan.loudness, projected);
}

// AUDIT-FIX (F4.1): clamp a plan's projectedTargets deltas to maxDeltaPerPlan
// relative to the last safe baseline. Used by applyValidatedPlan so a direct
// in-process caller that bypasses validate() cannot exceed the per-cycle slew
// limit (e.g. >0.6 LU/cycle loudness). Only the dimensions the plan actually
// applies (plan.appliedMask) are slew-limited; unmasked dimensions are left to
// the caller. Returns the count of clamped dimensions for telemetry.
template <std::size_t N>
std::size_t clampDeltaArray(std::array<float, N>& current,
                            const std::array<float, N>& previous,
                            const std::array<float, N>& maxDelta) noexcept
{
    std::size_t clamped = 0;
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        const float delta = current[i] - previous[i];
        const float bounded = std::clamp(delta, -maxDelta[i], maxDelta[i]);
        if (bounded != delta)
        {
            current[i] = previous[i] + bounded;
            ++clamped;
        }
    }
    return clamped;
}

// enforceDeltaCaps moved out of the anonymous namespace (C2888: a class member
// cannot be defined inside an anonymous namespace it wasn't declared in). See
// the definition below, after the anon-namespace closes.

bool isHardRejectIssue(NeuralMasteringValidationIssue issue) noexcept
{
    switch (issue)
    {
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
        case NeuralMasteringValidationIssue::None:
        case NeuralMasteringValidationIssue::StalePlan:
        case NeuralMasteringValidationIssue::LowConfidence:
        case NeuralMasteringValidationIssue::Abstain:
        case NeuralMasteringValidationIssue::ReviewOnly:
        case NeuralMasteringValidationIssue::MaxDeltaProjected:
            return false;
    }
    return true;
}

bool hasAnyHardRejectIssue(const NeuralMasteringValidationResult& result) noexcept
{
    for (std::size_t i = 0; i < result.issueCount; ++i)
        if (isHardRejectIssue(result.issues[i]))
            return true;
    return false;
}

bool hasIssue(const NeuralMasteringValidationResult& result, NeuralMasteringValidationIssue issue) noexcept
{
    return result.hasIssue(issue);
}

NeuralMasteringFallbackMode chooseRequestedFallback(const NeuralMasteringPlanCandidate& candidate,
                                                    NeuralMasteringFallbackMode defaultFallback) noexcept
{
    return candidate.requestedFallbackMode == NeuralMasteringFallbackMode::None
        ? defaultFallback
        : candidate.requestedFallbackMode;
}

void applyFallback(NeuralMasteringValidationResult& result,
                   NeuralMasteringFallbackMode fallbackMode,
                   const ValidatedNeuralMasteringPlan& lastSafePlan,
                   bool hasLastSafePlan) noexcept
{
    result.accepted = false;
    result.fallbackSelected = fallbackMode != NeuralMasteringFallbackMode::Reject;

    if (fallbackMode == NeuralMasteringFallbackMode::LastSafeHold)
    {
        if (hasLastSafePlan)
        {
            result.plan = lastSafePlan;
            result.plan.fallbackMode = NeuralMasteringFallbackMode::LastSafeHold;
            result.plan.valid = false;
            result.fallbackSelected = true;
            return;
        }
        fallbackMode = NeuralMasteringFallbackMode::DeterministicBaseline;
    }

    result.plan.valid = false;
    result.plan.projected = false;
    result.plan.projectedTargets = {};
    result.plan.appliedMask = {};
    result.plan.fallbackMode = fallbackMode;
    if (fallbackMode == NeuralMasteringFallbackMode::DeterministicBaseline
        || fallbackMode == NeuralMasteringFallbackMode::TransparentBypass
        || fallbackMode == NeuralMasteringFallbackMode::ReviewOnly)
    {
        result.fallbackSelected = true;
    }
}

void populateRuntimeGates(NeuralMasteringValidationResult& result) noexcept
{
    setGate(result.plan.gateResults,
            NeuralMasteringGateId::G04,
            hasIssue(result, NeuralMasteringValidationIssue::AudioCallbackRuntime)
                ? NeuralMasteringGateStatus::Fail
                : NeuralMasteringGateStatus::Pass,
            hasIssue(result, NeuralMasteringValidationIssue::AudioCallbackRuntime)
                ? NeuralMasteringDecision::NoGo
                : NeuralMasteringDecision::Proceed);

    const bool invalidOutputFailure = hasIssue(result, NeuralMasteringValidationIssue::SchemaVersionMismatch)
                                   || hasIssue(result, NeuralMasteringValidationIssue::NonFiniteValue)
                                   || hasIssue(result, NeuralMasteringValidationIssue::TargetOutOfRange)
                                   || hasIssue(result, NeuralMasteringValidationIssue::DeltaOutOfRange)
                                   || hasIssue(result, NeuralMasteringValidationIssue::IllegalMask)
                                   || hasIssue(result, NeuralMasteringValidationIssue::HighRiskMask)
                                   || hasIssue(result, NeuralMasteringValidationIssue::InvalidTimestamp);
    setGate(result.plan.gateResults,
            NeuralMasteringGateId::G06,
            invalidOutputFailure ? NeuralMasteringGateStatus::Fail : NeuralMasteringGateStatus::Pass,
            invalidOutputFailure ? NeuralMasteringDecision::NoGo : NeuralMasteringDecision::Proceed);

    setGate(result.plan.gateResults,
            NeuralMasteringGateId::G07,
            hasIssue(result, NeuralMasteringValidationIssue::HighRiskMask)
                ? NeuralMasteringGateStatus::Fail
                : NeuralMasteringGateStatus::Pass,
            hasIssue(result, NeuralMasteringValidationIssue::HighRiskMask)
                ? NeuralMasteringDecision::NoGo
                : NeuralMasteringDecision::Proceed);

    setGate(result.plan.gateResults,
            NeuralMasteringGateId::G08,
            NeuralMasteringGateStatus::NotMeasured,
            NeuralMasteringDecision::ReviewOnly);

    setGate(result.plan.gateResults,
            NeuralMasteringGateId::G09,
            NeuralMasteringGateStatus::Pass,
            result.fallbackSelected ? NeuralMasteringDecision::FallbackOnly : NeuralMasteringDecision::Proceed);
}

} // namespace

NeuralMasteringSafetyPolicyConfig NeuralMasteringSafetyPolicy::defaultConfig() noexcept
{
    NeuralMasteringSafetyPolicyConfig config;
    fillTargets(config.minTargets, -1.0f);
    fillTargets(config.maxTargets, 1.0f);
    fillTargets(config.maxDeltaPerPlan, 0.10f);
    fillArray(config.maxDeltaPerPlan.eq, 0.15f);
    fillArray(config.maxDeltaPerPlan.dynamics, 0.12f);
    fillArray(config.maxDeltaPerPlan.stereo, 0.10f);
    fillArray(config.maxDeltaPerPlan.harmonic, 0.08f);
    fillArray(config.maxDeltaPerPlan.limiter, 0.05f);
    fillArray(config.maxDeltaPerPlan.loudness, 0.10f);
    config.highRiskControls.harmonic = true;
    config.highRiskControls.limiter = true;
    return config;
}

// AUDIT-FIX (F4.1): clamp a plan's projectedTargets deltas to maxDeltaPerPlan
// relative to the last safe baseline. Defined OUTSIDE the anonymous namespace
// (it's a class member; C2888 forbids member defs inside an anon namespace).
// Uses clampDeltaArray which is internal-linkage and stays in the anon block.
std::size_t NeuralMasteringSafetyPolicy::enforceDeltaCaps(
    ValidatedNeuralMasteringPlan& current,
    const ValidatedNeuralMasteringPlan& lastSafePlan,
    bool hasLastSafe) noexcept
{
    if (!hasLastSafe)
        return 0; // no baseline -> first apply is unconstrained (same as validate())

    const auto& cfg = config_;
    const auto& prev = lastSafePlan.projectedTargets;
    std::size_t total = 0;
    if (current.appliedMask.eq)
        total += clampDeltaArray(current.projectedTargets.eq, prev.eq, cfg.maxDeltaPerPlan.eq);
    if (current.appliedMask.dynamics)
        total += clampDeltaArray(current.projectedTargets.dynamics, prev.dynamics, cfg.maxDeltaPerPlan.dynamics);
    if (current.appliedMask.stereo)
        total += clampDeltaArray(current.projectedTargets.stereo, prev.stereo, cfg.maxDeltaPerPlan.stereo);
    if (current.appliedMask.harmonic)
        total += clampDeltaArray(current.projectedTargets.harmonic, prev.harmonic, cfg.maxDeltaPerPlan.harmonic);
    if (current.appliedMask.limiter)
        total += clampDeltaArray(current.projectedTargets.limiter, prev.limiter, cfg.maxDeltaPerPlan.limiter);
    if (current.appliedMask.loudness)
        total += clampDeltaArray(current.projectedTargets.loudness, prev.loudness, cfg.maxDeltaPerPlan.loudness);
    return total;
}

NeuralMasteringSafetyPolicy::NeuralMasteringSafetyPolicy(NeuralMasteringSafetyPolicyConfig config) noexcept
    : config_(config)
{
}

NeuralMasteringValidationResult NeuralMasteringSafetyPolicy::validate(const NeuralMasteringPlanCandidate& candidate,
                                                                       const NeuralMasteringRuntimeState& runtimeState) noexcept
{
    NeuralMasteringValidationResult result;
    result.plan.sourcePlanId = candidate.planId;
    result.plan.projectedTargets = candidate.targets;
    result.plan.appliedMask = candidate.editableMask;
    result.plan.fallbackMode = NeuralMasteringFallbackMode::None;
    result.plan.gateResults = makeDefaultGates();
    result.plan.evidenceLevel = candidate.evidenceLevel;
    // AUDIT-FIX: propagate the compressor sidecar from the candidate so the
    // verdict preserves the model's full per-band params. Previously dropped.
    result.plan.compParams    = candidate.compParams;
    result.plan.hasCompParams = candidate.hasCompParams;
    result.plan.capturedAtSteadyClockNs = candidate.capturedAtSteadyClockNs;

    if (candidate.schemaVersion != config_.planSchemaVersion)
        result.addIssue(NeuralMasteringValidationIssue::SchemaVersionMismatch);

    if (!runtimeModeAllowed(candidate.runtimeMode))
        result.addIssue(NeuralMasteringValidationIssue::AudioCallbackRuntime);

    if (candidate.expiresAfterFrame < candidate.producedAtFrame || candidate.producedAtFrame > runtimeState.currentFrame)
        result.addIssue(NeuralMasteringValidationIssue::InvalidTimestamp);
    else if (candidate.expiresAfterFrame < runtimeState.currentFrame
             || runtimeState.currentFrame - candidate.producedAtFrame > config_.maxPlanAgeFrames)
        result.addIssue(NeuralMasteringValidationIssue::StalePlan);

    if (!std::isfinite(candidate.confidence)
        || !allFinite(candidate.targets)
        || !allFinite(candidate.deltas))
    {
        result.addIssue(NeuralMasteringValidationIssue::NonFiniteValue);
    }

    if (std::isfinite(candidate.confidence)
        && (candidate.confidence < 0.0f || candidate.confidence > 1.0f))
        result.addIssue(NeuralMasteringValidationIssue::NonFiniteValue);

    if (std::isfinite(candidate.confidence) && candidate.confidence < config_.minConfidence)
        result.addIssue(NeuralMasteringValidationIssue::LowConfidence);

    if (candidate.abstain)
        result.addIssue(NeuralMasteringValidationIssue::Abstain);

    if (candidate.reviewOnly)
        result.addIssue(NeuralMasteringValidationIssue::ReviewOnly);

    if (!layoutSupported(runtimeState.layout, config_))
        result.addIssue(NeuralMasteringValidationIssue::UnsupportedLayout);

    if (allFinite(candidate.targets) && !inRange(candidate.targets, config_.minTargets, config_.maxTargets))
        result.addIssue(NeuralMasteringValidationIssue::TargetOutOfRange);

    if (allFinite(candidate.deltas) && hasDeltaOutOfRange(candidate.deltas))
        result.addIssue(NeuralMasteringValidationIssue::DeltaOutOfRange);

    if (!candidate.editableMask.any())
        result.addIssue(NeuralMasteringValidationIssue::IllegalMask);

    if (hasHighRiskMask(candidate.editableMask, config_.highRiskControls))
        result.addIssue(NeuralMasteringValidationIssue::HighRiskMask);

    if (hasAnyHardRejectIssue(result))
    {
        applyFallback(result, NeuralMasteringFallbackMode::Reject, lastSafePlan_, hasLastSafePlan_);
        populateRuntimeGates(result);
        return result;
    }

    if (hasIssue(result, NeuralMasteringValidationIssue::StalePlan))
    {
        applyFallback(result,
                      chooseRequestedFallback(candidate, NeuralMasteringFallbackMode::LastSafeHold),
                      lastSafePlan_,
                      hasLastSafePlan_);
        populateRuntimeGates(result);
        return result;
    }

    if (hasIssue(result, NeuralMasteringValidationIssue::Abstain))
    {
        applyFallback(result,
                      chooseRequestedFallback(candidate, NeuralMasteringFallbackMode::DeterministicBaseline),
                      lastSafePlan_,
                      hasLastSafePlan_);
        populateRuntimeGates(result);
        return result;
    }

    if (hasIssue(result, NeuralMasteringValidationIssue::ReviewOnly)
        || hasIssue(result, NeuralMasteringValidationIssue::LowConfidence)
        || runtimeState.overload)
    {
        applyFallback(result,
                      chooseRequestedFallback(candidate,
                                              runtimeState.overload
                                                  ? NeuralMasteringFallbackMode::LastSafeHold
                                                  : NeuralMasteringFallbackMode::ReviewOnly),
                      lastSafePlan_,
                      hasLastSafePlan_);
        populateRuntimeGates(result);
        return result;
    }

    MasteringTargetVector previous {};
    if (hasLastSafePlan_)
        previous = lastSafePlan_.projectedTargets;

    bool projected = false;
    MasteringTargetVector projectedTargets {};
    projectMaskedTargets(projectedTargets, previous, candidate, config_, projected);
    if (projected)
        result.addIssue(NeuralMasteringValidationIssue::MaxDeltaProjected);

    result.accepted = true;
    result.fallbackSelected = false;
    result.plan.valid = true;
    result.plan.projected = projected;
    result.plan.projectedTargets = projectedTargets;
    result.plan.appliedMask = candidate.editableMask;
    result.plan.fallbackMode = NeuralMasteringFallbackMode::None;
    populateRuntimeGates(result);

    lastSafePlan_ = result.plan;
    hasLastSafePlan_ = true;
    return result;
}

void NeuralMasteringSafetyPolicy::clearLastSafePlan() noexcept
{
    lastSafePlan_ = {};
    hasLastSafePlan_ = false;
}

void NeuralMasteringSafetyPolicy::setLastSafePlan(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    lastSafePlan_ = plan;
    hasLastSafePlan_ = plan.valid;
}

} // namespace more_phi
