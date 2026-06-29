// tests/Unit/TestGenreClassifierFallback.cpp
//
// Phase B/D: the GenreClassifier's fail-soft discipline. With no model file
// (the shipping default), loadModel returns false and the time-domain heuristic
// keeps producing a live genre guess — so the prior-wiring never goes silent.
// These tests run with ORT either ON or OFF; no .onnx fixture is needed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/GenreClassifier.h"
#include "AI/GenreMasteringProfile.h"

#include <juce_core/juce_core.h>
#include <cmath>

namespace {
// Build `numSamples` of mono-into-stereo audio at freqHz, amplitude amp.
juce::AudioBuffer<float> tone(double freqHz, int numSamples, double sr, float amp = 0.6f)
{
    juce::AudioBuffer<float> buf(2, numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        const float s = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freqHz * i / sr));
        buf.setSample(0, i, s);
        buf.setSample(1, i, s);
    }
    return buf;
}
} // namespace

TEST_CASE("GenreClassifier: loadModel with missing file returns false",
          "[GenreClassifier]")
{
    more_phi::GenreClassifier gc;
    const auto missing = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("definitely_not_here_genre_xyz.onnx");
    CHECK_FALSE(gc.loadModel(missing));
    CHECK_FALSE(gc.isModelLoaded());
}

TEST_CASE("GenreClassifier: heuristic runs with no model and reports a valid genre",
          "[GenreClassifier][Heuristic]")
{
    // Shipping default: no model. Feed bass-heavy audio and run the heuristic
    // via the test hook. The reported genre must be in-range and its profile
    // must resolve to a finite (non-sentinel) target LUFS.
    more_phi::GenreClassifier gc;
    constexpr int kSr = 48000;
    gc.feedAudio(tone(80.0, kSr * 3, kSr, 0.6f), kSr);
    gc.runClassificationForTest();

    const int g = gc.getTopGenre();
    CHECK(g >= 0);
    CHECK(g < more_phi::GenreClassifier::kNumGenres);

    const auto profile = more_phi::getGenreMasteringProfile(g);
    CHECK(profile.targetLufs != more_phi::kUseModelTargetLufs);
}

TEST_CASE("GenreClassifier: heuristic distinguishes bright vs dark content",
          "[GenreClassifier][Heuristic]")
{
    // A bass-heavy signal and a bright/noisy signal should not both collapse to
    // the same genre — the heuristic's low/high split + ZCR must differentiate.
    constexpr int kSr = 48000;
    more_phi::GenreClassifier gcDark, gcBright;

    gcDark.feedAudio(tone(60.0, kSr * 3, kSr, 0.6f), kSr);   // dark: low fundamental
    gcDark.runClassificationForTest();
    const int gDark = gcDark.getTopGenre();

    gcBright.feedAudio(tone(4000.0, kSr * 3, kSr, 0.4f), kSr); // bright: high fundamental
    gcBright.runClassificationForTest();
    const int gBright = gcBright.getTopGenre();

    // The two should differ (not both Streaming-default). If they happen to
    // match, the heuristic isn't differentiating — the test flags that.
    CHECK(gDark != gBright);
}

TEST_CASE("GenreClassifier: unloadModel keeps a valid in-range genre",
          "[GenreClassifier]")
{
    more_phi::GenreClassifier gc;
    constexpr int kSr = 48000;
    gc.feedAudio(tone(120.0, kSr * 3, kSr, 0.5f), kSr);
    gc.runClassificationForTest();
    gc.unloadModel();

    CHECK_FALSE(gc.isModelLoaded());
    const int g = gc.getTopGenre();
    CHECK(g >= 0);
    CHECK(g < more_phi::GenreClassifier::kNumGenres);
    // unload does NOT reset to the Streaming default — the last heuristic guess
    // (or a fresh one) stays live so the prior-wiring isn't stranded.
}
