#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "Core/NeuralMasteringSafetyPolicy.h"

namespace {

more_phi::NeuralMasteringRuntimeState stereoRuntime(std::uint64_t currentFrame = 1000)
{
    more_phi::NeuralMasteringRuntimeState runtime;
    runtime.currentFrame = currentFrame;
    runtime.sampleRate = 48000.0;
    runtime.channelCount = 2;
    runtime.layout = more_phi::NeuralMasteringLayout::Stereo;
    return runtime;
}

more_phi::NeuralMasteringPlanCandidate validCandidate(std::uint64_t planId = 1)
{
    more_phi::NeuralMasteringPlanCandidate candidate;
    candidate.schemaVersion = more_phi::kNeuralMasteringPlanSchemaVersion;
    candidate.planId = planId;
    candidate.runtimeMode = more_phi::NeuralMasteringRuntimeMode::Background;
    candidate.producedAtFrame = 900;
    candidate.expiresAfterFrame = 1100;
    candidate.confidence = 0.9f;
    candidate.abstain = false;
    candidate.reviewOnly = false;
    candidate.evidenceLevel = more_phi::NeuralMasteringEvidenceLevel::Planning;
    candidate.editableMask.eq = true;
    candidate.editableMask.dynamics = true;
    candidate.editableMask.stereo = true;
    candidate.targets.eq[0] = 0.2f;
    candidate.targets.dynamics[0] = -0.1f;
    candidate.targets.stereo[0] = 0.05f;
    candidate.deltas.eq[0] = 0.05f;
    candidate.deltas.dynamics[0] = -0.04f;
    candidate.deltas.stereo[0] = 0.03f;
    return candidate;
}

more_phi::ValidatedNeuralMasteringPlan lastSafePlan()
{
    more_phi::ValidatedNeuralMasteringPlan plan;
    plan.sourcePlanId = 77;
    plan.valid = true;
    plan.projectedTargets.eq[0] = 0.33f;
    plan.appliedMask.eq = true;
    plan.fallbackMode = more_phi::NeuralMasteringFallbackMode::None;
    return plan;
}

bool hasGateStatus(const more_phi::NeuralMasteringValidationResult& result,
                   more_phi::NeuralMasteringGateId gateId,
                   more_phi::NeuralMasteringGateStatus status)
{
    const auto& gate = result.plan.gateResults[static_cast<std::size_t>(gateId)];
    return gate.gateId == gateId && gate.status == status;
}

} // namespace

TEST_CASE("NeuralMasteringSafety rejects malformed candidates", "[NeuralMasteringSafety][US1]")
{
    more_phi::NeuralMasteringSafetyPolicy policy;
    auto runtime = stereoRuntime();

    SECTION("schema mismatch is rejected")
    {
        auto candidate = validCandidate();
        candidate.schemaVersion = more_phi::kNeuralMasteringPlanSchemaVersion + 1;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK_FALSE(result.plan.valid);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::SchemaVersionMismatch));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G06, more_phi::NeuralMasteringGateStatus::Fail));
    }

    SECTION("NaN target is rejected")
    {
        auto candidate = validCandidate();
        candidate.targets.eq[0] = std::numeric_limits<float>::quiet_NaN();

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::NonFiniteValue));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G06, more_phi::NeuralMasteringGateStatus::Fail));
    }

    SECTION("Inf delta is rejected")
    {
        auto candidate = validCandidate();
        candidate.deltas.stereo[0] = std::numeric_limits<float>::infinity();

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::NonFiniteValue));
    }

    SECTION("out-of-range targets are rejected")
    {
        auto candidate = validCandidate();
        candidate.targets.dynamics[0] = 1.25f;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::TargetOutOfRange));
    }

    SECTION("empty editable masks are illegal")
    {
        auto candidate = validCandidate();
        candidate.editableMask = {};

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::IllegalMask));
    }
}

TEST_CASE("NeuralMasteringSafety handles confidence and runtime gating", "[NeuralMasteringSafety][US1]")
{
    more_phi::NeuralMasteringSafetyPolicy policy;
    auto runtime = stereoRuntime();

    SECTION("low confidence falls back to review-only")
    {
        auto candidate = validCandidate();
        candidate.confidence = 0.2f;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.fallbackSelected);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::ReviewOnly);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::LowConfidence));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G09, more_phi::NeuralMasteringGateStatus::Pass));
    }

    SECTION("abstain falls back to deterministic baseline")
    {
        auto candidate = validCandidate();
        candidate.abstain = true;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::DeterministicBaseline);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::Abstain));
    }

    SECTION("review-only candidates are not accepted for application")
    {
        auto candidate = validCandidate();
        candidate.reviewOnly = true;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::ReviewOnly);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::ReviewOnly));
    }

    SECTION("stale plans hold the last safe plan")
    {
        policy.setLastSafePlan(lastSafePlan());
        auto candidate = validCandidate();
        candidate.expiresAfterFrame = 999;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::LastSafeHold);
        CHECK(result.plan.sourcePlanId == 77);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::StalePlan));
    }

    SECTION("unsupported layouts are rejected without changing last-safe state")
    {
        auto candidate = validCandidate();
        runtime.layout = more_phi::NeuralMasteringLayout::Wider;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK_FALSE(policy.hasLastSafePlan());
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::UnsupportedLayout));
    }

    SECTION("audio callback runtime is prohibited")
    {
        auto candidate = validCandidate();
        candidate.runtimeMode = more_phi::NeuralMasteringRuntimeMode::AudioCallbackProhibited;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::AudioCallbackRuntime));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G04, more_phi::NeuralMasteringGateStatus::Fail));
    }
}

TEST_CASE("NeuralMasteringSafety projects bounded plans and selects fallback modes", "[NeuralMasteringSafety][US1]")
{
    more_phi::NeuralMasteringSafetyPolicy policy;
    auto runtime = stereoRuntime();

    SECTION("max delta projection preserves an accepted bounded plan")
    {
        auto candidate = validCandidate();
        candidate.targets.eq[0] = 0.9f;
        candidate.deltas.eq[0] = 0.4f;

        const auto result = policy.validate(candidate, runtime);

        CHECK(result.accepted);
        CHECK(result.plan.valid);
        CHECK(result.plan.projected);
        CHECK(result.plan.projectedTargets.eq[0] == 0.15f);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::MaxDeltaProjected));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G07, more_phi::NeuralMasteringGateStatus::Pass));
    }

    SECTION("high-risk controls are blocked by mask policy")
    {
        auto candidate = validCandidate();
        candidate.editableMask.limiter = true;
        candidate.targets.limiter[0] = 0.4f;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK(result.hasIssue(more_phi::NeuralMasteringValidationIssue::HighRiskMask));
    }

    SECTION("last safe hold preserves the prior plan when available")
    {
        policy.setLastSafePlan(lastSafePlan());
        auto candidate = validCandidate();
        candidate.confidence = 0.1f;
        candidate.requestedFallbackMode = more_phi::NeuralMasteringFallbackMode::LastSafeHold;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::LastSafeHold);
        CHECK(result.plan.sourcePlanId == 77);
        CHECK(result.plan.projectedTargets.eq[0] == 0.33f);
    }

    SECTION("deterministic baseline fallback is observable")
    {
        auto candidate = validCandidate();
        candidate.abstain = true;
        candidate.requestedFallbackMode = more_phi::NeuralMasteringFallbackMode::DeterministicBaseline;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::DeterministicBaseline);
        CHECK(result.plan.projectedTargets.eq[0] == 0.0f);
    }

    SECTION("transparent bypass fallback is observable")
    {
        auto candidate = validCandidate();
        candidate.confidence = 0.1f;
        candidate.requestedFallbackMode = more_phi::NeuralMasteringFallbackMode::TransparentBypass;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::TransparentBypass);
        CHECK_FALSE(result.plan.appliedMask.any());
    }

    SECTION("explicit reject fallback does not update last-safe state")
    {
        auto candidate = validCandidate();
        candidate.confidence = 0.1f;
        candidate.requestedFallbackMode = more_phi::NeuralMasteringFallbackMode::Reject;

        const auto result = policy.validate(candidate, runtime);

        CHECK_FALSE(result.accepted);
        CHECK(result.plan.fallbackMode == more_phi::NeuralMasteringFallbackMode::Reject);
        CHECK_FALSE(policy.hasLastSafePlan());
    }

    SECTION("valid plans update last-safe state")
    {
        auto candidate = validCandidate(42);

        const auto result = policy.validate(candidate, runtime);

        REQUIRE(result.accepted);
        REQUIRE(policy.hasLastSafePlan());
        CHECK(policy.getLastSafePlan().sourcePlanId == 42);
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G06, more_phi::NeuralMasteringGateStatus::Pass));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G08, more_phi::NeuralMasteringGateStatus::NotMeasured));
        CHECK(hasGateStatus(result, more_phi::NeuralMasteringGateId::G09, more_phi::NeuralMasteringGateStatus::Pass));
    }
}
