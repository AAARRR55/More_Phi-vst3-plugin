/*
 * More-Phi — Core/NeuralCompressor.h
 *
 * Per-band compressor that wraps a micro-TCN ONNX model for
 * intelligent gain reduction parameter prediction.
 *
 * When the ONNX model is not loaded (default), a set of carefully
 * tuned heuristic defaults is used (see kHeuristicDefaults).
 * This ensures the module is fully functional at all times.
 *
 * ONNX inference runs on the message thread (via a juce::Timer at 30ms).
 * Results are written to std::atomic arrays that the audio thread reads.
 * Zero heap allocation after prepare().
 *
 * Thread safety:
 *   processBlock() — audio thread, noexcept (reads atomics, no inference).
 *   timerCallback() — message thread only (runs ONNX inference).
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <string>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "MultibandDynamicsProcessor.h"

namespace more_phi {

class NeuralCompressor : public juce::Timer
{
public:
    static constexpr int kNumBands = MultibandDynamicsProcessor::kNumBands;

    NeuralCompressor();
    ~NeuralCompressor() override;

    // ── Model management (message thread) ─────────────────────────────────────

    /** Load micro-TCN ONNX model from file. Returns true on success. */
    bool loadModel(const juce::File& modelFile);

    /** Unload model and revert to heuristic defaults. */
    void unloadModel();

    [[nodiscard]] bool isModelLoaded() const noexcept { return modelLoaded_.load(std::memory_order_relaxed); }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Link to the MultibandDynamicsProcessor that will receive parameter updates.
     * Must be called before start().
     */
    void prepare(MultibandDynamicsProcessor& dynamics, double sampleRate);

    /** Start the 30ms inference timer. */
    void start() { startTimerHz(33); }

    /** Stop the inference timer. */
    void stop()  { stopTimer(); }

    // ── Heuristic defaults ────────────────────────────────────────────────────

    /** Push heuristic defaults immediately to the linked dynamics processor. */
    void applyHeuristicDefaults() noexcept;

    // ── Audio thread: provide signal analysis input ───────────────────────────

    /** Feed per-band RMS (linear) for the next inference cycle.
     *  Called from audio thread — written to atomics. */
    void updateBandRMS(int band, float rmsLinear) noexcept
    {
        if (band >= 0 && band < kNumBands)
            bandRMS_[band].store(rmsLinear, std::memory_order_relaxed);
    }

private:
    void timerCallback() override;

    void pushParamsToDynamics(const std::array<MultibandDynamicsProcessor::BandParams, kNumBands>& p) noexcept;

    MultibandDynamicsProcessor* dynamics_ = nullptr;
    std::atomic<bool> modelLoaded_ { false };
    double sampleRate_ = 48000.0;

    // Audio thread writes, message thread reads for inference input
    std::array<std::atomic<float>, kNumBands> bandRMS_ {};

    static const MultibandDynamicsProcessor::BandParams kHeuristicDefaults[kNumBands];
};

} // namespace more_phi
