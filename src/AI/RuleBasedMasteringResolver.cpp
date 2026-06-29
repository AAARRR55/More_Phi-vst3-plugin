/*
 * More-Phi — AI/RuleBasedMasteringResolver.cpp
 *
 * Deterministic mastering parameter resolver.  See header for architecture.
 */
#include "RuleBasedMasteringResolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace more_phi {

namespace {

constexpr float kEps = 1.0e-12f;
constexpr float kMinFreq = 20.0f;
constexpr float kMaxFreq = 20000.0f;

float clampFloat(float v, float lo, float hi) noexcept
{
    return std::clamp(v, lo, hi);
}

float linearToDb(float linear) noexcept
{
    return 20.0f * std::log10(std::max(linear, kEps));
}

} // namespace

MultiEffectPlan RuleBasedMasteringResolver::resolve(const RuleBasedMasteringInput& in) const noexcept
{
    MultiEffectPlan plan;
    plan.valid = true;

    plan.compressionNeed = resolveCompressionNeed(in);
    plan.useNeuralComp = false;

    plan.eqPrescriptionJSON = resolveEQ(in);

    const auto widths = resolveWidthCurve(in);
    for (int i = 0; i < 4; ++i)
        plan.widthCurve[i] = widths[static_cast<std::size_t>(i)];

    plan.targetLUFS  = resolveTargetLUFS(in);
    plan.ceilingDBTP = resolveCeilingDBTP(in);
    plan.exciterEnabled = resolveExciterEnabled(in);

    return plan;
}

juce::String RuleBasedMasteringResolver::buildEQPrescription(const RuleBasedMasteringInput& in) const noexcept
{
    return resolveEQ(in);
}

juce::String RuleBasedMasteringResolver::resolveEQ(const RuleBasedMasteringInput& in) const noexcept
{
    const MasteringTargetCurve* curve = findTargetCurve(in.targetCurveName);
    if (curve == nullptr)
        curve = &target_curves::kStreaming;

    // If the spectrum snapshot is invalid, fall back to the target curve as a
    // static warm-start (no correction, just apply the archetype).
    if (in.spectrum.binCount <= 0 || in.spectrum.sampleRate <= 0.0)
    {
        juce::String json = "{ \"bands\": [";
        for (std::size_t i = 0; i < kEqFreqs.size(); ++i)
        {
            if (i > 0) json += ", ";
            const float gainDb = clampFloat(curve->gainsDb[i] + gainAtFrequency(*curve, kEqFreqs[i]) - gainAtFrequency(*curve, 1000.0f),
                                            kMaxEqCutDb, kMaxEqGainDb);
            json += "{ \"freq\": " + juce::String(kEqFreqs[i], 1)
                  + ", \"gain\": " + juce::String(gainDb, 2)
                  + ", \"Q\": " + juce::String(kEqQs[i], 2)
                  + ", \"type\": \"" + juce::String(kEqTypes[i]) + "\" }";
        }
        json += "] }";
        return json;
    }

    // Measured magnitude at each EQ frequency.
    std::array<float, kMasteringTargetCurveBandCount> measuredDb {};
    float sumMeasuredDb = 0.0f;
    int validMeasurements = 0;
    for (std::size_t i = 0; i < kEqFreqs.size(); ++i)
    {
        measuredDb[i] = readSpectrumDb(in.spectrum, kEqFreqs[i]);
        if (std::isfinite(measuredDb[i]))
        {
            sumMeasuredDb += measuredDb[i];
            ++validMeasurements;
        }
    }

    // Level-invariant relative shape: subtract average measured level.
    const float avgMeasuredDb = validMeasurements > 0 ? sumMeasuredDb / static_cast<float>(validMeasurements) : 0.0f;

    juce::String json = "{ \"bands\": [";
    for (std::size_t i = 0; i < kEqFreqs.size(); ++i)
    {
        if (i > 0) json += ", ";

        const float measuredRelDb = std::isfinite(measuredDb[i]) ? measuredDb[i] - avgMeasuredDb : 0.0f;
        const float targetRelDb   = curve->gainsDb[i];  // already relative to 1 kHz neutral
        const float slopeDb       = gainAtFrequency(*curve, kEqFreqs[i]) - gainAtFrequency(*curve, 1000.0f);
        const float correctionDb  = (targetRelDb + slopeDb) - measuredRelDb;

        // Intensity scales how aggressively we correct (0.3..1.0 effective gain).
        const float intensityGain = 0.3f + 0.7f * clampFloat(in.intensity, 0.0f, 1.0f);
        const float gainDb = clampFloat(correctionDb * intensityGain, kMaxEqCutDb, kMaxEqGainDb);

        json += "{ \"freq\": " + juce::String(kEqFreqs[i], 1)
              + ", \"gain\": " + juce::String(gainDb, 2)
              + ", \"Q\": " + juce::String(kEqQs[i], 2)
              + ", \"type\": \"" + juce::String(kEqTypes[i]) + "\" }";
    }
    json += "] }";
    return json;
}

float RuleBasedMasteringResolver::resolveCompressionNeed(const RuleBasedMasteringInput& in) const noexcept
{
    // Base need from loudness range.
    float need = 0.5f;
    if (in.lra < 4.0f)
        need = 0.2f;
    else if (in.lra < 9.0f)
        need = 0.5f;
    else
        need = 0.8f;

    // Crest factor fine-tuning: very dynamic material needs more control;
    // already squashed material gets less.
    if (in.crestFactor > 14.0f)
        need += 0.1f;
    else if (in.crestFactor < 6.0f)
        need -= 0.15f;

    // Intensity scaling: gentle intensity pulls need down, aggressive pulls up.
    need = need * (0.6f + 0.5f * clampFloat(in.intensity, 0.0f, 1.0f));

    return clampFloat(need, kMinCompressionNeed, kMaxCompressionNeed);
}

std::array<float, 4> RuleBasedMasteringResolver::resolveWidthCurve(const RuleBasedMasteringInput& in) const noexcept
{
    std::array<float, 4> curve = { 0.0f, 0.6f, 1.0f, 1.4f }; // neutral starting curve

    // If the stereo snapshot is invalid, return the neutral curve unchanged.
    if (in.stereo.sampleRate <= 0.0)
        return curve;

    // Map the 4 StereoFieldAnalyzer bands onto our 4 width regions (sub/low/mid/high).
    // The analyzer reports correlation and M/S energy ratio per band.  High
    // correlation ≈ mono/narrow → widen.  Low/negative correlation → phase issues
    // → narrow.  We also clamp so we never invert polarity or collapse to true mono
    // (0.0) unless correlation is strongly negative.
    for (std::size_t b = 0; b < StereoFieldAnalyzer::kNumBands && b < curve.size(); ++b)
    {
        const float corr = clampFloat(in.stereo.correlation[b], -1.0f, 1.0f);
        const float msRatio = clampFloat(in.stereo.msEnergyRatio[b], 0.0f, 10.0f);

        float width = 1.0f; // default neutral width multiplier
        if (corr > 0.75f)
        {
            // Very mono-ish: widen, capped by intensity.
            width = 1.0f + 0.5f * clampFloat(in.intensity, 0.0f, 1.0f);
        }
        else if (corr < 0.0f)
        {
            // Phase problems: narrow below neutral.
            width = 0.5f;
        }
        else if (corr < 0.3f)
        {
            // Already wide: slightly narrow for focus.
            width = 0.8f;
        }

        // M/S ratio guard: if side energy is already very high, don't widen further.
        if (msRatio > 2.0f)
            width = std::min(width, 1.0f);

        curve[b] = clampFloat(width, 0.0f, 2.0f);
    }

    return curve;
}

float RuleBasedMasteringResolver::resolveCeilingDBTP(const RuleBasedMasteringInput& in) const noexcept
{
    // Never allow a ceiling laxer than -1.0 dBTP (streaming safe).
    // If the true peak of the input is close to 0 dBFS, keep a little headroom.
    const float headroom = 1.0f; // dB
    const float desiredCeiling = std::min(kStreamingSafeCeilingDBTP, in.truePeakDbTp + headroom);
    return clampFloat(desiredCeiling, kMinCeilingDBTP, kMaxCeilingDBTP);
}

float RuleBasedMasteringResolver::resolveTargetLUFS(const RuleBasedMasteringInput& in) const noexcept
{
    // Start from user intent; intensity can shift ±1 LU within the safe bounds.
    const float shift = (in.intensity - 0.5f) * 2.0f; // [-1, +1]
    return clampFloat(in.targetLufs + shift, kMinTargetLUFS, kMaxTargetLUFS);
}

bool RuleBasedMasteringResolver::resolveExciterEnabled(const RuleBasedMasteringInput& in) const noexcept
{
    // Enable exciter for bright, aggressive targets where high-frequency presence
    // is desired and the source is not already saturated.
    if (in.intensity < 0.5f)
        return false;

    const bool brightTarget = (in.targetCurveName == "electronic_dance" ||
                               in.targetCurveName == "hip_hop_rnb");
    const bool brightSource = in.spectrum.spectralCentroid > 4000.0f;
    return brightTarget && brightSource;
}

float RuleBasedMasteringResolver::gainAtFrequency(const MasteringTargetCurve& curve, float freqHz) noexcept
{
    if (freqHz <= kMinFreq)
        return 0.0f;
    const float octaves = std::log2(freqHz / 1000.0f);
    return curve.slopeDbPerOctave * octaves;
}

float RuleBasedMasteringResolver::readSpectrumDb(
    const RealtimeSpectrumAnalyzer::SpectrumSnapshot& spectrum,
    float freqHz) noexcept
{
    if (spectrum.binCount <= 0 || spectrum.sampleRate <= 0.0 || spectrum.fftSize <= 0)
        return -96.0f;

    const double sampleRate = spectrum.sampleRate;
    const int fftSize = spectrum.fftSize;
    const int numRawBins = fftSize / 2 + 1;
    const int binsPerPublished = std::max(1, (numRawBins - 1) / RealtimeSpectrumAnalyzer::kMaxBins);

    // Find the published bin whose centre raw-bin frequency is closest to freqHz.
    const float rawBinForFreq = static_cast<float>(freqHz * static_cast<double>(fftSize) / sampleRate);
    const int publishedBin = static_cast<int>(std::round(rawBinForFreq / static_cast<float>(binsPerPublished)));
    const int clampedBin = std::clamp(publishedBin, 0, static_cast<int>(spectrum.binCount) - 1);

    return spectrum.magnitudeDB[static_cast<std::size_t>(clampedBin)];
}

} // namespace more_phi
