#include "NeuralMasteringController.h"

namespace more_phi {
namespace {

bool isFrameUsable(const NeuralMasteringFeatureFrame& frame) noexcept
{
    return frame.schemaVersion == kNeuralMasteringFeatureSchemaVersion
        && frame.sampleRate > 0.0
        && (frame.channelCount == 1 || frame.channelCount == 2)
        && frame.blockSize > 0
        && NeuralMasteringFeatureExtractor::isFrameFinite(frame);
}

void captureFailureGate(NeuralMasteringControllerStatus& status) noexcept
{
    const auto& gates = status.lastValidation.plan.gateResults;
    for (const auto& gate : gates)
    {
        if (gate.status == NeuralMasteringGateStatus::Fail)
        {
            status.lastFailureGate = gate.gateId;
            status.lastFailureGateValid = true;
            return;
        }
    }
}

} // namespace

NeuralMasteringController::NeuralMasteringController() noexcept
    : safetyPolicy_(NeuralMasteringSafetyPolicy::defaultConfig()),
      runner_(&nullRunner_)
{
}

NeuralMasteringController::NeuralMasteringController(NeuralMasteringSafetyPolicyConfig config) noexcept
    : safetyPolicy_(config),
      runner_(&nullRunner_)
{
}

void NeuralMasteringController::setModelRunner(INeuralMasteringModelRunner* runner) noexcept
{
    runner_ = runner != nullptr ? runner : &nullRunner_;
}

NeuralMasteringPlannerResult
NeuralMasteringController::makeCandidate(const NeuralMasteringFeatureFrame& frame) noexcept
{
    auto* selectedRunner = runner_ != nullptr ? runner_ : &nullRunner_;
    auto result = selectedRunner->proposePlan(frame);
    lastStatus_.plannerInvoked = true;
    lastStatus_.modelInvoked = result.usedModel || selectedRunner->usesExternalInference();

    if (!result.producedCandidate)
    {
        result = deterministicRunner_.proposePlan(frame);
        lastStatus_.plannerInvoked = true;
        lastStatus_.modelInvoked = false;
    }

    if (result.candidate.planId == 0)
        result.candidate.planId = nextPlanId_++;

    return result;
}

NeuralMasteringControllerStatus
NeuralMasteringController::processFeatureFrame(const NeuralMasteringFeatureFrame& frame,
                                               const NeuralMasteringRuntimeState& runtimeState,
                                               bool applyValidatedPlan) noexcept
{
    lastStatus_ = {};
    lastStatus_.featureFrameValid = isFrameUsable(frame);

    if (!lastStatus_.featureFrameValid)
    {
        lastStatus_.fallbackSelected = true;
        lastStatus_.fallbackMode = NeuralMasteringFallbackMode::Reject;
        return lastStatus_;
    }

    auto plannerResult = makeCandidate(frame);
    lastStatus_.evidenceLevel = plannerResult.candidate.evidenceLevel;
    lastStatus_.lastValidation = safetyPolicy_.validate(plannerResult.candidate, runtimeState);
    lastStatus_.validationAccepted = lastStatus_.lastValidation.accepted;
    lastStatus_.fallbackSelected = lastStatus_.lastValidation.fallbackSelected;
    lastStatus_.fallbackMode = lastStatus_.lastValidation.plan.fallbackMode;
    captureFailureGate(lastStatus_);

    if (lastStatus_.validationAccepted && applyValidatedPlan && applicationEngine_ != nullptr)
    {
        lastStatus_.applicationAttempted = true;
        lastStatus_.applicationAccepted = applicationEngine_->applyValidatedPlan(lastStatus_.lastValidation.plan);
    }

    return lastStatus_;
}

} // namespace more_phi
