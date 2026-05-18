#pragma once

#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringModelRunner.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringSafetyPolicy.h"

#include <cstdint>

namespace more_phi {

struct NeuralMasteringControllerStatus
{
    bool featureFrameValid = false;
    bool plannerInvoked = false;
    bool modelInvoked = false;
    bool validationAccepted = false;
    bool fallbackSelected = false;
    bool applicationAttempted = false;
    bool applicationAccepted = false;
    bool lastFailureGateValid = false;
    NeuralMasteringFallbackMode fallbackMode = NeuralMasteringFallbackMode::None;
    NeuralMasteringEvidenceLevel evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    NeuralMasteringGateId lastFailureGate = NeuralMasteringGateId::G01;
    NeuralMasteringValidationResult lastValidation {};
};

class NeuralMasteringController
{
public:
    NeuralMasteringController() noexcept;
    explicit NeuralMasteringController(NeuralMasteringSafetyPolicyConfig config) noexcept;

    void setModelRunner(INeuralMasteringModelRunner* runner) noexcept;
    void setApplicationEngine(AutoMasteringEngine* engine) noexcept { applicationEngine_ = engine; }
    void resetStatus() noexcept { lastStatus_ = {}; }

    [[nodiscard]] NeuralMasteringControllerStatus processFeatureFrame(const NeuralMasteringFeatureFrame& frame,
                                                                      const NeuralMasteringRuntimeState& runtimeState,
                                                                      bool applyValidatedPlan) noexcept;

    [[nodiscard]] const NeuralMasteringControllerStatus& getLastStatus() const noexcept { return lastStatus_; }
    [[nodiscard]] NeuralMasteringSafetyPolicy& getSafetyPolicy() noexcept { return safetyPolicy_; }
    [[nodiscard]] const NeuralMasteringSafetyPolicy& getSafetyPolicy() const noexcept { return safetyPolicy_; }

private:
    NeuralMasteringSafetyPolicy safetyPolicy_;
    NullNeuralMasteringModelRunner nullRunner_;
    DeterministicBaselineNeuralMasteringRunner deterministicRunner_;
    INeuralMasteringModelRunner* runner_ = nullptr;
    AutoMasteringEngine* applicationEngine_ = nullptr;
    NeuralMasteringControllerStatus lastStatus_ {};
    std::uint64_t nextPlanId_ = 1;

    [[nodiscard]] NeuralMasteringPlannerResult makeCandidate(const NeuralMasteringFeatureFrame& frame) noexcept;
};

} // namespace more_phi
