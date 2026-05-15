/*
 * More-Phi — AI/StyleEncoder.cpp
 */
#include "StyleEncoder.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

StyleEncoder::StyleEncoder()
{
    embeddingA_.fill(0.f);
    embeddingB_.fill(0.f);
    melBuffer_.assign(kMelBins * kMelFrames, 0.f);
}

bool StyleEncoder::loadModel(const juce::File& /*modelFile*/)
{
    // ONNX Runtime integration stub.
    // When onnxruntime is added to CMakeLists.txt, implement:
    //   session_ = std::make_unique<Ort::Session>(env_, path, opts_);
    juce::Logger::writeToLog("StyleEncoder: ONNX model loading not yet implemented.");
    return false;
}

void StyleEncoder::unloadModel()
{
    modelLoaded_.store(false, std::memory_order_relaxed);
    valid_.store(false, std::memory_order_relaxed);
    embeddingA_.fill(0.f);
    embeddingB_.fill(0.f);
}

bool StyleEncoder::getEmbedding(float* out, int maxDim) const noexcept
{
    if (!valid_.load(std::memory_order_relaxed) || out == nullptr) return false;
    const int front = frontBuffer_.load(std::memory_order_acquire);
    const auto& src = (front == 0) ? embeddingA_ : embeddingB_;
    const int dim = std::min(maxDim, kEmbeddingDim);
    std::copy_n(src.data(), dim, out);
    return true;
}

float StyleEncoder::cosineSimilarity(const float* ref, int dim) const noexcept
{
    if (!valid_.load(std::memory_order_relaxed) || ref == nullptr) return 0.f;
    const int front = frontBuffer_.load(std::memory_order_acquire);
    const auto& emb = (front == 0) ? embeddingA_ : embeddingB_;
    const int d = std::min(dim, kEmbeddingDim);

    float dot = 0.f, normEmb = 0.f, normRef = 0.f;
    for (int i = 0; i < d; ++i)
    {
        dot     += emb[i] * ref[i];
        normEmb += emb[i] * emb[i];
        normRef += ref[i] * ref[i];
    }
    const float denom = std::sqrt(normEmb * normRef);
    return (denom > 1e-12f) ? (dot / denom) : 0.f;
}

void StyleEncoder::feedAudio(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    sampleRate_ = sampleRate;
    // Accumulate up to 2 s of audio for mel computation
    const int targetSamples = static_cast<int>(sampleRate * 2.0);
    if (accumulatedSamples_ < targetSamples)
    {
        const int toAdd = std::min(audio.getNumSamples(), targetSamples - accumulatedSamples_);
        audioAccum_.setSize(1, targetSamples, true, false, true);
        for (int i = 0; i < toAdd; ++i)
        {
            float mono = audio.getReadPointer(0)[i];
            if (audio.getNumChannels() > 1)
                mono = (mono + audio.getReadPointer(1)[i]) * 0.5f;
            audioAccum_.getWritePointer(0)[accumulatedSamples_ + i] = mono;
        }
        accumulatedSamples_ += toAdd;
        if (accumulatedSamples_ >= targetSamples)
            hasNewAudio_.store(true, std::memory_order_release);
    }
}

void StyleEncoder::computeMelSpectrogram(const juce::AudioBuffer<float>& /*audio*/, double /*sr*/)
{
    // Mel spectrogram computation stub.
    // Full implementation: STFT (FFT size 1024, hop 512) → mel filterbank → log
    // For now, fill with zeros — ONNX model stub will not produce meaningful output.
    melBuffer_.assign(kMelBins * kMelFrames, 0.f);
}

void StyleEncoder::runInference()
{
    // ONNX inference stub.
    // When model is loaded: feed melBuffer_ as input tensor → get 256-dim output.
    // L2-normalize output, write to back buffer, swap frontBuffer_.
    if (!modelLoaded_.load(std::memory_order_relaxed)) return;

    // Write to back buffer and swap
    const int back = 1 - frontBuffer_.load(std::memory_order_relaxed);
    auto& dst = (back == 0) ? embeddingA_ : embeddingB_;
    dst.fill(0.f);  // placeholder
    frontBuffer_.store(back, std::memory_order_release);
    valid_.store(true, std::memory_order_release);
}

void StyleEncoder::timerCallback()
{
    if (!hasNewAudio_.load(std::memory_order_acquire)) return;
    hasNewAudio_.store(false, std::memory_order_relaxed);
    accumulatedSamples_ = 0;

    computeMelSpectrogram(audioAccum_, sampleRate_);
    runInference();
}

} // namespace more_phi
