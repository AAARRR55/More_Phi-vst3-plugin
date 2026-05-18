#pragma once

#include "NeuralMasteringTypes.h"

namespace more_phi {

class NeuralMasteringPlanSmoother
{
public:
    void reset(const MasteringTargetVector& value) noexcept;

    [[nodiscard]] const MasteringTargetVector& getCurrent() const noexcept { return current_; }

    [[nodiscard]] MasteringTargetVector smoothTowards(const MasteringTargetVector& target,
                                                      const MasteringTargetVector& maxStep) noexcept;

    [[nodiscard]] static bool isTransitionWithinMaxDelta(const MasteringTargetVector& from,
                                                         const MasteringTargetVector& to,
                                                         const MasteringTargetVector& maxDelta) noexcept;

private:
    MasteringTargetVector current_ {};
};

} // namespace more_phi
