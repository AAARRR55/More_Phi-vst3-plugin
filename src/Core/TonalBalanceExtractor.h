/*
 * More-Phi — Core/TonalBalanceExtractor.h
 *
 * Integrates the realtime spectrum analyzer's magnitude-DB snapshot into the 8
 * SonicMaster EQ band frequencies, producing a level-invariant per-band shape.
 * This is the "measured tonal balance" half of Ozone §3.2's spectral-gap
 * matching — paired with a MasteringTargetCurve it yields the residual that the
 * SonicMaster decode blends into its EQ recommendation.
 *
 * Header-only: sits on top of RealtimeSpectrumAnalyzer::SpectrumSnapshot. No new
 * analyzer, no allocation. The nearest-bin lookup mirrors
 * RuleBasedMasteringResolver::readSpectrumDb (RuleBasedMasteringResolver.cpp:229);
 * ponytail: duplicated ~15 lines rather than promoting a file-static across an
 * AI/Core boundary for one caller. If a third caller appears, promote to a
 * shared header then.
 */
#pragma once

#include "Core/RealtimeSpectrumAnalyzer.h"
#include "AI/SonicMasterDecisionDecoder.h"  // kSonicMasterEqFrequenciesHz, kSonicMasterEqGainCount

#include <array>
#include <cmath>
#include <cstddef>

namespace more_phi {

// Read the magnitude (dB) at a single frequency from a spectrum snapshot.
// Returns a finite -96.0 dB floor when the snapshot is empty/invalid (matching
// the rule-based resolver's convention so callers don't need a separate path).
inline float readBalanceDbAt(const RealtimeSpectrumAnalyzer::SpectrumSnapshot& s, float freqHz) noexcept
{
    if (s.binCount <= 0 || s.sampleRate <= 0.0 || s.fftSize <= 0)
        return -96.0f;
    const int numRawBins = s.fftSize / 2 + 1;
    const int binsPerPublished = std::max(1, (numRawBins - 1) / RealtimeSpectrumAnalyzer::kMaxBins);
    const float rawBinForFreq = static_cast<float>(freqHz * static_cast<double>(s.fftSize) / s.sampleRate);
    const int publishedBin = static_cast<int>(std::round(rawBinForFreq / static_cast<float>(binsPerPublished)));
    const int clamped = (publishedBin < 0) ? 0
                      : (publishedBin >= s.binCount) ? s.binCount - 1
                      : publishedBin;
    return s.magnitudeDB[static_cast<std::size_t>(clamped)];
}

// Integrate the spectrum into the 8 EQ band frequencies, then remove the average
// so the result is a level-invariant SHAPE (dB relative to the program's own
// mean) — the same normalization RuleBasedMasteringResolver applies. This makes
// the residual (target − measured) independent of overall input level, so a
// quiet or loud input of the same tonal shape yields the same correction.
inline std::array<float, kSonicMasterEqGainCount>
extractTonalBalanceDb(const RealtimeSpectrumAnalyzer::SpectrumSnapshot& spectrum) noexcept
{
    std::array<float, kSonicMasterEqGainCount> bands {};
    float sum = 0.0f;
    int valid = 0;
    for (std::size_t i = 0; i < bands.size(); ++i)
    {
        bands[i] = readBalanceDbAt(spectrum, kSonicMasterEqFrequenciesHz[i]);
        if (std::isfinite(bands[i])) { sum += bands[i]; ++valid; }
    }
    const float avg = valid > 0 ? sum / static_cast<float>(valid) : 0.0f;
    for (auto& b : bands)
        b = std::isfinite(b) ? (b - avg) : 0.0f;
    return bands;
}

} // namespace more_phi
