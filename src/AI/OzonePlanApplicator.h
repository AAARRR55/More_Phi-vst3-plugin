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
#include "ChainPlanExecutor.h"   // brings in OzoneApplyBreakdown + MultiEffectPlan (Fix 6)
#include <juce_core/juce_core.h>
#include <cstdint>   // P3.10: std::uint64_t for plan ids

namespace more_phi {

class MorePhiProcessor;

// AUDIT-FIX (Fix 2): verification result returned by
// OzonePlanApplicator::getLastVerification(). Defined in Core/AutoMasteringEngine.h;
// forward-declared here to avoid a header dependency cycle (AutoMasteringEngine.h
// includes ChainPlanExecutor.h, which this header also includes).
struct ApplyVerification;

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
    // AUDIT-FIX (Fix 6): default returns an empty breakdown so existing test stubs
    // don't need to implement it; the production applicator overrides to return the
    // real per-slot detail.
    virtual OzoneApplyBreakdown getLastApplyBreakdown() const noexcept { return {}; }
    // AUDIT-FIX (Fix 2): default returns an empty verification so stubs compile.
    // The production applicator overrides to read back each enqueued param via the
    // hosted-plugin bridge and compare to the requested normalized value with a
    // discrete-aware tolerance.
    virtual ApplyVerification getLastVerification() const noexcept;
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

    // AUDIT-FIX (Fix 6): per-slot breakdown of the last apply() — enqueued/skipped/
    // unmapped/ambiguous counts plus the list of {index, requestedValue} pairs
    // actually enqueued, for readback verification by the caller.
    OzoneApplyBreakdown getLastApplyBreakdown() const noexcept override { return lastBreakdown_; }

    // AUDIT-FIX (Fix 2): readback verification of the last apply(). For each
    // enqueued {index, requestedValue}, reads the hosted plugin's current normalized
    // value back and compares with a discrete-aware tolerance. Counts verified,
    // driftedDiscrete (discrete params that snapped to a different step), and
    // mismatched (outside tolerance). Empty before the first apply.
    ApplyVerification getLastVerification() const noexcept override;

    // AUDIT-F2.1: static public so the tokenized-match regression can be unit
    // tested without a hosted plugin. It is a pure function over two strings.
    static bool nameMatches(const juce::String& expectedName,
                            const juce::String& actualName) noexcept;

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

    // P2.6 (AUDIT): overload that re-validates the hosted plugin's current
    // parameter name at `idx` against `expectedName` before writing. Used to
    // detect index drift when the hosted plugin is swapped after the map was
    // built — a stale positional index would otherwise write to whatever
    // parameter now occupies that slot in the new plugin. On mismatch the write
    // is skipped (counted as skipped in the breakdown) rather than corrupting an
    // unrelated control. Pass an empty expectedName to skip the check (legacy
    // callers that don't yet supply a name).
    int enqueueIfMapped(int idx, float normalizedValue, const juce::String& expectedName);

    MorePhiProcessor&        processor_;
    const OzoneParameterMap& map_;
    int                      lastAppliedCount_ = 0;
    // P3.10 (AUDIT): monotonic plan id stamped into each enqueuePlanBoundary so
    // the audio thread's getLastDrainedPlanId() can confirm a full plan drained.
    std::uint64_t            lastPlanId_ = 0;
    // AUDIT-FIX (Fix 6): populated by apply() via enqueueIfMapped. Cleared at the
    // start of each apply() so it always reflects the most recent call.
    OzoneApplyBreakdown      lastBreakdown_ {};

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
