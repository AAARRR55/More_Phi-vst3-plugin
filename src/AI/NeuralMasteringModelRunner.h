#pragma once

#include "Core/NeuralMasteringTypes.h"

namespace more_phi {

struct NeuralMasteringPlannerResult
{
    bool producedCandidate = false;
    bool usedModel = false;
    NeuralMasteringPlanCandidate candidate {};
    NeuralMasteringFallbackMode fallbackMode = NeuralMasteringFallbackMode::DeterministicBaseline;
};

class INeuralMasteringModelRunner
{
public:
    virtual ~INeuralMasteringModelRunner() = default;

    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
    [[nodiscard]] virtual bool usesExternalInference() const noexcept = 0;
    [[nodiscard]] virtual NeuralMasteringPlannerResult proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept = 0;
};

class NullNeuralMasteringModelRunner final : public INeuralMasteringModelRunner
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return false; }
    [[nodiscard]] bool usesExternalInference() const noexcept override { return false; }
    [[nodiscard]] NeuralMasteringPlannerResult proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept override;
};

class DeterministicBaselineNeuralMasteringRunner final : public INeuralMasteringModelRunner
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return true; }
    [[nodiscard]] bool usesExternalInference() const noexcept override { return false; }
    [[nodiscard]] NeuralMasteringPlannerResult proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept override;
};

} // namespace more_phi
