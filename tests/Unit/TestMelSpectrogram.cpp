// tests/Unit/TestMelSpectrogram.cpp
//
// Phase A: the 128-bin log-mel frontend. Pure DSP — no ONNX, no model file.
// Pins four invariants: mel bins are monotonic in frequency (a higher tone
// peaks at a higher bin), silence produces finite non-NaN output, a DC input
// concentrates energy in the lowest bin, and the output is deterministic
// (same input twice → same output).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/MelSpectrogram.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr double kSr = 22050.0;   // lower rate → fewer frames, faster test
// Use a window short enough to keep the test fast but long enough to yield >=1
// STFT frame. mel_ uses FFT 2048 / hop 512 internally.

// Generate `numSamples` of a pure tone at `freqHz`, amplitude 0.8.
std::vector<float> tone(double freqHz, std::size_t numSamples, double sr)
{
    std::vector<float> v(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
        v[i] = static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979 * freqHz * i / sr));
    return v;
}

// argmax over the per-frame mel slice for frame 0: which mel bin has the most
// energy. The mel bin axis is monotonic in centre frequency, so a higher tone
// should peak at a higher-or-equal mel bin.
int argmaxMelBinFrame0(const more_phi::MelSpectrogram& mel)
{
    const float* out = mel.getOutput();
    const std::size_t frames = mel.getFrameCount();
    if (frames == 0) return -1;
    int best = 0;
    float bestVal = out[0];   // row-major [mel][frame] → frame 0 at offsets 0,frames,2*frames,...
    for (int m = 0; m < mel.kMelBins; ++m)
    {
        const float v = out[static_cast<std::size_t>(m) * frames];
        if (v > bestVal) { bestVal = v; best = m; }
    }
    return best;
}

} // namespace

TEST_CASE("MelSpectrogram: higher tone peaks at a higher mel bin",
          "[MelSpectrogram]")
{
    // Two tones an octave apart. The higher one's mel energy peak must be at a
    // bin >= the lower one's (mel bins are monotonic in frequency).
    constexpr std::size_t kSamples = 1 * static_cast<std::size_t>(kSr);  // 1 s
    more_phi::MelSpectrogram mel;
    mel.prepare(kSr, kSamples);
    REQUIRE(mel.getFrameCount() > 0);

    const auto lo = tone(220.0, kSamples, kSr);
    mel.process(lo.data(), kSamples);
    const int binLo = argmaxMelBinFrame0(mel);

    const auto hi = tone(1760.0, kSamples, kSr);   // ~3 octaves up
    mel.process(hi.data(), kSamples);
    const int binHi = argmaxMelBinFrame0(mel);

    CHECK(binHi > binLo);
}

TEST_CASE("MelSpectrogram: silence is finite and non-NaN",
          "[MelSpectrogram]")
{
    constexpr std::size_t kSamples = 1 * static_cast<std::size_t>(kSr);
    more_phi::MelSpectrogram mel;
    mel.prepare(kSr, kSamples);

    std::vector<float> silence(kSamples, 0.0f);
    mel.process(silence.data(), kSamples);

    const float* out = mel.getOutput();
    const std::size_t n = mel.getOutputCount();
    REQUIRE(n > 0);
    for (std::size_t i = 0; i < n; ++i)
    {
        CHECK(std::isfinite(out[i]));
    }
}

TEST_CASE("MelSpectrogram: DC concentrates energy in the lowest bin",
          "[MelSpectrogram]")
{
    // A constant (DC) signal has all its energy at bin 0 (DC). After the mel
    // filterbank (triangles starting at 0 Hz), the lowest mel bin dominates.
    constexpr std::size_t kSamples = 1 * static_cast<std::size_t>(kSr);
    more_phi::MelSpectrogram mel;
    mel.prepare(kSr, kSamples);

    std::vector<float> dc(kSamples, 0.5f);
    mel.process(dc.data(), kSamples);

    const int bin = argmaxMelBinFrame0(mel);
    // The lowest 1-2 mel bins should carry the DC energy.
    CHECK(bin <= 1);
}

TEST_CASE("MelSpectrogram: deterministic (same input → same output)",
          "[MelSpectrogram]")
{
    constexpr std::size_t kSamples = 1 * static_cast<std::size_t>(kSr);
    more_phi::MelSpectrogram mel;
    mel.prepare(kSr, kSamples);

    const auto sig = tone(440.0, kSamples, kSr);
    mel.process(sig.data(), kSamples);
    std::vector<float> first(mel.getOutput(), mel.getOutput() + mel.getOutputCount());

    mel.process(sig.data(), kSamples);
    std::vector<float> second(mel.getOutput(), mel.getOutput() + mel.getOutputCount());

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
        CHECK(first[i] == Catch::Approx(second[i]).margin(1e-6f));
}

TEST_CASE("MelSpectrogram: prepare sizes output to melBins * frameCount",
          "[MelSpectrogram]")
{
    constexpr std::size_t kSamples = 2 * static_cast<std::size_t>(kSr);
    more_phi::MelSpectrogram mel;
    mel.prepare(kSr, kSamples);

    const std::size_t expected = static_cast<std::size_t>(mel.kMelBins) * mel.getFrameCount();
    CHECK(mel.getOutputCount() == expected);
    CHECK(mel.getFrameCount() > 0);
}
