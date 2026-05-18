#include "NeuralMasteringPlanSmoother.h"

#include <algorithm>
#include <cmath>

namespace more_phi {
namespace {

template <std::size_t N>
void smoothArray(std::array<float, N>& current,
                 const std::array<float, N>& target,
                 const std::array<float, N>& maxStep) noexcept
{
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        const float step = std::max(0.0f, maxStep[i]);
        const float delta = std::clamp(target[i] - current[i], -step, step);
        current[i] += delta;
    }
}

template <std::size_t N>
bool withinDelta(const std::array<float, N>& from,
                 const std::array<float, N>& to,
                 const std::array<float, N>& maxDelta) noexcept
{
    for (std::size_t i = 0; i < from.size(); ++i)
        if (std::abs(to[i] - from[i]) > maxDelta[i])
            return false;

    return true;
}

} // namespace

void NeuralMasteringPlanSmoother::reset(const MasteringTargetVector& value) noexcept
{
    current_ = value;
}

MasteringTargetVector NeuralMasteringPlanSmoother::smoothTowards(const MasteringTargetVector& target,
                                                                 const MasteringTargetVector& maxStep) noexcept
{
    smoothArray(current_.eq, target.eq, maxStep.eq);
    smoothArray(current_.dynamics, target.dynamics, maxStep.dynamics);
    smoothArray(current_.stereo, target.stereo, maxStep.stereo);
    smoothArray(current_.harmonic, target.harmonic, maxStep.harmonic);
    smoothArray(current_.limiter, target.limiter, maxStep.limiter);
    smoothArray(current_.loudness, target.loudness, maxStep.loudness);
    return current_;
}

bool NeuralMasteringPlanSmoother::isTransitionWithinMaxDelta(const MasteringTargetVector& from,
                                                            const MasteringTargetVector& to,
                                                            const MasteringTargetVector& maxDelta) noexcept
{
    return withinDelta(from.eq, to.eq, maxDelta.eq)
        && withinDelta(from.dynamics, to.dynamics, maxDelta.dynamics)
        && withinDelta(from.stereo, to.stereo, maxDelta.stereo)
        && withinDelta(from.harmonic, to.harmonic, maxDelta.harmonic)
        && withinDelta(from.limiter, to.limiter, maxDelta.limiter)
        && withinDelta(from.loudness, to.loudness, maxDelta.loudness);
}

} // namespace more_phi
