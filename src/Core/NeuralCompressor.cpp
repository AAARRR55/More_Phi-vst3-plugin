/*
 * More-Phi — Core/NeuralCompressor.cpp
 */
#include "NeuralCompressor.h"
#include <juce_core/juce_core.h>

namespace more_phi {

// Heuristic defaults derived from published mastering literature
const MultibandDynamicsProcessor::BandParams NeuralCompressor::kHeuristicDefaults[kNumBands] = {
    // Band 0: Sub (< 80 Hz)
    { .thresholdDB = -18.f, .ratio = 1.5f, .attackMs =  50.f, .releaseMs = 200.f,
      .makeupDB = 0.f, .kneeDB = 4.f },
    // Band 1: Low (80–250 Hz)
    { .thresholdDB = -20.f, .ratio = 2.5f, .attackMs =  15.f, .releaseMs = 150.f,
      .makeupDB = 0.f, .kneeDB = 3.f },
    // Band 2: Mid (250 Hz–5 kHz)
    { .thresholdDB = -22.f, .ratio = 3.0f, .attackMs =   8.f, .releaseMs = 120.f,
      .makeupDB = 0.f, .kneeDB = 2.f },
    // Band 3: High (> 5 kHz)
    { .thresholdDB = -18.f, .ratio = 2.0f, .attackMs =   3.f, .releaseMs =  80.f,
      .makeupDB = 0.f, .kneeDB = 2.f },
};

NeuralCompressor::NeuralCompressor()
{
    for (auto& rms : bandRMS_)
        rms.store(0.f, std::memory_order_relaxed);
}

NeuralCompressor::~NeuralCompressor()
{
    stopTimer();
}

bool NeuralCompressor::loadModel(const juce::File& /*modelFile*/)
{
    // ONNX Runtime integration stub:
    // When onnxruntime is added to CMakeLists.txt, implement:
    //   onnxSession_ = std::make_unique<Ort::Session>(env_, modelPath, opts_);
    // For now, heuristic fallback is always used.
    juce::Logger::writeToLog("NeuralCompressor: ONNX model loading not yet implemented. Using heuristics.");
    return false;
}

void NeuralCompressor::unloadModel()
{
    modelLoaded_.store(false, std::memory_order_relaxed);
    applyHeuristicDefaults();
}

void NeuralCompressor::prepare(MultibandDynamicsProcessor& dynamics, double sampleRate)
{
    dynamics_   = &dynamics;
    sampleRate_ = sampleRate;
    applyHeuristicDefaults();
}

void NeuralCompressor::applyHeuristicDefaults() noexcept
{
    if (dynamics_ == nullptr) return;
    for (int b = 0; b < kNumBands; ++b)
        dynamics_->setBandParams(b, kHeuristicDefaults[b]);
}

void NeuralCompressor::timerCallback()
{
    if (dynamics_ == nullptr) return;

    if (!modelLoaded_.load(std::memory_order_relaxed))
    {
        // No model — heuristic defaults already applied in prepare()
        // Optionally do dynamic threshold adaptation based on RMS here
        return;
    }

    // ONNX inference stub (activated when model is loaded):
    // 1. Collect per-band RMS from bandRMS_ atomics
    // 2. Build input tensor [1, numBands]
    // 3. Run inference → output [1, numBands * 5] (threshold, ratio, attack, release, makeup per band)
    // 4. Clamp outputs to safe ranges
    // 5. Call pushParamsToDynamics(params)
}

void NeuralCompressor::pushParamsToDynamics(
    const std::array<MultibandDynamicsProcessor::BandParams, kNumBands>& params) noexcept
{
    if (dynamics_ == nullptr) return;
    for (int b = 0; b < kNumBands; ++b)
        dynamics_->setBandParams(b, params[b]);
}

} // namespace more_phi
