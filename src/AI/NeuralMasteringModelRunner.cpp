#include "NeuralMasteringModelRunner.h"

#include <algorithm>
#include <cmath>

namespace more_phi {
namespace {

float finiteOrZero(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

} // namespace

NeuralMasteringPlannerResult
NullNeuralMasteringModelRunner::proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept
{
    NeuralMasteringPlannerResult result;
    result.producedCandidate = false;
    result.usedModel = false;
    result.fallbackMode = NeuralMasteringFallbackMode::DeterministicBaseline;
    result.candidate.producedAtFrame = frame.frameIndex;
    result.candidate.expiresAfterFrame = frame.frameIndex;
    result.candidate.abstain = true;
    return result;
}

NeuralMasteringPlannerResult
DeterministicBaselineNeuralMasteringRunner::proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept
{
    NeuralMasteringPlannerResult result;
    result.producedCandidate = true;
    result.usedModel = false;
    result.fallbackMode = NeuralMasteringFallbackMode::None;

    auto& candidate = result.candidate;
    candidate.schemaVersion = kNeuralMasteringPlanSchemaVersion;
    candidate.runtimeMode = NeuralMasteringRuntimeMode::Background;
    candidate.producedAtFrame = frame.frameIndex;
    candidate.expiresAfterFrame = frame.frameIndex + 96000;
    candidate.confidence = 0.82f;
    candidate.evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    candidate.editableMask.eq = true;
    candidate.editableMask.dynamics = true;
    candidate.editableMask.stereo = true;
    candidate.editableMask.loudness = true;

    const float spectralCorrection = std::clamp(-finiteOrZero(frame.spectralTilt) / 24.0f, -0.10f, 0.10f);
    const float loudnessCorrection = std::clamp((-14.0f - finiteOrZero(frame.integratedLUFS)) / 120.0f, -0.08f, 0.08f);
    const float stereoCorrection = frame.channelCount == 1 ? -0.05f : 0.02f;

    candidate.targets.eq[0] = spectralCorrection;
    candidate.deltas.eq[0] = spectralCorrection;
    candidate.targets.dynamics[0] = std::clamp(finiteOrZero(frame.crestFactorDb) / 120.0f, -0.06f, 0.06f);
    candidate.deltas.dynamics[0] = candidate.targets.dynamics[0];
    candidate.targets.stereo[0] = stereoCorrection;
    candidate.deltas.stereo[0] = stereoCorrection;
    candidate.targets.loudness[0] = loudnessCorrection;
    candidate.deltas.loudness[0] = loudnessCorrection;

    return result;
}

} // namespace more_phi
