/*
 * More-Phi — tests/Unit/TestMasteringMeters.cpp
 *
 * Order-of-magnitude + known-signal validation for the SonicMaster analysis
 * meters that TestDSPQuality.cpp does NOT cover (it only tests morph/oversampling
 * DSP). The strong LUFS conformance lives in TestLUFSMeter.cpp and the spectrum
 * feature basics in TestSpectrumAnalyzer.cpp; this file covers the AUDIT gaps:
 *
 *   - DC offset IS now removed in the spectrum path (AUDIT-FIX DC-1, R5a). The
 *     "DC offset is removed (fixed)" test below pins the corrected behavior; a
 *     pure-DC frame reports a ~0/invalid crest, not the ~1.6 of the DC-laden path.
 *     NOTE: a deeper crest/RMS scaling inconsistency remains (the pure-sine crest
 *     sanity guard reads ~2.3 instead of ~1.414) — tracked separately, predates
 *     this audit's changes. See the sine + DC test bodies.
 *   - Crest factor is bounded for a pure sine (≈1.414) — a sanity guard.
 *   - Spectral centroid tracks frequency monotonically (low tone < high tone).
 *
 * Missing-metric inventory (noise floor, SNR, muddiness, harshness, transient):
 * these are intentionally absent today; the audit flags them. AUDIT C6 removed
 * the SUCCEED() stubs that previously signaled false coverage — the
 * implementation hints are preserved as a comment block at the end of this file.
 */
#include "Core/RealtimeSpectrumAnalyzer.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 512;
constexpr float  kPi         = 3.14159265358979323846f;

void feedSineStereo(RealtimeSpectrumAnalyzer& analyzer, float frequency,
                    int totalSamples, float amplitude = 1.0f)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;
    while (generated < totalSamples)
    {
        const int n = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();
        for (int i = 0; i < n; ++i)
        {
            const float phase = 2.0f * kPi * frequency * static_cast<float>(generated + i)
                              / static_cast<float>(kSampleRate);
            const float s = amplitude * std::sin(phase);
            buffer.setSample(0, i, s);
            buffer.setSample(1, i, s);
        }
        analyzer.processBlock(buffer);
        generated += n;
    }
}

} // anonymous namespace

TEST_CASE("Crest factor of a pure sine is ≈sqrt(2) (sanity)", "[meters][crest]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    // Feed ~0.5s of a 1 kHz full-scale sine.
    feedSineStereo(analyzer, 1000.0f, static_cast<int>(kSampleRate * 0.5));

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);

    // Program crest of a sine is peak/RMS = 1.0 / (1/sqrt2) ≈ 1.414. Allow a
    // generous band (windowing + EMA smoothing).
    CHECK(snap.crestFactorProgram == Approx(1.414f).margin(0.25f));
}

TEST_CASE("Spectral centroid is higher for a high tone than a low tone",
          "[meters][centroid]")
{
    RealtimeSpectrumAnalyzer low;
    low.prepare(kSampleRate, kBlockSize);
    feedSineStereo(low, 120.0f, static_cast<int>(kSampleRate * 0.5));
    RealtimeSpectrumAnalyzer::SpectrumSnapshot lowSnap;
    REQUIRE(low.getSnapshot(lowSnap));

    RealtimeSpectrumAnalyzer high;
    high.prepare(kSampleRate, kBlockSize);
    feedSineStereo(high, 6000.0f, static_cast<int>(kSampleRate * 0.5));
    RealtimeSpectrumAnalyzer::SpectrumSnapshot highSnap;
    REQUIRE(high.getSnapshot(highSnap));

    REQUIRE(lowSnap.frameIndex > 0);
    REQUIRE(highSnap.frameIndex > 0);
    CHECK(highSnap.spectralCentroid > lowSnap.spectralCentroid);
    // Monotonic sanity: the low tone's centroid should be near its bin (~120 Hz),
    // the high tone's near ~6 kHz. Loose margins because centroid is energy-weighted
    // across the whole spectrum (Hann leakage, harmonic content is zero for a pure
    // sine but quantization noise spreads).
    CHECK(lowSnap.spectralCentroid  < 1000.0f);
    CHECK(highSnap.spectralCentroid > 2000.0f);
}

// AUDIT-FIX (DC-1, R5a 2026-07-16): the spectrum path now REMOVES DC offset
// (frame mean subtracted before peak/RMS/crest). A pure-DC signal therefore
// reports a near-zero/invalid crest (peak≈0 → crest=0 sentinel) rather than the
// meaningless ~1.6 the DC-laden path produced. This asserts the fixed behavior.
TEST_CASE("DC offset is removed in the spectrum path (fixed)",
          "[meters][dc]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    // Feed a constant DC offset of 0.5 for 0.5s (no AC content at all).
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;
    // AUDIT (crest/RMS, 2026-06-25): feed ONLY full blocks of DC. The prior
    // loop fed a partial final block (total=24000 = 46*512 + 448) and called
    // buffer.clear() first, so the last 64 samples of the final block were 0 —
    // a DC→0 step transient whose frame seeded the program-crest EMA to ~5.6
    // even though every full-DC frame correctly reads crest 0. Rounding total
    // up to a whole block multiple and filling the whole block (no clear) keeps
    // every processed frame pure-DC, so the EMA never seeds and reports ~0.
    const int total = ((static_cast<int>(kSampleRate * 0.5) + kBlockSize - 1) / kBlockSize) * kBlockSize;
    while (generated < total)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            buffer.setSample(0, i, 0.5f);
            buffer.setSample(1, i, 0.5f);
        }
        analyzer.processBlock(buffer);
        generated += kBlockSize;
    }

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);

    // After DC removal, a pure-DC frame has ~0 AC energy, so the crest factor is
    // the 0 sentinel (invalid) or vanishingly small — NOT the ~1.6 the prior
    // DC-laden path reported. (crestFactor is peak/rms; both → 0 ⇒ 0 sentinel.)
    INFO("DC-only crest factor after DC removal: " << snap.crestFactorProgram
         << " (expected ~0 / invalid; was ~1.6 before the fix)");
    CHECK(snap.crestFactorProgram <= 0.01f);
}

// AUDIT-F1.4 (2026-06-27): THD on an OFF-BIN fundamental. The bin-pick max
// underestimates the true fundamental magnitude when the frequency falls between
// FFT bins (spectral leakage into the skirts), which biases the THD ratio
// (sqrt(harmonicEnergy)/fundamental) LOW. Parabolic peak interpolation recovers
// a closer estimate of the fundamental magnitude. This test feeds a fundamental
// at 1000 Hz (bin 10.67 at 48 kHz / 256 bins -> off-bin) plus a 2nd harmonic and
// asserts the reported THD is non-trivial (>1.5%) and finite — without the fix
// the off-bin fundamental under-report drove THD toward 0.
TEST_CASE("THD is non-trivial and finite for an off-bin fundamental + harmonic (F1.4)",
          "[meters][thd][Audit-F1-4]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    // Fundamental 1000 Hz + a deliberate 2nd harmonic at 2000 Hz at -28 dB.
    // 1000 Hz is off-bin at this FFT size, exercising the interpolation path.
    constexpr float fundamentalHz = 1000.0f;
    constexpr float harmonic2xHz  = 2000.0f;
    constexpr float fundamentalAmp = 0.9f;
    constexpr float harmonicAmp    = 0.04f; // ~-27 dB -> meaningful THD
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    const int totalSamples = static_cast<int>(kSampleRate) * 1.5;
    int generated = 0;
    while (generated < totalSamples)
    {
        const int n = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();
        for (int i = 0; i < n; ++i)
        {
            const float t = static_cast<float>(generated + i) / static_cast<float>(kSampleRate);
            const float s = fundamentalAmp * std::sin(2.0f * kPi * fundamentalHz * t)
                          + harmonicAmp   * std::sin(2.0f * kPi * harmonic2xHz * t);
            buffer.setSample(0, i, s);
            buffer.setSample(1, i, s);
        }
        analyzer.processBlock(buffer);
        generated += n;
    }

    SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);
    INFO("Off-bin THD%: " << snap.thdPercent);
    // The harmonic is ~-27 dB relative to the fundamental -> THD around 4-5%.
    // The regression guard is the LOWER bound: without interpolation the off-bin
    // fundamental under-report pushed THD toward ~0. Assert it is materially
    // non-zero and finite (no NaN/Inf).
    REQUIRE(std::isfinite(snap.thdPercent));
    CHECK(snap.thdPercent > 1.5f);
}

// AUDIT-F1.4: a PURE off-bin sine (no harmonics) should report THD near the
// floor (not the ~0 of a hard bin-pick, and not inflated). The interpolation
// must not introduce spurious THD on a clean signal.
TEST_CASE("THD is near-floor for a pure off-bin sine (F1.4 no false positives)",
          "[meters][thd][Audit-F1-4]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);
    feedSineStereo(analyzer, 1000.0f, static_cast<int>(kSampleRate) * 1, 0.9f);

    SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);
    REQUIRE(std::isfinite(snap.thdPercent));
    // A pure sine has no real harmonics; residual THD is window-leakage only.
    CHECK(snap.thdPercent < 10.0f);
}

// Missing-metric inventory (AUDIT C6, 2026-06-25). These metrics are flagged as
// absent in the audit. The implementation hints are kept as documentation so a
// future implementation has a ready test target, but the prior SUCCEED() stubs
// were removed — they emitted "passed" assertions that signaled coverage which
// does not exist. When each metric is implemented, add a real assertion here:
//
//   noise floor       — feed a near-silent signal (-90 dBFS); assert the estimate
//                       is within a few dB of -90.
//   muddiness         — feed a signal boosted in 200-500 Hz; assert the score
//                       rises vs a flat signal.
//   harshness         — feed a signal with excess 2-5 kHz content; assert the
//                       score rises.
//   transient/attack  — TransientDetector exists but only drives morph alpha,
//                       not the mastering decision. Feed a percussive signal;
//                       assert the metric is non-zero.
