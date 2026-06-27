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

#include "AI/MelSpectrogram.h"

#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <string>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

namespace more_phi {

// ONNX session is pimpl'd so the header stays ORT-free for the many TUs that
// include it (AutoMasteringEngine, PluginProcessor, tests). Defined in the .cpp.
struct GenreSessionHandle;

class GenreClassifier : public juce::Timer
{
public:
    static constexpr int kNumGenres = 12;

    // Phase B helper: remap a model's per-class softmax output onto the plugin's
    // kNumGenres slots, given a modelOutputIdx→pluginSlot table (-1 = unmapped).
    // Writes the kNumGenres-wide probability vector to outProbs (must have room
    // for kNumGenres floats). Returns the remapped argmax slot, or -1 if the
    // top model class is unmapped (caller should fall back to the heuristic).
    // Pure/noexcept — unit-testable without any ONNX or model file.
    static int remapGenreProbs(const float* modelProbs, int numModelClasses,
                               const int* genreRemap, int genreRemapCount,
                               float* outProbs) noexcept;

    // Genre index → name mapping (matches mastering_profiles.json)
    static const char* const kGenreNames[kNumGenres];

    GenreClassifier();
    // Out-of-line destructor: session_ is a unique_ptr<GenreSessionHandle> and
    // GenreSessionHandle is an incomplete pimpl in this header (defined in the
    // .cpp). An inline destructor would instantiate ~unique_ptr here with an
    // incomplete type → "can't delete an incomplete type" in every including TU.
    ~GenreClassifier() override;

    // Non-copyable, non-movable: the pimpl'd ONNX session + the juce::Timer base
    // make moves unsafe, and deleting these (matching SonicMasterDecisionRunner's
    // idiom) also forces MSVC to emit copy/move destruction only in the .cpp.
    GenreClassifier(const GenreClassifier&) = delete;
    GenreClassifier& operator=(const GenreClassifier&) = delete;
    GenreClassifier(GenreClassifier&&) = delete;
    GenreClassifier& operator=(GenreClassifier&&) = delete;

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

    // Test hook: run one classification immediately on the calling thread,
    // bypassing the 1 Hz / 30 s timer gate. Used by unit tests to exercise the
    // heuristic (and, when a model is loaded, the ONNX path) deterministically.
    // Message thread only.
    void runClassificationForTest() noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void start() { startTimerHz(1); }  // run at 1 Hz, but only classifies every 30 s
    void stop()  { stopTimer(); }

private:
    void timerCallback() override;
    void runClassification();
    // Phase B: ONNX path. Returns true if it published a genre guess; false on
    // any failure (no audio, inference threw, shape drift, unmapped argmax) so
    // runClassification can fall through to the heuristic. noexcept.
    bool runNeuralClassification_() noexcept;
    // Time-domain heuristic: low/high band split + zero-crossing rate → coarse
    // genre guess. The documented fallback for slots a model can't classify.
    void runHeuristicClassification_();

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

    // ── Neural genre path (Phase B, 2026-06-27) ───────────────────────────────
    // When a model is loaded, runClassification runs the mel frontend + ONNX
    // inference and remaps the model's output classes onto the 12 genre slots;
    // any failure falls through to the time-domain heuristic below. See .cpp.
    MelSpectrogram mel_;
    std::vector<float> probsScratch_;   // model output softmax (numModelClasses)
    std::vector<int>  genreRemap_;      // modelOutputIdx → plugin slot idx (-1 = unmapped)
    int numModelClasses_ = 0;           // model output width (read at load)
    std::unique_ptr<GenreSessionHandle> session_;

    static constexpr int kAnalysisIntervalSeconds = 30;
};

} // namespace more_phi
