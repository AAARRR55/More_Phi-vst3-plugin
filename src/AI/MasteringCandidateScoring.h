/*
 * More-Phi — AI/MasteringCandidateScoring.h
 *
 * Small deterministic scorer for completed offline mastering renders.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace more_phi {

struct RenderCandidateMetrics
{
    bool success = false;
    float peakDb = 0.0f;
    float rmsDb = -100.0f;
    bool hasSilence = false;
    bool hasClipping = false;
};

inline float scoreRenderedMasteringCandidate(const RenderCandidateMetrics& candidate) noexcept
{
    if (!candidate.success)
        return 0.0f;

    float score = 1.0f;
    if (candidate.hasClipping)
        score -= 0.45f;
    if (candidate.hasSilence)
        score -= 0.35f;

    if (!std::isfinite(candidate.peakDb))
        score -= 0.35f;
    else if (candidate.peakDb > -0.1f)
        score -= 0.20f;
    else if (candidate.peakDb > -1.0f)
        score -= 0.05f;

    if (!std::isfinite(candidate.rmsDb))
        score -= 0.25f;
    else if (candidate.rmsDb < -60.0f)
        score -= 0.20f;
    else if (candidate.rmsDb < -40.0f)
        score -= 0.10f;

    return std::clamp(score, 0.0f, 1.0f);
}

} // namespace more_phi
