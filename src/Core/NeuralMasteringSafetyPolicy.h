#pragma once

#include "NeuralMasteringTypes.h"

namespace more_phi {

struct NeuralMasteringSafetyPolicyConfig
{
    std::uint32_t schemaVersion = kNeuralMasteringSchemaVersion;
    std::uint32_t planSchemaVersion = kNeuralMasteringPlanSchemaVersion;
    float minConfidence = 0.75f;
    std::uint64_t maxPlanAgeFrames = 96000;
    MasteringTargetVector minTargets {};
    MasteringTargetVector maxTargets {};
    MasteringTargetVector maxDeltaPerPlan {};
    MasteringControlMask highRiskControls {};
    bool supportsMono = true;
    bool supportsStereo = true;
    bool supportsWider = false;
};

struct NeuralMasteringValidationResult
{
    ValidatedNeuralMasteringPlan plan {};
    std::array<NeuralMasteringValidationIssue, kNeuralMasteringIssueCapacity> issues {};
    std::size_t issueCount = 0;
    bool accepted = false;
    bool fallbackSelected = false;

    void addIssue(NeuralMasteringValidationIssue issue) noexcept
    {
        if (issueCount < issues.size())
            issues[issueCount++] = issue;
    }

    [[nodiscard]] bool hasIssue(NeuralMasteringValidationIssue issue) const noexcept
    {
        for (std::size_t i = 0; i < issueCount; ++i)
            if (issues[i] == issue)
                return true;
        return false;
    }
};

class NeuralMasteringSafetyPolicy
{
public:
    static NeuralMasteringSafetyPolicyConfig defaultConfig() noexcept;

    explicit NeuralMasteringSafetyPolicy(NeuralMasteringSafetyPolicyConfig config = defaultConfig()) noexcept;

    [[nodiscard]] NeuralMasteringValidationResult validate(const NeuralMasteringPlanCandidate& candidate,
                                                           const NeuralMasteringRuntimeState& runtimeState) noexcept;

    void clearLastSafePlan() noexcept;
    void setLastSafePlan(const ValidatedNeuralMasteringPlan& plan) noexcept;

    [[nodiscard]] bool hasLastSafePlan() const noexcept { return hasLastSafePlan_; }
    [[nodiscard]] const ValidatedNeuralMasteringPlan& getLastSafePlan() const noexcept { return lastSafePlan_; }
    [[nodiscard]] const NeuralMasteringSafetyPolicyConfig& getConfig() const noexcept { return config_; }

    // AUDIT-FIX (F4.1, 2026-06-27): enforce the per-cycle delta caps against the
    // last safe plan's projectedTargets, mutating `current.projectedTargets` in
    // place. Caps run at DECODE time inside validate() (SonicMasterAnalysisEngine
    // runCycle/requestDecisionNow), but applyValidatedPlan can be reached by a
    // direct in-process caller that hand-builds a ValidatedNeuralMasteringPlan
    // without going through validate() — that caller would otherwise bypass the
    // 0.6 LU/cycle loudness slew limit. This method makes the cap invariant hold
    // regardless of the entry point. Returns the number of dimensions clamped
    // (for telemetry); 0 when there is no last-safe baseline (first apply).
    std::size_t enforceDeltaCaps(ValidatedNeuralMasteringPlan& current,
                                 const ValidatedNeuralMasteringPlan& lastSafePlan,
                                 bool hasLastSafePlan) noexcept;

private:
    NeuralMasteringSafetyPolicyConfig config_ {};
    ValidatedNeuralMasteringPlan lastSafePlan_ {};
    bool hasLastSafePlan_ = false;
};

} // namespace more_phi
