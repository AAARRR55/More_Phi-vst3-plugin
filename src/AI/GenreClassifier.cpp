/*
 * More-Phi — AI/GenreClassifier.cpp
 */
#include "GenreClassifier.h"
#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

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
    DBG("GenreClassifier: ONNX model loading not yet implemented. Using default genre.");
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
    if (modelLoaded_.load(std::memory_order_relaxed))
    {
        // ONNX path: see header comment for the intended pipeline.
        // (Not wired — model export is blocked. Falls through to heuristic.)
    }

    // ponytail: no model available, so guess the genre from cheap time-domain
    // features of the accumulated mono buffer. Coarse by design — only good
    // enough to unstick the AI decision chain so LUFS/EQ/exciter react to what's
    // playing. The 12-class CNN remains the upgrade path.
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
