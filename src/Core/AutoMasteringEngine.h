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
 *   - NeuralCompressor updates dynamics params every 30ms
 *   - EQParameterTranslator applies genre warm-start to EQ bands 0–7
 *   - ChainPlanExecutor runs 5-step CoT reasoning every 30s
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
#include "LUFSMeter.h"
#include "LoudnessNormalizer.h"
#include "MonoCompatibilityChecker.h"
#include "NeuralCompressor.h"
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

    void prepare(double sampleRate, int maxBlockSize);
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

    // ── Metering (any thread) ─────────────────────────────────────────────────

    [[nodiscard]] float getLUFSMomentary()  const noexcept { return lufs_.getMomentary(); }
    [[nodiscard]] float getLUFSShortTerm()  const noexcept { return lufs_.getShortTerm(); }
    [[nodiscard]] float getLUFSIntegrated() const noexcept { return lufs_.getIntegrated(); }
    [[nodiscard]] float getLRA()            const noexcept { return lufs_.getLRA(); }
    [[nodiscard]] float getTruePeak_dBTP()  const noexcept { return limiter_.getGainReductionDB(); }
    [[nodiscard]] float getGainReductionDB(int band) const noexcept { return dynamics_.getGainReductionDB(band); }

    // ── Chain access (message thread — for ABCompareEngine etc.) ─────────────

    LUFSMeter&                    getLUFSMeter()        noexcept { return lufs_; }
    AdaptiveEQ&                   getAdaptiveEQ()       noexcept { return eq_; }
    StereoImager&                 getStereoImager()     noexcept { return stereo_; }
    LoudnessNormalizer&           getLoudnessNormalizer() noexcept { return normalizer_; }
    MultibandDynamicsProcessor&   getDynamics()         noexcept { return dynamics_; }
    GenreClassifier&              getGenreClassifier()  noexcept { return genreClassifier_; }
    ChainPlanExecutor&            getChainPlanner()     noexcept { return chainPlanner_; }

    // ── Loudness target (any thread) ──────────────────────────────────────────

    void setTargetLUFS(float lufs) noexcept { normalizer_.setTargetLUFS(lufs); }

private:
    void timerCallback() override;
    void applyPlan(const MultiEffectPlan& plan);
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
    LUFSMeter                   lufs_;
    LoudnessNormalizer          normalizer_;
    MonoCompatibilityChecker    monoChecker_;

    // ── Intelligence layer ────────────────────────────────────────────────────
    NeuralCompressor            neuralComp_;
    EQParameterTranslator       eqTranslator_;
    GenreClassifier             genreClassifier_;
    ChainPlanExecutor           chainPlanner_;

    // ── Pre-allocated band buffers ────────────────────────────────────────────
    juce::AudioBuffer<float> bandBuffers_[MultibandSplitter::kNumBands];

    std::atomic<bool> active_ { false };
    double sampleRate_  = 48000.0;
    int    blockSize_   = 512;

    // Timer tick counters (message thread only)
    int tickCount_            = 0;
    int plannerUpdateInterval_= 300;  // ~30s at 10Hz timer
};

} // namespace more_phi
