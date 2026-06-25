/*
 * More-Phi — Core/AutoMasteringEngine.h
 *
 * Top-level orchestrator for the 10-stage automated mastering chain.
 *
 * Stage order:
 *   [1]  MSMatrix::encode         L/R → M/S
 *   [2]  MultibandSplitter        4-band Linkwitz-Riley (80/250/5kHz)
 *   [3]  MultibandDynamicsProcessor  per-band VCA compressor
 *   [4]  Band summation           4 bands → stereo M/S
 *   [5]  AdaptiveEQ               32-band parametric EQ
 *   [6]  StereoImager             freq-dependent M/S width
 *   [7]  HarmonicExciter          optional tanh soft-sat
 *   [8]  BrickwallLimiter         4ms lookahead, ISP detection
 *   [9]  LUFSMeter                integrated LUFS measurement
 *   [10] LoudnessNormalizer       LUFS → target correction gain
 *   [11] MSMatrix::decode         M/S → L/R
 *
 * Intelligence (timers on the message thread):
 *   - EQParameterTranslator applies genre warm-start to EQ bands 0–7
 *   - ChainPlanExecutor runs a 5-step heuristic rule planner every 30s
 *   - GenreClassifier classifies genre every 30s
 *   - LoudnessNormalizer.updateCorrectionGain() called every 100ms
 *   - MonoCompatibilityChecker checks fold-down every 1s
 *
 * Thread safety (THREADSWEEP-2026-06):
 *   - processBlock()       — audio thread, noexcept.
 *   - analyzeBlock()       — AUDIO thread (called throttled from the host's
 *                            processBlock in PluginProcessor). Despite the name
 *                            it is NOT a message-thread path; its meter writes
 *                            are safe only because LUFSMeter /
 *                            RealtimeSpectrumAnalyzer / StereoFieldAnalyzer /
 *                            TruePeakEstimator / MeterWindowAccumulator /
 *                            GenreClassifier publish their cross-thread outputs
 *                            via atomic floats or seqlocks. Do NOT add a second
 *                            analyzeBlock caller on another thread — that would
 *                            create a two-writer race on the meters' raw state.
 *   - prepare/reset        — message thread only, and the JUCE contract
 *                            guarantees the host is NOT calling processBlock /
 *                            analyzeBlock during them. That mutual exclusion is
 *                            what makes the non-atomic analysis accumulators
 *                            below (analysisSumSquares_, analysisSampleCount_,
 *                            analysisElapsedSeconds_, analysisSamplesSinceWindow
 *                            Sample_) and sampleRate_/blockSize_ safe: they have
 *                            a single writer during playback (the audio thread)
 *                            and are only mutated otherwise by prepare/reset,
 *                            which run with playback stopped. Atomizing them
 *                            would be cargo-cult — the host already serializes
 *                            the access.
 *   - Intelligence timers  — message thread only.
 *   - applyValidatedPlan() — message thread only. The SonicMaster analysis
 *                            thread hops to the message thread via callAsync
 *                            (SonicMasterAnalysisEngine::applyRamped) before
 *                            calling it, so DSP setter semantics hold.
 */
#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "MSMatrix.h"
#include "MultibandSplitter.h"
#include "MultibandDynamicsProcessor.h"
#include "AdaptiveEQ.h"
#include "StereoImager.h"
#include "HarmonicExciter.h"
#include "BrickwallLimiter.h"
#include "TruePeakEstimator.h"
#include "LUFSMeter.h"
#include "LoudnessNormalizer.h"
#include "MonoCompatibilityChecker.h"
#include "MeterWindowAccumulator.h"
#include "RealtimeSpectrumAnalyzer.h"
#include "StereoFieldAnalyzer.h"
#include "NeuralMasteringTypes.h"
#include "../AI/EQParameterTranslator.h"
#include "../AI/ChainPlanExecutor.h"
#include "../AI/GenreClassifier.h"

namespace more_phi {

class AutoMasteringEngine : public juce::Timer
{
public:
    AutoMasteringEngine();
    ~AutoMasteringEngine() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void prepare(double sampleRate, int maxBlockSize, bool startIntelligence = true);
    void reset() noexcept;

    // ── Activation (any thread — atomic) ─────────────────────────────────────

    void setActive(bool active) noexcept { active_.store(active, std::memory_order_relaxed); }
    [[nodiscard]] bool isActive() const noexcept { return active_.load(std::memory_order_relaxed); }

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Process audio through all 10 mastering stages.
     * If !isActive() returns immediately — zero processing cost.
     * noexcept — all sub-modules are noexcept.
     */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

    /**
     * Live analysis tap: updates LUFS, true peak, spectrum, stereo-field meters
     * and the genre classifier from the supplied buffer WITHOUT applying
     * mastering processing or changing the audio.
     *
     * THREADSWEEP-2026-06: AUDIO THREAD ONLY. Despite "analyze" in the name this
     * is called throttled from the host's processBlock (PluginProcessor:1520,
     * 2102), not from a message-thread timer. Its cross-thread meter reads below
     * are safe only via the meters' atomic/seqlock publications. A second caller
     * on any other thread would race the meters' raw (pre-publication) state.
     */
    void analyzeBlock(const juce::AudioBuffer<float>& buf) noexcept;

    // ── Metering (any thread) ─────────────────────────────────────────────────

    [[nodiscard]] float getLUFSMomentary()  const noexcept { return lufs_.getMomentary(); }
    [[nodiscard]] float getLUFSShortTerm()  const noexcept { return lufs_.getShortTerm(); }
    [[nodiscard]] float getLUFSIntegrated() const noexcept { return lufs_.getIntegrated(); }
    [[nodiscard]] float getLRA()            const noexcept { return lufs_.getLRA(); }
    [[nodiscard]] float getTruePeak_dBTP()  const noexcept { return analysisTruePeak_.getTruePeak_dBTP(); }
    [[nodiscard]] float getLimiterGainReductionDB() const noexcept { return limiter_.getGainReductionDB(); }
    [[nodiscard]] float getGainReductionDB(int band) const noexcept { return dynamics_.getGainReductionDB(band); }

    /** ENHANCERS-1/PDC: total lookahead latency of the mastering chain in
     *  host-rate samples (brickwall-limiter lookahead + exciter oversampling
     *  when enabled). Returns 0 when the chain is inactive (dormant), so the
     *  shipped plugin's reported latency is unchanged until mastering is engaged. */
    [[nodiscard]] int getMasteringChainLatency() const noexcept;
    [[nodiscard]] MeterWindowAccumulator::WindowStatistics computeMeterWindow(float windowSeconds) const noexcept
    {
        return meterWindow_.computeWindow(windowSeconds);
    }
    [[nodiscard]] bool isGenreClassifierModelLoaded() const noexcept { return genreClassifier_.isModelLoaded(); }

    // ── Chain access (message thread — for ABCompareEngine etc.) ─────────────

    LUFSMeter&                    getLUFSMeter()        noexcept { return lufs_; }
    AdaptiveEQ&                   getAdaptiveEQ()       noexcept { return eq_; }
    StereoImager&                 getStereoImager()     noexcept { return stereo_; }
    LoudnessNormalizer&           getLoudnessNormalizer() noexcept { return normalizer_; }
    MultibandDynamicsProcessor&   getDynamics()         noexcept { return dynamics_; }
    GenreClassifier&              getGenreClassifier()  noexcept { return genreClassifier_; }
    ChainPlanExecutor&            getChainPlanner()     noexcept { return chainPlanner_; }
    RealtimeSpectrumAnalyzer&     getSpectrumAnalyzer() noexcept { return spectrumAnalyzer_; }
    StereoFieldAnalyzer&          getStereoFieldAnalyzer() noexcept { return stereoFieldAnalyzer_; }
    const RealtimeSpectrumAnalyzer& getSpectrumAnalyzer() const noexcept { return spectrumAnalyzer_; }
    const StereoFieldAnalyzer&    getStereoFieldAnalyzer() const noexcept { return stereoFieldAnalyzer_; }

    // ── Loudness target (any thread) ──────────────────────────────────────────

    void setTargetLUFS(float lufs) noexcept { normalizer_.setTargetLUFS(lufs); }

    // ── Neural mastering validated-plan handoff (non-audio thread) ───────────

    bool applyValidatedPlan(const ValidatedNeuralMasteringPlan& plan) noexcept;
    void clearLastSafeNeuralMasteringPlan() noexcept;
    [[nodiscard]] bool hasLastSafeNeuralMasteringPlan() const noexcept { return hasLastSafeNeuralPlan_; }
    [[nodiscard]] const ValidatedNeuralMasteringPlan& getLastSafeNeuralMasteringPlan() const noexcept { return lastSafeNeuralPlan_; }

    // AUDIT CRITICAL-6/7/17: the neural plan is bridged to the hosted plugin via
    // the OzonePlanApplicator path (chainPlanner_.applyPlan forwards the plan to
    // whatever mastering plugin is loaded). Returns the parameter-enqueue count
    // from the last apply (>0 means the hosted plugin actually received writes),
    // or -1 when no applicator is registered / no apply has run. Read from any
    // thread (atomic).
    [[nodiscard]] int getLastOzoneAppliedCount() const noexcept { return lastOzoneAppliedCount_.load(std::memory_order_acquire); }

    // True when the last neural apply reached an audible path: either the
    // internal chain is active, or the Ozone applicator wrote at least one
    // parameter. SonicMasterAnalysisEngine uses this to distinguish Applied from
    // AppliedNoAudioPath.
    [[nodiscard]] bool lastApplyReachedAudioPath() const noexcept
    {
        return isActive() || getLastOzoneAppliedCount() > 0;
    }

    // ── State persistence (MED-11: survive save/restore) ────────────────────
    // Serializes the last applied neural plan (if any) as a MASTERING_PLAN child
    // of `parent`. No-op when no plan is held. Message thread only.
    void serializeLastPlan(juce::XmlElement& parent) const;
    // Restores a previously serialized plan. Returns true on success. Safe to
    // call when no MASTERING_PLAN element exists (no-op, returns false).
    bool restoreLastPlan(const juce::XmlElement& parent);

private:
    void timerCallback() override;
    void applyPlan(const MultiEffectPlan& plan);
    // AUDIT CRITICAL-7: convert a neural plan into the MultiEffectPlan the
    // OzonePlanApplicator consumes. Kept as a member so it can read the engine's
    // current stereo/limiter state to fill fields the neural plan leaves at
    // defaults. Message thread only.
    MultiEffectPlan buildBridgePlanFromNeural(const ValidatedNeuralMasteringPlan& plan) const noexcept;
    void updateAnalysisWindow(const juce::AudioBuffer<float>& buf) noexcept;
    void sumBands(juce::AudioBuffer<float> bands[MultibandSplitter::kNumBands],
                  juce::AudioBuffer<float>& out) noexcept;

    // ── Processing chain ──────────────────────────────────────────────────────
    MSMatrix                    ms_;
    MultibandSplitter           splitter_;
    MultibandDynamicsProcessor  dynamics_;
    AdaptiveEQ                  eq_;
    StereoImager                stereo_;
    HarmonicExciter             exciter_;
    BrickwallLimiter            limiter_;
    TruePeakEstimator           analysisTruePeak_;
    LUFSMeter                   lufs_;
    LoudnessNormalizer          normalizer_;
    MonoCompatibilityChecker    monoChecker_;
    MeterWindowAccumulator      meterWindow_;
    RealtimeSpectrumAnalyzer    spectrumAnalyzer_;
    StereoFieldAnalyzer         stereoFieldAnalyzer_;

    // ── Intelligence layer ────────────────────────────────────────────────────
    EQParameterTranslator       eqTranslator_;
    GenreClassifier             genreClassifier_;
    ChainPlanExecutor           chainPlanner_;

    // ── Pre-allocated band buffers ────────────────────────────────────────────
    juce::AudioBuffer<float> bandBuffers_[MultibandSplitter::kNumBands];

    std::atomic<bool> active_ { false };
    // THREADSWEEP-2026-06: sampleRate_/blockSize_ are written by prepare()
    // (message thread) and read by updateAnalysisWindow()/analyzeBlock() (audio
    // thread). Safe only under the JUCE contract that prepare runs with playback
    // stopped — NOT atomics. If you ever reconfigure the chain while audio is
    // flowing, gate that path on the audio thread first (or make these atomic).
    double sampleRate_  = 48000.0;
    int    blockSize_   = 512;
    // Same lifecycle invariant: single audio-thread writer during playback,
    // mutated by reset()/prepare() only when the host has stopped playback.
    double analysisElapsedSeconds_ = 0.0;
    int    analysisSamplesSinceWindowSample_ = 0;
    double analysisSumSquares_ = 0.0;
    int    analysisSampleCount_ = 0;

    // Timer tick counters (message thread only)
    int tickCount_            = 0;
    int plannerUpdateInterval_= 300;  // ~30s at 10Hz timer
    float smoothedSpectralTilt_ = 0.0f;
    ValidatedNeuralMasteringPlan lastSafeNeuralPlan_ {};
    bool hasLastSafeNeuralPlan_ = false;
    // AUDIT CRITICAL-7: count of hosted-plugin parameters written by the last
    // neural→Ozone bridge apply. -1 = no applicator / not yet applied.
    std::atomic<int> lastOzoneAppliedCount_ { -1 };
};

} // namespace more_phi
