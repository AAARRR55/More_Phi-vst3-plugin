/*
 * More-Phi — AI/ChainPlanExecutor.h
 * Extended in v3.3.1 to support OzonePlanApplicator registration.
 *
 * Heuristic multi-effect chain planner.
 *
 * Implements a 5-step deterministic rule chain:
 *   Step 1: Dynamics Assessment → compression_need, style
 *   Step 2: Spectral Assessment → EQ prescription JSON
 *   Step 3: Stereo Assessment   → width_curve[4]
 *   Step 4: Loudness Target     → target_lufs, ceiling_dbtp
 *   Step 5: Stage Enable/Disable → {exciter, neural_comp, ...}
 *
 * Results are assembled into a MultiEffectPlan struct and pushed to
 * AutoMasteringEngine via a callback.
 *
 * Runs on a background thread (juce::ThreadPool) — never on audio thread.
 *
 * Thread safety:
 *   executePlan() — background thread only.
 *   getLastPlan() — any thread (atomic copy).
 *   setCallback() — any thread before executePlan().
 */
#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <vector>   // AUDIT-FIX (Fix 6): OzoneApplyBreakdown::applied
#include <juce_core/juce_core.h>

// Forward declaration — avoids header dependency loop
namespace more_phi { class OzonePlanApplicatorBase; }

namespace more_phi {

// AUDIT-FIX (Fix 6): breakdown of a single OzonePlanApplicator::apply() call.
// Previously apply() returned only an int (params enqueued), so a 3-of-40 partial
// apply was indistinguishable from a full apply. Defined here (not in
// OzonePlanApplicator.h) because ChainPlanExecutor returns it by value, and
// OzonePlanApplicator.h includes this header. Carries enqueued/skipped/unmapped/
// ambiguous counts AND the {index, requestedValue} pairs actually pushed so the
// caller can do readback verification (Fix 2).
struct OzoneApplyBreakdown
{
    int enqueued = 0;       // successfully pushed to the command queue
    int skipped = 0;        // idx valid but push failed (queue full / throttle)
    int unmapped = 0;       // idx == -1 (no audit mapping for this slot)
    int ambiguous = 0;      // idx == -2 (Fix 3: ambiguous name match)
    struct AppliedParam { int index; float requestedNormalized; };
    std::vector<AppliedParam> applied;   // for readback verification (Fix 2)
};

// AUDIT-FIX (Fix 2): readback verification of the most recent OzonePlanApplicator::
// apply() call. 'requested' = slots the plan tried to write; 'enqueued' = how many
// reached the command queue; 'verified' = how many read back within tolerance after
// the drain; 'driftedDiscrete' = discrete params whose value snapped to a different
// step than requested (expected with setParameterNormalizedSnapped); 'mismatched' =
// params whose readback was outside tolerance for non-snap reasons. Defined here
// (alongside OzoneApplyBreakdown) because ChainPlanExecutor returns it by value.
struct ApplyVerification
{
    int requested = 0;
    int enqueued = 0;
    int verified = 0;
    int driftedDiscrete = 0;
    int mismatched = 0;
    int ambiguous = 0;      // slots the OzoneParameterMap flagged ambiguous (Fix 3)
    int unmapped = 0;       // slots with no audit mapping
    [[nodiscard]] float verifiedFraction() const noexcept
    { return enqueued > 0 ? static_cast<float>(verified) / static_cast<float>(enqueued) : 0.0f; }
};

struct MultiEffectPlan
{
    // Dynamics
    float compressionNeed = 0.5f;    // [0=gentle, 1=aggressive]
    bool  useNeuralComp   = false;

    // EQ
    juce::String eqPrescriptionJSON;  // JSON for EQParameterTranslator

    // Stereo
    float widthCurve[4] = { 0.0f, 0.6f, 1.0f, 1.4f };  // sub/low/mid/high

    // Loudness
    float targetLUFS    = -14.f;
    float ceilingDBTP   =  -1.f;

    // Stage enable flags
    bool  exciterEnabled = false;
    bool  valid          = false;
};

class ChainPlanExecutor
{
public:
    using PlanCallback = std::function<void(const MultiEffectPlan&)>;

    ChainPlanExecutor() = default;

    /** Register callback invoked when a plan is ready. */
    void setPlanCallback(PlanCallback cb) { callback_ = std::move(cb); }

    // ── Ozone integration ─────────────────────────────────────────────────────

    /**
     * Register an OzonePlanApplicatorBase to receive every generated plan.
     * Called from MorePhiProcessor on Ozone 11 load (message thread).
     * The applicator must outlive this executor, or be cleared before destruction.
     */
    void setOzonePlanApplicator(OzonePlanApplicatorBase* applicator) noexcept
    {
        const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
        ozoneApplicator_ = applicator;
    }

    /** Remove the registered applicator (call before Ozone unload). */
    void clearOzonePlanApplicator() noexcept
    {
        const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
        ozoneApplicator_ = nullptr;
    }

    /** True when an Ozone applicator is registered. */
    bool hasOzoneApplicator() const noexcept
    {
        const juce::SpinLock::ScopedLockType guard(ozoneApplicatorLock_);
        return ozoneApplicator_ != nullptr;
    }

    /**
     * Execute the 5-step deterministic rule chain on the calling thread.
     * Call from a background ThreadPool job.
     *
     * @param genreIndex    Current genre index (0–11) from GenreClassifier.
     * @param dynamicRange  Measured dynamic range in LU.
     * @param spectralTilt  Measured spectral tilt in dB/octave.
     * @param correlationMS Measured M/S correlation [-1, 1].
     */
    MultiEffectPlan executePlan(int    genreIndex,
                                float  dynamicRange,
                                float  spectralTilt,
                                float  correlationMS);

    /**
     * Build the same plan as executePlan() without mutating lastPlan_, invoking
     * callbacks, or applying it to any hosted plugin.
     */
    MultiEffectPlan previewPlan(int    genreIndex,
                                float  dynamicRange,
                                float  spectralTilt,
                                float  correlationMS);

    /** Apply an already-built plan through callbacks and registered hosted-plugin applicators. */
    int applyPlan(const MultiEffectPlan& plan);

    // AUDIT-FIX (Fix 6): per-slot breakdown of the most recent applyPlan() call to
    // the registered Ozone applicator — enqueued/skipped/unmapped/ambiguous counts
    // plus the {index, requestedValue} pairs actually pushed. Returns an empty
    // breakdown when no applicator is registered. Used by AutoMasteringEngine::
    // applyValidatedPlan for honest partial-apply reporting and readback (Fix 2).
    [[nodiscard]] OzoneApplyBreakdown getLastOzoneApplyBreakdown() const noexcept;

    // AUDIT-FIX (Fix 2): forward the applicator's readback verification of the most
    // recent apply. Returns an empty ApplyVerification when no applicator is set.
    // ApplyVerification is defined in Core/AutoMasteringEngine.h; forward-declared
    // here to keep this header dependency-light.
    [[nodiscard]] ApplyVerification getLastOzoneVerification() const noexcept;

    /** Get the last executed plan. Thread-safe (returns a copy). */
    [[nodiscard]] MultiEffectPlan getLastPlan() const noexcept { return lastPlan_; }

private:
    MultiEffectPlan buildPlan(int genreIndex,
                              float dynamicRange,
                              float spectralTilt,
                              float correlationMS);

    // Step implementations
    MultiEffectPlan stepDynamicsAssessment(float dynamicRange);
    void stepSpectralAssessment(MultiEffectPlan& plan, int genreIndex, float spectralTilt);
    void stepStereoAssessment(MultiEffectPlan& plan, float correlationMS);
    void stepLoudnessTarget(MultiEffectPlan& plan, int genreIndex);
    void stepStageControl(MultiEffectPlan& plan);

    PlanCallback  callback_;
    MultiEffectPlan lastPlan_;
    mutable juce::SpinLock ozoneApplicatorLock_;
    OzonePlanApplicatorBase* ozoneApplicator_ = nullptr;

    // Genre LUFS targets (matching mastering_profiles.json)
    static constexpr float kGenreLUFS[12] = {
        -9.f, -9.f, -11.f, -13.f, -12.f, -16.f,
        -17.f, -20.f, -18.f, -10.f, -14.f, -23.f
    };
};

} // namespace more_phi
