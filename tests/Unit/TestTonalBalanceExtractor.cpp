// tests/Unit/TestTonalBalanceExtractor.cpp
//
// Stage 2 (Ozone §3.2): the tonal-balance extractor integrates the spectrum
// snapshot into the 8 SonicMaster EQ bands, mean-subtracted so the result is a
// level-invariant SHAPE. The decoder pairs this with a MasteringTargetCurve to
// form the residual that gets blended into the EQ recommendation.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/TonalBalanceExtractor.h"
#include "Core/RealtimeSpectrumAnalyzer.h"
#include "AI/SonicMasterDecisionDecoder.h"

#include <cmath>

namespace {

using more_phi::RealtimeSpectrumAnalyzer;

// Build a synthetic snapshot with a flat magnitude (all bins = flatDb) so the
// extractor's mean-subtraction yields ~0 dB everywhere — the level-invariant
// shape of a flat spectrum.
RealtimeSpectrumAnalyzer::SpectrumSnapshot flatSnapshot(float flatDb, int bins = 64)
{
    RealtimeSpectrumAnalyzer::SpectrumSnapshot s {};
    s.binCount = bins;
    s.sampleRate = 48000.0;
    s.fftSize = 2048;
    s.frameIndex = 1;
    for (int i = 0; i < bins; ++i) s.magnitudeDB[static_cast<size_t>(i)] = flatDb;
    return s;
}

} // namespace

TEST_CASE("Flat spectrum extracts to ~0 dB shape (level-invariant)",
          "[TonalBalance][Extractor]")
{
    // Two different absolute levels (−40 and −10 dB) must yield the SAME shape
    // (~0 dB) because the extractor mean-subtracts. This is the property that
    // makes the residual independent of input gain.
    const auto shape1 = more_phi::extractTonalBalanceDb(flatSnapshot(-40.0f));
    const auto shape2 = more_phi::extractTonalBalanceDb(flatSnapshot(-10.0f));
    for (std::size_t i = 0; i < shape1.size(); ++i)
    {
        CHECK(shape1[i] == Catch::Approx(0.0f).margin(1e-3f));
        CHECK(shape2[i] == Catch::Approx(0.0f).margin(1e-3f));
    }
}

TEST_CASE("Tilted spectrum produces monotonic per-band shape",
          "[TonalBalance][Extractor]")
{
    // Spectrum that rises 3 dB per bin — high frequencies louder. The 8 EQ band
    // centres (60..10k Hz) map to increasing bin indices, so the extracted shape
    // must be monotonically increasing.
    RealtimeSpectrumAnalyzer::SpectrumSnapshot s {};
    s.binCount = 128;
    s.sampleRate = 48000.0;
    s.fftSize = 2048;
    s.frameIndex = 1;
    for (int i = 0; i < 128; ++i)
        s.magnitudeDB[static_cast<size_t>(i)] = -60.0f + 0.5f * static_cast<float>(i);
    const auto bands = more_phi::extractTonalBalanceDb(s);

    for (std::size_t i = 1; i < bands.size(); ++i)
        CHECK(bands[i] >= bands[i - 1] - 1e-3f);  // non-decreasing (allow rounding)
}

TEST_CASE("Empty snapshot yields finite zero shape, not NaN",
          "[TonalBalance][Extractor]")
{
    RealtimeSpectrumAnalyzer::SpectrumSnapshot s {};  // binCount=0
    const auto bands = more_phi::extractTonalBalanceDb(s);
    for (auto b : bands)
    {
        CHECK(std::isfinite(b));
        CHECK(b == Catch::Approx(0.0f).margin(1e-3f));
    }
}
