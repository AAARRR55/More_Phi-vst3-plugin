/*
 * More-Phi — AI/StyleEncoder.h
 *
 * AFx-Rep style encoder: wraps a 256-dim audio style embedding ONNX model.
 *
 * Input:  128-bin mel spectrogram of 2 seconds of audio (pre-computed on message thread)
 * Output: 256-dim L2-normalized style embedding vector
 *
 * When no model is loaded, returns an all-zeros embedding and sets valid_ = false.
 * Inference runs on the message thread (via juce::Timer) — never on audio thread.
 *
 * The embedding is double-buffered (front/back pattern) so the audio thread
 * can always read a complete, stable embedding without blocking.
 *
 * Thread safety:
 *   timerCallback() / computeEmbedding() — message thread only.
 *   getEmbedding() — any thread (atomic index + read-only array access).
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <string>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

namespace more_phi {

class StyleEncoder : public juce::Timer
{
public:
    static constexpr int kEmbeddingDim = 256;
    static constexpr int kMelBins      = 128;
    static constexpr int kMelFrames    = 200;   // ~2 s at hop=512, sr=51200 equivalent

    StyleEncoder();
    ~StyleEncoder() override { stopTimer(); }

    // ── Model management (message thread) ─────────────────────────────────────

    /** Load AFx-Rep ONNX model. Returns true on success. */
    bool loadModel(const juce::File& modelFile);

    /** Unload model. getEmbedding() returns zeros. */
    void unloadModel();

    [[nodiscard]] bool isModelLoaded() const noexcept { return modelLoaded_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isValid()       const noexcept { return valid_.load(std::memory_order_relaxed); }

    // ── Embedding access (any thread) ─────────────────────────────────────────

    /**
     * Copy current embedding into out (must be at least kEmbeddingDim floats).
     * Returns false if no valid embedding is available.
     */
    bool getEmbedding(float* out, int maxDim) const noexcept;

    /**
     * Compute cosine similarity between current embedding and a reference.
     * ref must be kEmbeddingDim floats, L2-normalized.
     */
    float cosineSimilarity(const float* ref, int dim) const noexcept;

    // ── Feed audio for next inference ─────────────────────────────────────────

    /** Called from message thread with the latest audio to analyze (stereo or mono). */
    void feedAudio(const juce::AudioBuffer<float>& audio, double sampleRate);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Start periodic embedding updates at ~1 Hz. */
    void start() { startTimerHz(1); }
    void stop()  { stopTimer(); }

private:
    void timerCallback() override;
    void computeMelSpectrogram(const juce::AudioBuffer<float>& audio, double sr);
    void runInference();

    // Double-buffered embedding
    std::array<float, kEmbeddingDim> embeddingA_{};
    std::array<float, kEmbeddingDim> embeddingB_{};
    std::atomic<int>  frontBuffer_  { 0 };   // 0 = A is front, 1 = B is front

    // Mel spectrogram buffer (message thread only)
    std::vector<float> melBuffer_;   // kMelBins * kMelFrames

    // Audio accumulation buffer (message thread only)
    juce::AudioBuffer<float> audioAccum_;
    int accumulatedSamples_ = 0;
    double sampleRate_      = 48000.0;

    std::atomic<bool> modelLoaded_ { false };
    std::atomic<bool> valid_       { false };
    std::atomic<bool> hasNewAudio_ { false };
};

} // namespace more_phi
