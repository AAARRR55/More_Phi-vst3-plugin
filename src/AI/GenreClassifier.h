/*
 * More-Phi — AI/GenreClassifier.h
 *
 * Lightweight CNN-based genre classifier (12 genres).
 *
 * Input:  128-bin mel spectrogram, 10 seconds of audio, hop 512
 * Output: Softmax probability over 12 genre classes
 *
 * When no ONNX model is loaded, defaults to "Streaming (general)" (index 10).
 * Inference runs on message thread via juce::Timer every 30 seconds.
 *
 * Genre interpolation: when confidence is split between two genres,
 * MorphProcessor linearly blends the two corresponding mastering profiles.
 *
 * Thread safety:
 *   getTopGenre() / getGenreProbs() — any thread (atomic reads).
 *   timerCallback() — message thread only.
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

class GenreClassifier : public juce::Timer
{
public:
    static constexpr int kNumGenres = 12;

    // Genre index → name mapping (matches mastering_profiles.json)
    static const char* const kGenreNames[kNumGenres];

    GenreClassifier();
    ~GenreClassifier() override { stopTimer(); }

    // ── Model management (message thread) ─────────────────────────────────────

    bool loadModel(const juce::File& modelFile);
    void unloadModel();
    [[nodiscard]] bool isModelLoaded() const noexcept { return modelLoaded_.load(std::memory_order_relaxed); }

    // ── Genre output (any thread) ─────────────────────────────────────────────

    /** Index of the top genre (0–11). Returns 10 (Streaming) if no model. */
    [[nodiscard]] int getTopGenre() const noexcept { return topGenre_.load(std::memory_order_relaxed); }

    /** Confidence of the top genre [0, 1]. */
    [[nodiscard]] float getTopConfidence() const noexcept { return topConf_.load(std::memory_order_relaxed); }

    /** Returns name of the top genre. */
    [[nodiscard]] const char* getTopGenreName() const noexcept;

    /**
     * Copy all 12 genre probabilities to out[12].
     * Returns false if no valid classification available.
     */
    bool getGenreProbs(float* out) const noexcept;

    // ── Feed audio (message thread) ───────────────────────────────────────────

    void feedAudio(const juce::AudioBuffer<float>& audio, double sampleRate);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void start() { startTimerHz(1); }  // run at 1 Hz, but only classifies every 30 s
    void stop()  { stopTimer(); }

private:
    void timerCallback() override;
    void runClassification();

    std::atomic<int>   topGenre_    { 10 };   // default: Streaming
    std::atomic<float> topConf_     { 1.f };

    // Double-buffered probabilities
    std::array<float, kNumGenres> probsA_{};
    std::array<float, kNumGenres> probsB_{};
    std::atomic<int>  frontBuffer_  { 0 };

    juce::AudioBuffer<float> audioAccum_;
    int accumulatedSamples_  = 0;
    int classificationTimer_ = 0;
    double sampleRate_       = 48000.0;

    std::atomic<bool> modelLoaded_ { false };
    std::atomic<bool> hasNewAudio_ { false };

    static constexpr int kAnalysisIntervalSeconds = 30;
};

} // namespace more_phi
