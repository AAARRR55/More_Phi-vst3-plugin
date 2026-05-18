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

private:
    NeuralMasteringSafetyPolicyConfig config_ {};
    ValidatedNeuralMasteringPlan lastSafePlan_ {};
    bool hasLastSafePlan_ = false;
};

} // namespace more_phi
