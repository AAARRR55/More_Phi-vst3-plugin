/*
 * More-Phi — AI/ChainPlanExecutor.h
 * Extended in v3.3.1 to support OzonePlanApplicator registration.
 *
 * Chain-of-thought multi-effect chain planner.
 *
 * Implements a 5-step CoT reasoning chain:
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
#include <juce_core/juce_core.h>

// Forward declaration — avoids header dependency loop
namespace more_phi { class OzonePlanApplicatorBase; }

namespace more_phi {

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
        ozoneApplicator_ = applicator;
    }

    /** Remove the registered applicator (call before Ozone unload). */
    void clearOzonePlanApplicator() noexcept { ozoneApplicator_ = nullptr; }

    /** True when an Ozone applicator is registered. */
    bool hasOzoneApplicator() const noexcept { return ozoneApplicator_ != nullptr; }

    /**
     * Execute the 5-step CoT chain on the calling thread.
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

    /** Get the last executed plan. Thread-safe (returns a copy). */
    [[nodiscard]] MultiEffectPlan getLastPlan() const noexcept { return lastPlan_; }

private:
    // Step implementations
    MultiEffectPlan stepDynamicsAssessment(float dynamicRange);
    void stepSpectralAssessment(MultiEffectPlan& plan, int genreIndex, float spectralTilt);
    void stepStereoAssessment(MultiEffectPlan& plan, float correlationMS);
    void stepLoudnessTarget(MultiEffectPlan& plan, int genreIndex);
    void stepStageControl(MultiEffectPlan& plan);

    PlanCallback  callback_;
    MultiEffectPlan lastPlan_;
    OzonePlanApplicatorBase* ozoneApplicator_ = nullptr;

    // Genre LUFS targets (matching mastering_profiles.json)
    static constexpr float kGenreLUFS[12] = {
        -9.f, -9.f, -11.f, -13.f, -12.f, -16.f,
        -17.f, -20.f, -18.f, -10.f, -14.f, -23.f
    };
};

} // namespace more_phi
