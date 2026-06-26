/*
 * More-Phi — AI/ChainPlanExecutor.cpp
 *
 * Heuristic multi-effect chain planner, now backed by RuleBasedMasteringResolver.
 */
#include "ChainPlanExecutor.h"
#include "OzonePlanApplicator.h"
#include "RuleBasedMasteringResolver.h"
#include "Core/AutoMasteringEngine.h"   // P1.2: ApplyVerification complete type
#include <algorithm>
#include <cmath>

namespace more_phi {

constexpr float ChainPlanExecutor::kGenreLUFS[12];

MultiEffectPlan ChainPlanExecutor::executePlan(const RuleBasedMasteringInput& input)
{
    MultiEffectPlan plan = buildPlan(input);
    applyPlan(plan);
    return plan;
}

MultiEffectPlan ChainPlanExecutor::executePlan(int   genreIndex,
                                               float dynamicRange,
                                               float spectralTilt,
                                               float correlationMS)
{
    return executePlan(makeInputFromLegacy(genreIndex, dynamicRange, spectralTilt, correlationMS));
}

MultiEffectPlan ChainPlanExecutor::previewPlan(const RuleBasedMasteringInput& input)
{
    return buildPlan(input);
}

MultiEffectPlan ChainPlanExecutor::previewPlan(int   genreIndex,
                                               float dynamicRange,
                                               float spectralTilt,
                                               float correlationMS)
{
    return previewPlan(makeInputFromLegacy(genreIndex, dynamicRange, spectralTilt, correlationMS));
}

int ChainPlanExecutor::applyPlan(const MultiEffectPlan& plan)
{
    if (!plan.valid)
        return 0;

    lastPlan_ = plan;

    if (callback_) callback_(plan);

    const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
    return ozoneApplicator_ != nullptr ? ozoneApplicator_->apply(plan) : 0;
}

OzoneApplyBreakdown ChainPlanExecutor::getLastOzoneApplyBreakdown() const noexcept
{
    const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
    if (ozoneApplicator_ != nullptr)
        return ozoneApplicator_->getLastApplyBreakdown();
    return {};
}

ApplyVerification ChainPlanExecutor::getLastOzoneVerification() const noexcept
{
    const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
    if (ozoneApplicator_ != nullptr)
        return ozoneApplicator_->getLastVerification();
    return {};
}

MultiEffectPlan ChainPlanExecutor::buildPlan(const RuleBasedMasteringInput& input) const
{
    RuleBasedMasteringResolver resolver;
    MultiEffectPlan plan = resolver.resolve(input);

    // If the caller did not supply a target LUFS (legacy path leaves it at the
    // default -14), map the genre index onto the legacy target table for
    // backward compatibility.
    if (std::abs(input.targetLufs - -14.0f) < 0.01f && input.targetCurveName.empty())
    {
        // No-op: the resolver already clamps to [-23, -8].  Legacy callers that
        // want genre-specific targets should set targetLufs explicitly before
        // calling executePlan(const RuleBasedMasteringInput&).
    }

    return plan;
}

RuleBasedMasteringInput ChainPlanExecutor::makeInputFromLegacy(int    genreIndex,
                                                                float  dynamicRange,
                                                                float  /*spectralTilt*/,
                                                                float  correlationMS) const noexcept
{
    RuleBasedMasteringInput input;
    input.lra = dynamicRange;
    input.targetLufs = kGenreLUFS[std::clamp(genreIndex, 0, 11)];
    input.intensity = 0.5f;

    // Correlation is not in the rich input, but we can encode it into the stereo
    // snapshot so the width-curve resolver still behaves like the old heuristic.
    input.stereo.sampleRate = 48000.0;
    input.stereo.stereoWidth = 1.0f - std::clamp(correlationMS, 0.0f, 1.0f);
    for (auto& c : input.stereo.correlation)
        c = correlationMS;

    return input;
}

} // namespace more_phi
