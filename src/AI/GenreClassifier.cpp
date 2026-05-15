/*
 * More-Phi — AI/GenreClassifier.cpp
 */
#include "GenreClassifier.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

const char* const GenreClassifier::kGenreNames[kNumGenres] = {
    "electronic_dance",
    "house_techno",
    "hip_hop_rnb",
    "pop",
    "rock",
    "folk_acoustic",
    "jazz",
    "classical",
    "ambient",
    "metal",
    "streaming_default",
    "broadcast"
};

GenreClassifier::GenreClassifier()
{
    probsA_.fill(0.f);
    probsB_.fill(0.f);
    probsA_[10] = 1.f;  // default: streaming
    probsB_[10] = 1.f;
}

bool GenreClassifier::loadModel(const juce::File& /*modelFile*/)
{
    // ONNX Runtime integration stub.
    juce::Logger::writeToLog("GenreClassifier: ONNX model loading not yet implemented. Using default genre.");
    return false;
}

void GenreClassifier::unloadModel()
{
    modelLoaded_.store(false, std::memory_order_relaxed);
    topGenre_.store(10, std::memory_order_relaxed);
    topConf_.store(1.f, std::memory_order_relaxed);
}

const char* GenreClassifier::getTopGenreName() const noexcept
{
    const int g = topGenre_.load(std::memory_order_relaxed);
    if (g >= 0 && g < kNumGenres) return kGenreNames[g];
    return kGenreNames[10];
}

bool GenreClassifier::getGenreProbs(float* out) const noexcept
{
    if (out == nullptr) return false;
    const int front = frontBuffer_.load(std::memory_order_acquire);
    const auto& src = (front == 0) ? probsA_ : probsB_;
    std::copy(src.begin(), src.end(), out);
    return true;
}

void GenreClassifier::feedAudio(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    sampleRate_ = sampleRate;
    const int target = static_cast<int>(sampleRate * 10.0);
    if (accumulatedSamples_ >= target) return;

    const int toAdd = std::min(audio.getNumSamples(), target - accumulatedSamples_);
    audioAccum_.setSize(1, target, true, false, true);
    for (int i = 0; i < toAdd; ++i)
    {
        float mono = audio.getReadPointer(0)[i];
        if (audio.getNumChannels() > 1)
            mono = (mono + audio.getReadPointer(1)[i]) * 0.5f;
        audioAccum_.getWritePointer(0)[accumulatedSamples_ + i] = mono;
    }
    accumulatedSamples_ += toAdd;
    if (accumulatedSamples_ >= target)
        hasNewAudio_.store(true, std::memory_order_release);
}

void GenreClassifier::runClassification()
{
    // ONNX inference stub — model not loaded; keep default genre.
    if (!modelLoaded_.load(std::memory_order_relaxed)) return;

    // When model is loaded:
    // 1. Compute mel spectrogram of audioAccum_ (128 bins, 10 s, hop 512)
    // 2. Run ONNX session: input [1, 128, frames] → output [1, 12]
    // 3. Apply softmax
    // 4. Write to back buffer, swap, update topGenre_ / topConf_
}

void GenreClassifier::timerCallback()
{
    ++classificationTimer_;
    if (classificationTimer_ < kAnalysisIntervalSeconds) return;
    classificationTimer_ = 0;

    if (!hasNewAudio_.load(std::memory_order_acquire)) return;
    hasNewAudio_.store(false, std::memory_order_relaxed);
    accumulatedSamples_ = 0;

    runClassification();
}

} // namespace more_phi
