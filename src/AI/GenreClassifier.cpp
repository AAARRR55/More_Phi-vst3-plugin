/*
 * More-Phi — AI/GenreClassifier.cpp
 */
#include "GenreClassifier.h"
#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

#ifndef MORE_PHI_HAS_ONNX
#define MORE_PHI_HAS_ONNX 0
#endif

#if MORE_PHI_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace more_phi {

#if MORE_PHI_HAS_ONNX
// Mirrors SonicMasterSessionHandle (SonicMasterDecisionRunner.cpp:30). One env
// per classifier (per the existing per-runner convention); the 1 Hz / 30 s
// cadence means there is no contention to share.
struct GenreSessionHandle
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
    std::string inputName;
    std::string outputName;
    Ort::MemoryInfo memoryInfo { Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault) };
    std::vector<float> inputBuffer;          // [1, 128, T], reused per inference
    std::vector<int64_t> inputShape;         // {1, 128, T} (or model-declared)
    std::vector<int64_t> outputShape;        // {1, numClasses}
};
#else
struct GenreSessionHandle {};
#endif

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

GenreClassifier::~GenreClassifier()
{
    stopTimer();
    // session_ (unique_ptr<GenreSessionHandle>) destroyed here, where the type
    // is complete — see the corresponding note in GenreClassifier.h.
}

bool GenreClassifier::loadModel(const juce::File& modelFile)
{
    unloadModel();
    if (!modelFile.existsAsFile()) return false;

#if !MORE_PHI_HAS_ONNX
    DBG("GenreClassifier: ONNX support not compiled in (MORE_PHI_ENABLE_ONNX=OFF). "
        "Heuristic fallback remains active.");
    return false;
#else
    try
    {
        session_ = std::make_unique<GenreSessionHandle>();
        session_->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "genre");
        session_->allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);                  // 1 Hz cadence; no pool gain
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        const auto pathStr = modelFile.getFullPathName().toStdString();
        session_->session = std::make_unique<Ort::Session>(
#ifdef _WIN32
            *session_->env, std::wstring(pathStr.begin(), pathStr.end()).c_str(), opts
#else
            *session_->env, pathStr.c_str(), opts
#endif
        );

        // ── Validate I/O contract: 1 in, 1 out, float tensors ───────────────
        if (session_->session->GetInputCount() != 1 || session_->session->GetOutputCount() != 1)
        { unloadModel(); return false; }

        auto inputInfo  = session_->session->GetInputTypeInfo(0);
        auto outputInfo = session_->session->GetOutputTypeInfo(0);
        auto inTensor   = inputInfo.GetTensorTypeAndShapeInfo();
        auto outTensor  = outputInfo.GetTensorTypeAndShapeInfo();
        if (inTensor.GetElementType()  != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        { unloadModel(); return false; }

        // ORT 1.22.1 quirk: GetDimensionsCount/GetDimensions, not GetShape()
        // (segfaults on symbolic batch/time — same as SonicMasterDecisionRunner).
        const auto inDimCount  = inTensor.GetDimensionsCount();
        const auto outDimCount = outTensor.GetDimensionsCount();
        std::vector<int64_t> inDims(inDimCount, -1);
        inTensor.GetDimensions(inDims.data(), inDimCount);
        std::vector<int64_t> outDims(outDimCount, -1);
        outTensor.GetDimensions(outDims.data(), outDimCount);

        // Input: [batch, mel, time], batch∈{1,-1}, mel==128 (or symbolic -1).
        if (inDimCount != 3
            || (inDims[0] != 1 && inDims[0] != -1)
            || (inDims[1] != MelSpectrogram::kMelBins && inDims[1] != -1))
        { unloadModel(); return false; }

        // Output: [batch, numClasses]. Read the class count.
        if (outDimCount != 2 || (outDims[0] != 1 && outDims[0] != -1) || outDims[1] <= 0)
        { unloadModel(); return false; }
        numModelClasses_ = static_cast<int>(outDims[1]);

        // Time dimension: if the model declares a concrete T, honor it; if
        // symbolic (-1), derive T from the 10 s window via the mel frontend.
        const std::size_t analysisSamples = static_cast<std::size_t>(sampleRate_ * 10.0);
        mel_.prepare(sampleRate_, analysisSamples);
        std::size_t melFrames = mel_.getFrameCount();
        int64_t modelT = inDims[2];
        if (modelT <= 0) modelT = static_cast<int64_t>(melFrames);  // symbolic → use frontend
        // If the model wants a different T than the frontend produces, prefer the
        // model's concrete T (re-prepare the frontend to that exact window).
        if (modelT != static_cast<int64_t>(melFrames))
        {
            // Reconstruct the sample count that yields modelT frames:
            // frames = 1 + (samples - fftSize)/hop  →  samples = (frames-1)*hop + fftSize
            const std::size_t samplesForT =
                static_cast<std::size_t>(modelT - 1) * MelSpectrogram::kHopSize + MelSpectrogram::kFftSize;
            mel_.prepare(sampleRate_, samplesForT);
            melFrames = mel_.getFrameCount();
            if (melFrames != static_cast<std::size_t>(modelT))
            { unloadModel(); return false; }   // couldn't reconcile — refuse
        }

        session_->inputShape  = { 1, static_cast<int64_t>(MelSpectrogram::kMelBins), modelT };
        session_->outputShape = { 1, static_cast<int64_t>(numModelClasses_) };
        session_->inputBuffer.assign(
            static_cast<std::size_t>(MelSpectrogram::kMelBins * modelT), 0.0f);
        probsScratch_.assign(static_cast<std::size_t>(numModelClasses_), 0.0f);

        // Label remap: identity-map the first min(numModelClasses_, 12) outputs
        // onto the plugin's 12 slots in order. A real model can override this
        // later by emitting genre_labels metadata; for now the heuristic fills
        // any unmapped slot. ponytail: identity default = zero remap config.
        genreRemap_.assign(static_cast<std::size_t>(numModelClasses_), -1);
        const int identityN = std::min(numModelClasses_, kNumGenres);
        for (int i = 0; i < identityN; ++i)
            genreRemap_[static_cast<std::size_t>(i)] = i;

        auto inNameAlloc  = session_->session->GetInputNameAllocated(0, *session_->allocator);
        auto outNameAlloc = session_->session->GetOutputNameAllocated(0, *session_->allocator);
        session_->inputName  = inNameAlloc.get();
        session_->outputName = outNameAlloc.get();

        modelLoaded_.store(true, std::memory_order_relaxed);
        DBG("GenreClassifier: ONNX model loaded (" + pathStr + "), " +
            juce::String(numModelClasses_) + " classes, " +
            juce::String(static_cast<int>(modelT)) + " mel frames.");
        return true;
    }
    catch (const Ort::Exception&)
    {
        unloadModel();
        return false;
    }
    catch (...)
    {
        unloadModel();
        return false;
    }
#endif
}

void GenreClassifier::unloadModel()
{
#if MORE_PHI_HAS_ONNX
    session_.reset();
#endif
    probsScratch_.clear();
    probsScratch_.shrink_to_fit();
    genreRemap_.clear();
    genreRemap_.shrink_to_fit();
    numModelClasses_ = 0;
    modelLoaded_.store(false, std::memory_order_relaxed);
    // NOTE: do NOT reset topGenre_/topConf_ here — the heuristic keeps producing
    // a live genre guess after unload, so the prior-wiring isn't left dangling
    // on the Streaming default. The ONNX path only ever overrides the heuristic.
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

void GenreClassifier::runClassificationForTest() noexcept
{
    runClassification();
}

int GenreClassifier::remapGenreProbs(const float* modelProbs, int numModelClasses,
                                     const int* genreRemap, int genreRemapCount,
                                     float* outProbs) noexcept
{
    // Find the argmax over the model's classes.
    int bestModel = -1;
    float bestVal = -1.0f;
    for (int i = 0; i < numModelClasses; ++i)
    {
        if (modelProbs[i] > bestVal) { bestVal = modelProbs[i]; bestModel = i; }
    }
    if (bestModel < 0) { std::fill_n(outProbs, kNumGenres, 0.0f); return -1; }

    // Remap the argmax onto a plugin slot. Unmapped (-1) → caller falls back.
    int pluginSlot = -1;
    if (bestModel < genreRemapCount)
        pluginSlot = genreRemap[bestModel];
    if (pluginSlot < 0 || pluginSlot >= kNumGenres)
    { std::fill_n(outProbs, kNumGenres, 0.0f); return -1; }

    // Confidence = softmax max, clamped to [0,1].
    const float conf = std::clamp(bestVal, 0.0f, 1.0f);
    // Publish: remapped slot gets conf; remaining slots share (1 - conf) / (N-1).
    std::fill_n(outProbs, kNumGenres, 0.0f);
    outProbs[pluginSlot] = conf;
    const float rest = (1.0f - conf) / static_cast<float>(kNumGenres - 1);
    for (int i = 0; i < kNumGenres; ++i)
        if (i != pluginSlot) outProbs[i] = rest;
    return pluginSlot;
}

void GenreClassifier::runClassification()
{    // Phase B (2026-06-27): if a model is loaded, run mel + ONNX inference and
    // remap onto the 12 slots. On ANY failure (no audio, inference threw, output
    // malformed) fall through to the time-domain heuristic so the prior-wiring
    // never goes silent. The heuristic below is the documented fallback for
    // slots the model cannot classify (streaming_default, broadcast, etc.).
#if MORE_PHI_HAS_ONNX
    if (modelLoaded_.load(std::memory_order_relaxed) && session_ != nullptr)
    {
        if (runNeuralClassification_())
            return;
        // Neural path failed → drop to heuristic (fail-soft, not fail-deaf).
    }
#endif
    runHeuristicClassification_();
}

#if MORE_PHI_HAS_ONNX
bool GenreClassifier::runNeuralClassification_() noexcept
{
    if (accumulatedSamples_ <= 0 || sampleRate_ <= 0.0 || session_ == nullptr)
        return false;

    try
    {
        // Mel frontend: the 10 s mono buffer → [1, 128, T] log-mel.
        mel_.process(audioAccum_.getReadPointer(0),
                     static_cast<std::size_t>(accumulatedSamples_));
        // The frontend writes into its own internal buffer; copy into the
        // session's input buffer (zero-alloc view is built from session_->inputBuffer).
        const float* melData = mel_.getOutput();
        const std::size_t melCount = mel_.getOutputCount();
        if (melCount != session_->inputBuffer.size())
            return false;   // shape drift — refuse, let heuristic run
        std::copy_n(melData, melCount, session_->inputBuffer.data());

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            session_->memoryInfo,
            session_->inputBuffer.data(),
            session_->inputBuffer.size(),
            session_->inputShape.data(),
            session_->inputShape.size());

        const char* inputNames[]  = { session_->inputName.c_str() };
        const char* outputNames[] = { session_->outputName.c_str() };

        auto outputs = session_->session->Run(
            Ort::RunOptions { nullptr }, inputNames, &inputTensor, 1, outputNames, 1);
        if (outputs.empty()) return false;

        const float* out = outputs[0].GetTensorData<float>();
        const std::size_t outCount = static_cast<std::size_t>(numModelClasses_);
        // Copy into probsScratch_ (pre-sized) for the remap helper.
        std::copy_n(out, outCount, probsScratch_.data());

        // Remap the model's softmax onto the 12 plugin slots via the shared
        // helper (also unit-tested directly in TestGenreClassifierRemap). If the
        // argmax output class is unmapped (-1), the helper returns -1 and we fall
        // back to the heuristic rather than asserting a wrong genre.
        std::array<float, kNumGenres> remapped {};
        const int pluginSlot = remapGenreProbs(probsScratch_.data(), numModelClasses_,
                                               genreRemap_.data(),
                                               static_cast<int>(genreRemap_.size()),
                                               remapped.data());
        if (pluginSlot < 0) return false;

        const float conf = remapped[static_cast<std::size_t>(pluginSlot)];

        // Publish into the double-buffered probability vector.
        const int back = frontBuffer_.load(std::memory_order_relaxed) == 0 ? 1 : 0;
        auto& out2 = (back == 0) ? probsA_ : probsB_;
        std::copy(remapped.begin(), remapped.end(), out2.begin());
        frontBuffer_.store(back, std::memory_order_release);

        topGenre_.store(pluginSlot, std::memory_order_relaxed);
        topConf_.store(conf, std::memory_order_relaxed);
        return true;
    }
    catch (...)
    {
        return false;   // ORT threw → heuristic runs
    }
}
#endif

void GenreClassifier::runHeuristicClassification_()
{
    // ponytail: no model available (or neural path failed), so guess the genre
    // from cheap time-domain features of the accumulated mono buffer. Coarse by
    // design — only good enough to unstick the AI decision chain so LUFS/EQ/
    // exciter react to what's playing. The 12-class CNN remains the upgrade path.
    if (accumulatedSamples_ <= 0 || sampleRate_ <= 0.0)
        return;

    const float* d = audioAccum_.getReadPointer(0);
    const int N = audioAccum_.getNumSamples();
    if (d == nullptr || N <= 0)
        return;

    // AUDIT-FIX-7: split low vs high band energy at ~200 Hz. The previous code
    // hardcoded a one-pole alpha of 0.05 (claiming "~200 Hz at 48k") but that is
    // actually ~402 Hz at 48k and ~837 Hz at 96k, AND computed a `split` index
    // it then threw away (juce::ignoreUnused). Derive alpha from the actual
    // sample rate for a true -3 dB point at 200 Hz: fc = alpha*fs / (2*pi*(1-alpha)).
    // Solving for alpha: alpha = 2*pi*fc / (fs + 2*pi*fc).
    constexpr double kTargetCutoffHz = 200.0;
    const double alpha = (2.0 * juce::MathConstants<double>::pi * kTargetCutoffHz)
                         / (sampleRate_ + 2.0 * juce::MathConstants<double>::pi * kTargetCutoffHz);
    double lowE = 0.0, highE = 0.0;
    double lp = 0.0;  // one-pole low-pass state (also acts as DC-blocker via residual)
    for (int i = 0; i < N; ++i)
    {
        lp += alpha * (static_cast<double>(d[i]) - lp);
        const double lo = lp;
        const double hi = static_cast<double>(d[i]) - lp;
        lowE  += lo * lo;
        highE += hi * hi;
    }

    // Zero-crossing rate → brightness/noise proxy.
    int crossings = 0;
    for (int i = 1; i < N; ++i)
        crossings += (d[i] >= 0.f) != (d[i - 1] >= 0.f);

    const double zcr = static_cast<double>(crossings) / static_cast<double>(N);
    const double lowFrac = lowE / (lowE + highE + 1e-12);

    // Map features → one of a few representative genres. Index reference:
    // 0 electronic_dance, 1 house_techno, 2 hip_hop_rnb, 3 pop, 4 rock,
    // 5 folk_acoustic, 6 jazz, 7 classical, 8 ambient, 9 metal,
    // 10 streaming_default, 11 broadcast
    int guess = 10;
    float conf = 0.6f;
    if (lowFrac > 0.55 && zcr < 0.08)        { guess = 2;  conf = 0.65f; }  // hip_hop/rnb: bass-heavy, dark
    else if (lowFrac > 0.45 && zcr < 0.10)   { guess = 1;  conf = 0.6f;  }  // house/techno: punchy low end
    else if (lowFrac < 0.30 && zcr < 0.06)   { guess = 7;  conf = 0.6f;  }  // classical: bright-ish, low ZCR
    else if (lowFrac < 0.25)                 { guess = 5;  conf = 0.55f; }  // folk/acoustic: sparse low end
    else if (zcr > 0.18)                     { guess = 9;  conf = 0.6f;  }  // metal: bright/noisy
    else if (zcr > 0.13)                     { guess = 4;  conf = 0.55f; }  // rock
    else if (lowFrac > 0.40)                 { guess = 0;  conf = 0.55f; }  // electronic_dance
    else                                      { guess = 3;  conf = 0.5f;  }  // pop fallback

    topGenre_.store(guess, std::memory_order_relaxed);
    topConf_.store(conf,  std::memory_order_relaxed);

    // Publish the full probability vector (sparse: top = conf, rest spread).
    const int back = frontBuffer_.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    auto& out = (back == 0) ? probsA_ : probsB_;
    out.fill(0.f);
    out[guess] = conf;
    const float rest = (1.f - conf) / static_cast<float>(kNumGenres - 1);
    for (int i = 0; i < kNumGenres; ++i)
        if (i != guess) out[i] = rest;
    frontBuffer_.store(back, std::memory_order_release);
}

void GenreClassifier::timerCallback()
{
    ++classificationTimer_;
    if (classificationTimer_ < kAnalysisIntervalSeconds) return;
    classificationTimer_ = 0;

    if (!hasNewAudio_.load(std::memory_order_acquire)) return;
    hasNewAudio_.store(false, std::memory_order_relaxed);

    runClassification();

    // ponytail: reset the accumulator AFTER classification so runClassification's
    // accumulatedSamples_ guard sees the freshly captured window, not zero.
    accumulatedSamples_ = 0;
}

} // namespace more_phi
