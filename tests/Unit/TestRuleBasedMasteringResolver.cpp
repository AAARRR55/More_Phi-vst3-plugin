/*
 * More-Phi — tests/Unit/TestRuleBasedMasteringResolver.cpp
 *
 * Unit tests for the deterministic rule-based mastering resolver.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/RuleBasedMasteringResolver.h"
#include "Core/RealtimeSpectrumAnalyzer.h"
#include "Core/StereoFieldAnalyzer.h"

using namespace more_phi;

namespace {

RealtimeSpectrumAnalyzer::SpectrumSnapshot makeFlatSpectrum(float levelDb = -20.0f)
{
    RealtimeSpectrumAnalyzer::SpectrumSnapshot s;
    s.binCount = RealtimeSpectrumAnalyzer::kMaxBins;
    s.sampleRate = 48000.0;
    s.fftSize = 2048;
    s.magnitudeDB.fill(levelDb);
    s.spectralCentroid = 1000.0f;
    s.frameIndex = 1;
    return s;
}

RealtimeSpectrumAnalyzer::SpectrumSnapshot makeDarkSpectrum()
{
    auto s = makeFlatSpectrum(-20.0f);
    // Dark = highs rolled off by 12 dB relative to 1 kHz.
    for (int i = 0; i < s.binCount; ++i)
    {
        const float freq = static_cast<float>(i) * 48000.0f / 2048.0f;
        if (freq > 1000.0f)
            s.magnitudeDB[static_cast<std::size_t>(i)] -= 12.0f * std::log2(freq / 1000.0f);
    }
    s.spectralCentroid = 400.0f;
    return s;
}

RealtimeSpectrumAnalyzer::SpectrumSnapshot makeBrightSpectrum()
{
    auto s = makeFlatSpectrum(-20.0f);
    // Bright = highs lifted by 9 dB relative to 1 kHz.
    for (int i = 0; i < s.binCount; ++i)
    {
        const float freq = static_cast<float>(i) * 48000.0f / 2048.0f;
        if (freq > 1000.0f)
            s.magnitudeDB[static_cast<std::size_t>(i)] += 9.0f * std::log2(freq / 1000.0f);
    }
    s.spectralCentroid = 8000.0f;
    return s;
}

StereoFieldAnalyzer::StereoFieldSnapshot makeNarrowStereo()
{
    StereoFieldAnalyzer::StereoFieldSnapshot s;
    s.sampleRate = 48000.0;
    s.stereoWidth = 0.2f;
    s.correlation.fill(0.95f);
    s.frameIndex = 1;
    return s;
}

StereoFieldAnalyzer::StereoFieldSnapshot makePhaseyStereo()
{
    StereoFieldAnalyzer::StereoFieldSnapshot s;
    s.sampleRate = 48000.0;
    s.stereoWidth = 0.9f;
    s.correlation.fill(-0.3f);
    s.frameIndex = 1;
    return s;
}

RuleBasedMasteringInput makeInput(RealtimeSpectrumAnalyzer::SpectrumSnapshot spectrum,
                                   StereoFieldAnalyzer::StereoFieldSnapshot stereo,
                                   float lra = 8.0f,
                                   float crest = 10.0f,
                                   float intensity = 0.5f,
                                   float targetLufs = -14.0f,
                                   float truePeakDbTp = -2.0f)
{
    RuleBasedMasteringInput in;
    in.spectrum = spectrum;
    in.stereo = stereo;
    in.lra = lra;
    in.crestFactor = crest;
    in.intensity = intensity;
    in.targetLufs = targetLufs;
    in.truePeakDbTp = truePeakDbTp;
    in.lufsIntegrated = -16.0f;
    return in;
}

// Parse a single gain for a given frequency from the EQ prescription JSON.
float parseGainAtFreq(const juce::String& json, float freqHz)
{
    juce::var parsed;
    if (juce::JSON::parse(json, parsed).failed())
        return -999.0f;

    const auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return -999.0f;
    const auto* bandsVar = obj->getProperties().getVarPointer("bands");
    if (bandsVar == nullptr || !bandsVar->isArray()) return -999.0f;

    const auto* arr = bandsVar->getArray();
    if (arr == nullptr) return -999.0f;

    for (const auto& band : *arr)
    {
        const float freq = static_cast<float>(static_cast<double>(band["freq"]));
        if (std::abs(freq - freqHz) < 1.0f)
            return static_cast<float>(static_cast<double>(band["gain"]));
    }
    return -999.0f;
}

} // namespace

TEST_CASE("RuleBasedMasteringResolver produces a valid plan", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const auto plan = resolver.resolve(makeInput(makeFlatSpectrum(), makeNarrowStereo()));

    REQUIRE(plan.valid);
    REQUIRE(plan.compressionNeed >= 0.0f);
    REQUIRE(plan.compressionNeed <= 1.0f);
    REQUIRE(plan.targetLUFS >= -23.0f);
    REQUIRE(plan.targetLUFS <= -8.0f);
    REQUIRE(plan.ceilingDBTP <= -0.1f);
    REQUIRE(plan.ceilingDBTP >= -3.0f);
    REQUIRE(!plan.eqPrescriptionJSON.isEmpty());
}

TEST_CASE("Dark spectrum gets high-frequency boost", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const auto in = makeInput(makeDarkSpectrum(), makeNarrowStereo());
    const auto eq = resolver.resolveEQ(in);

    const float highShelfGain = parseGainAtFreq(eq, 10000.0f);
    REQUIRE(highShelfGain > 0.0f);
    REQUIRE(highShelfGain <= 6.0f);
}

TEST_CASE("Bright spectrum gets high-frequency cut", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const auto in = makeInput(makeBrightSpectrum(), makeNarrowStereo());
    const auto eq = resolver.resolveEQ(in);

    const float highShelfGain = parseGainAtFreq(eq, 10000.0f);
    REQUIRE(highShelfGain < 0.0f);
    REQUIRE(highShelfGain >= -6.0f);
}

TEST_CASE("Narrow stereo gets widened", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const auto plan = resolver.resolve(makeInput(makeFlatSpectrum(), makeNarrowStereo()));

    // Mid band should widen above neutral (1.0).
    REQUIRE(plan.widthCurve[2] > 1.0f);
}

TEST_CASE("Phasey stereo gets narrowed", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const auto plan = resolver.resolve(makeInput(makeFlatSpectrum(), makePhaseyStereo()));

    // All bands should be at or below neutral.
    for (int i = 0; i < 4; ++i)
        REQUIRE(plan.widthCurve[i] <= 1.0f);
}

TEST_CASE("High dynamic range increases compression need", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;
    const float gentle = resolver.resolveCompressionNeed(makeInput(makeFlatSpectrum(), makeNarrowStereo(), 3.0f, 5.0f));
    const float aggressive = resolver.resolveCompressionNeed(makeInput(makeFlatSpectrum(), makeNarrowStereo(), 12.0f, 16.0f));

    REQUIRE(aggressive > gentle);
}

TEST_CASE("Target LUFS and ceiling are clamped safely", "[rulebased][mastering]")
{
    RuleBasedMasteringResolver resolver;

    const auto loud = resolver.resolve(makeInput(makeFlatSpectrum(), makeNarrowStereo(), 8.0f, 10.0f, 1.0f, 0.0f));
    REQUIRE(loud.targetLUFS == Catch::Approx(-8.0f).margin(0.01f));

    const auto quiet = resolver.resolve(makeInput(makeFlatSpectrum(), makeNarrowStereo(), 8.0f, 10.0f, 0.0f, -40.0f));
    REQUIRE(quiet.targetLUFS == Catch::Approx(-23.0f).margin(0.01f));

    const auto hotPeak = resolver.resolve(makeInput(makeFlatSpectrum(), makeNarrowStereo(), 8.0f, 10.0f, 0.5f, -14.0f, -0.5f));
    REQUIRE(hotPeak.ceilingDBTP <= -1.0f);
}

TEST_CASE("Legacy ChainPlanExecutor overload still returns valid plan", "[rulebased][mastering]")
{
    ChainPlanExecutor planner;
    const auto plan = planner.previewPlan(0, 8.0f, 0.0f, 0.7f);

    REQUIRE(plan.valid);
    REQUIRE(!plan.eqPrescriptionJSON.isEmpty());
    REQUIRE(plan.targetLUFS <= -8.0f);
    REQUIRE(plan.targetLUFS >= -23.0f);
}
