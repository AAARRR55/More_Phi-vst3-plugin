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
 * Intelligence (all on message thread):
 *   - EQParameterTranslator applies genre warm-start to EQ bands 0–7
 *   - ChainPlanExecutor runs a 5-step heuristic rule planner every 30s
 *   - GenreClassifier classifies genre every 30s
 *   - LoudnessNormalizer.updateCorrectionGain() called every 100ms
 *   - MonoCompatibilityChecker checks fold-down every 1s
 *
 * Thread safety:
 *   processBlock() — audio thread, noexcept.
 *   prepare() — message thread only.
 *   All intelligence timers — message thread only.
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
     * Non-mutating live analysis tap. Updates LUFS, true peak, spectrum, and
     * stereo-field meters from the supplied buffer without applying mastering
     * processing or changing the audio.
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

private:
    void timerCallback() override;
    void applyPlan(const MultiEffectPlan& plan);
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
    double sampleRate_  = 48000.0;
    int    blockSize_   = 512;
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
};

} // namespace more_phi
