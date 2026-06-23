/*
 * More-Phi — AI/OzonePlanApplicator.h
 *
 * Translates a MultiEffectPlan into Ozone 11 parameter changes delivered
 * through MorePhiProcessor::enqueueParameterSet().
 *
 * Runs on the message thread (called from ChainPlanExecutor callback).
 * All parameter delivery is non-blocking — changes are queued to the
 * audio thread via the existing LockFreeQueue pathway.
 *
 * Gracefully skips any parameter whose index is -1 in OzoneParameterMap,
 * so the code compiles and runs safely before the Phase 1 audit is complete.
 */
#pragma once

#include "OzoneParameterMap.h"
#include "ChainPlanExecutor.h"
#include <juce_core/juce_core.h>

namespace more_phi {

class MorePhiProcessor;

/**
 * Abstract interface for Ozone plan applicators.
 * Used by ChainPlanExecutor to allow mock injection in tests.
 */
class OzonePlanApplicatorBase
{
public:
    virtual ~OzonePlanApplicatorBase() = default;
    virtual int apply(const MultiEffectPlan& plan) = 0;
    virtual int getLastAppliedCount() const noexcept = 0;
};

class OzonePlanApplicator : public OzonePlanApplicatorBase
{
public:
    OzonePlanApplicator(MorePhiProcessor& processor, const OzoneParameterMap& map) noexcept;

    /**
     * Translate all fields of the given plan to Ozone parameter values and
     * enqueue them via MorePhiProcessor::enqueueParameterSet().
     *
     * Parameters with index == -1 in the map are silently skipped.
     * Returns the number of parameters actually enqueued.
     *
     * AUDIT-FIX-5: if the map is all-stubs (no audit_ozone_parameters run yet),
     * this logs a loud warning so the silent no-op cannot be mistaken for a
     * successful apply. The return count is still 0; callers that care should
     * prefer isReady() before calling apply().
     */
    int apply(const MultiEffectPlan& plan) override;

    /** AUDIT-FIX-5: true iff the map has at least one mapped parameter. */
    [[nodiscard]] bool isReady() const noexcept { return map_.hasAnyMapping(); }

    /** Read back the number of parameters applied in the last apply() call. */
    int getLastAppliedCount() const noexcept override { return lastAppliedCount_; }

private:
    /** Apply EQ prescription (parses plan.eqPrescriptionJSON). */
    int applyEQ(const MultiEffectPlan& plan);

    /** Apply dynamics (compressionNeed → threshold/ratio/attack/release). */
    int applyDynamics(const MultiEffectPlan& plan);

    /** Apply stereo imager (widthCurve[4] → per-band Ozone Imager width). */
    int applyStereoImager(const MultiEffectPlan& plan);

    /** Apply maximizer (targetLUFS → output level, ceilingDBTP → ceiling). */
    int applyMaximizer(const MultiEffectPlan& plan);

    /** Enqueue a single parameter if idx != -1. Returns 1 on success, 0 if skipped. */
    int enqueueIfMapped(int idx, float normalizedValue);

    MorePhiProcessor&        processor_;
    const OzoneParameterMap& map_;
    int                      lastAppliedCount_ = 0;

    // Dynamics mapping constants
    // compressionNeed [0..1] → threshold dBFS in this range
    static constexpr float kThresholdAtMinNeed = -10.0f;  // gentle: -10 dBFS
    static constexpr float kThresholdAtMaxNeed = -30.0f;  // aggressive: -30 dBFS

    // compressionNeed [0..1] → ratio in this range
    static constexpr float kRatioAtMinNeed = 1.5f;
    static constexpr float kRatioAtMaxNeed = 6.0f;

    // Ozone ratio param range for normalization (estimated; verify with audit)
    static constexpr float kOzoneRatioMin = 1.0f;
    static constexpr float kOzoneRatioMax = 20.0f;

    // Attack/release defaults when not overridden by plan
    static constexpr float kDefaultAttackMs   = 10.0f;
    static constexpr float kDefaultReleaseMs  = 100.0f;
    static constexpr float kOzoneAttackMin    =   0.1f;
    static constexpr float kOzoneAttackMax    = 100.0f;
    static constexpr float kOzoneReleaseMin   =  10.0f;
    static constexpr float kOzoneReleaseMax   = 1000.0f;
};

} // namespace more_phi
