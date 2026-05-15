/*
 * More-Phi — AI/ChainPlanExecutor.cpp
 */
#include "ChainPlanExecutor.h"
#include "OzonePlanApplicator.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

constexpr float ChainPlanExecutor::kGenreLUFS[12];

MultiEffectPlan ChainPlanExecutor::executePlan(int   genreIndex,
                                               float dynamicRange,
                                               float spectralTilt,
                                               float correlationMS)
{
    MultiEffectPlan plan;

    // Step 1: Dynamics
    plan = stepDynamicsAssessment(dynamicRange);

    // Step 2: Spectral (EQ)
    stepSpectralAssessment(plan, genreIndex, spectralTilt);

    // Step 3: Stereo
    stepStereoAssessment(plan, correlationMS);

    // Step 4: Loudness target
    stepLoudnessTarget(plan, genreIndex);

    // Step 5: Stage enable/disable
    stepStageControl(plan);

    plan.valid = true;
    lastPlan_  = plan;

    if (callback_) callback_(plan);

    // Apply to Ozone 11 if a hosted instance is registered
    if (ozoneApplicator_) ozoneApplicator_->apply(plan);

    return plan;
}

MultiEffectPlan ChainPlanExecutor::stepDynamicsAssessment(float dynamicRange)
{
    MultiEffectPlan plan;

    // Dynamic range heuristic:
    //   LRA < 4 LU  → already very compressed, use gentle compression
    //   LRA 4–9 LU  → moderate compression
    //   LRA > 9 LU  → aggressive compression beneficial
    if (dynamicRange < 4.f)
        plan.compressionNeed = 0.2f;
    else if (dynamicRange < 9.f)
        plan.compressionNeed = 0.5f;
    else
        plan.compressionNeed = 0.8f;

    plan.useNeuralComp = (dynamicRange > 6.f);
    return plan;
}

void ChainPlanExecutor::stepSpectralAssessment(MultiEffectPlan& plan,
                                               int genreIndex,
                                               float spectralTilt)
{
    // Build simple EQ prescription based on genre + spectral tilt
    // Negative tilt = too dark (boost highs), positive = too bright (cut highs)
    const float highGain = std::clamp(-spectralTilt * 0.5f, -3.f, 3.f);

    // Construct JSON EQ prescription for bands 0-7
    juce::String json = "{ \"bands\": [";
    json += "{ \"freq\": 80, \"gain\": " +
            juce::String(genreIndex <= 2 ? 1.5f : 0.5f, 1) +
            ", \"Q\": 0.7, \"type\": \"lowshelf\" }";
    json += ", { \"freq\": 250, \"gain\": -0.5, \"Q\": 1.2, \"type\": \"peak\" }";
    json += ", { \"freq\": 800, \"gain\": 0.0, \"Q\": 1.0, \"type\": \"peak\" }";
    json += ", { \"freq\": 2000, \"gain\": 0.5, \"Q\": 1.0, \"type\": \"peak\" }";
    json += ", { \"freq\": 5000, \"gain\": " + juce::String(highGain * 0.5f, 1) +
            ", \"Q\": 1.0, \"type\": \"peak\" }";
    json += ", { \"freq\": 8000, \"gain\": " + juce::String(highGain * 0.7f, 1) +
            ", \"Q\": 0.9, \"type\": \"peak\" }";
    json += ", { \"freq\": 12000, \"gain\": " + juce::String(highGain, 1) +
            ", \"Q\": 0.7, \"type\": \"highshelf\" }";
    json += ", { \"freq\": 18000, \"gain\": " + juce::String(highGain * 0.5f, 1) +
            ", \"Q\": 0.7, \"type\": \"highshelf\" }";
    json += "] }";

    plan.eqPrescriptionJSON = json;
}

void ChainPlanExecutor::stepStereoAssessment(MultiEffectPlan& plan, float correlationMS)
{
    // correlationMS close to 1.0 = very mono → widen
    // correlationMS close to 0.0 = moderate stereo → standard curve
    // correlationMS < 0 = problematic phase → narrow

    if (correlationMS > 0.8f)
    {
        // Narrow mix — open it up
        plan.widthCurve[0] = 0.0f;
        plan.widthCurve[1] = 0.6f;
        plan.widthCurve[2] = 1.2f;
        plan.widthCurve[3] = 1.6f;
    }
    else if (correlationMS < 0.2f)
    {
        // Wide / phasey — tighten
        plan.widthCurve[0] = 0.0f;
        plan.widthCurve[1] = 0.5f;
        plan.widthCurve[2] = 0.8f;
        plan.widthCurve[3] = 1.0f;
    }
    else
    {
        // Standard
        plan.widthCurve[0] = 0.0f;
        plan.widthCurve[1] = 0.6f;
        plan.widthCurve[2] = 1.0f;
        plan.widthCurve[3] = 1.4f;
    }
}

void ChainPlanExecutor::stepLoudnessTarget(MultiEffectPlan& plan, int genreIndex)
{
    const int clampedIdx = std::clamp(genreIndex, 0, 11);
    plan.targetLUFS  = kGenreLUFS[clampedIdx];
    plan.ceilingDBTP = -1.0f;
}

void ChainPlanExecutor::stepStageControl(MultiEffectPlan& plan)
{
    // Enable exciter for brighter/harder genres
    plan.exciterEnabled = (plan.compressionNeed > 0.6f);
}

} // namespace more_phi
