#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Plugin/PluginProcessor.h"

#include <cmath>
#include <map>
#include <set>

namespace more_phi::test {

namespace {

juce::RangedAudioParameter& requireRangedParameter(MorePhiProcessor& processor, const char* parameterId)
{
    auto* parameter = processor.getAPVTS().getParameter(parameterId);
    INFO("parameterId=" << parameterId);
    REQUIRE(parameter != nullptr);
    return *parameter;
}

float requireRawParameterValue(MorePhiProcessor& processor, const char* parameterId)
{
    auto* value = processor.getAPVTS().getRawParameterValue(parameterId);
    INFO("parameterId=" << parameterId);
    REQUIRE(value != nullptr);
    return value->load(std::memory_order_relaxed);
}

void setNormalizedWithGesture(juce::RangedAudioParameter& parameter, float normalizedValue)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(normalizedValue);
    parameter.endChangeGesture();
}

juce::AudioBuffer<float> makeConstantStereoBuffer(float left, float right, int numSamples = 256)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    buffer.clear();
    buffer.applyGain(0.0f);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        buffer.setSample(0, sample, left);
        buffer.setSample(1, sample, right);
    }

    return buffer;
}

void processOneBlock(MorePhiProcessor& processor, juce::AudioBuffer<float>& buffer)
{
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

} // namespace

TEST_CASE("VST3 parameter automation exposes documented parameters to the host", "[integration][vst3][automation]")
{
    MorePhiProcessor processor;

    const std::set<juce::String> documentedParameters = {
        "morphX",
        "morphY",
        "faderPos",
        "physicsMode",
        "smoothing",
        "driftSpeed",
        "driftDistance",
        "driftChaos",
        "outputGain",
        "bypass",
        "sanityEnabled",
        "recallMode",
        "sidechainEnabled",
        "sidechainThreshold",
        "listenMode",
        "recallToggle",
        "driftOutputX",
        "driftOutputY",
        "smartRandomize",
        "linkMode",
    };

    std::set<juce::String> exposedParameterIds;
    std::map<juce::String, juce::AudioProcessorParameter*> exposedParameters;

    for (auto* parameter : processor.getParameters())
    {
        REQUIRE(parameter != nullptr);
        REQUIRE(parameter->getName(128).isNotEmpty());
        REQUIRE(parameter->getValue() >= 0.0f);
        REQUIRE(parameter->getValue() <= 1.0f);

        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter))
        {
            exposedParameterIds.insert(withId->paramID);
            exposedParameters.emplace(withId->paramID, parameter);
        }
    }

    for (const auto& parameterId : documentedParameters)
    {
        INFO("parameterId=" << parameterId);
        REQUIRE(exposedParameterIds.count(parameterId) == 1);
        REQUIRE(exposedParameters.at(parameterId)->isAutomatable());
    }
}

TEST_CASE("VST3 parameter automation updates representative APVTS values", "[integration][vst3][automation]")
{
    MorePhiProcessor processor;

    setNormalizedWithGesture(requireRangedParameter(processor, "morphX"), 0.75f);
    REQUIRE(requireRawParameterValue(processor, "morphX") == Catch::Approx(0.75f));

    setNormalizedWithGesture(requireRangedParameter(processor, "listenMode"), 1.0f);
    REQUIRE(requireRawParameterValue(processor, "listenMode") == Catch::Approx(1.0f));

    setNormalizedWithGesture(requireRangedParameter(processor, "recallMode"), 1.0f);
    REQUIRE(requireRawParameterValue(processor, "recallMode") == Catch::Approx(1.0f));
}

TEST_CASE("VST3 parameter automation syncs APVTS values into runtime state", "[integration][vst3][automation]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    setNormalizedWithGesture(requireRangedParameter(processor, "listenMode"), 1.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "linkMode"), 1.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "sidechainEnabled"), 1.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "recallToggle"), 0.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "recallMode"), 1.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "sidechainThreshold"), 0.75f);

    auto buffer = makeConstantStereoBuffer(0.05f, -0.05f);
    processOneBlock(processor, buffer);

    REQUIRE(processor.getListenMode());
    REQUIRE(processor.getLinkEnabled());
    REQUIRE(processor.getSidechainEnabled());
    REQUIRE_FALSE(processor.getRecallToggle());
    REQUIRE(processor.getRecallMode() == 1);
    REQUIRE(processor.getSidechainThreshold() == Catch::Approx(-15.0f).margin(0.001f));

    processor.releaseResources();
}

TEST_CASE("VST3 parameter automation applies output gain during processing", "[integration][vst3][automation][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    // outputProtect engages the lookahead brickwall limiter (≈192-sample delay),
    // which silences sample[0]. Bypass it to measure gain at sample[0]. The
    // limiter is exercised by its own dedicated tests.
    setNormalizedWithGesture(requireRangedParameter(processor, "outputProtect"), 0.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "outputGain"), 0.25f);

    auto buffer = makeConstantStereoBuffer(0.25f, -0.25f);
    processOneBlock(processor, buffer);

    const float expectedGain = juce::Decibels::decibelsToGain(-12.0f);
    REQUIRE(buffer.getSample(0, 0) == Catch::Approx(0.25f * expectedGain).margin(0.0001f));
    REQUIRE(buffer.getSample(1, 0) == Catch::Approx(-0.25f * expectedGain).margin(0.0001f));

    processor.releaseResources();
}

TEST_CASE("VST3 parameter automation bypass remains transparent with no hosted plugin", "[integration][vst3][automation][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    setNormalizedWithGesture(requireRangedParameter(processor, "bypass"), 1.0f);
    setNormalizedWithGesture(requireRangedParameter(processor, "outputGain"), 1.0f);

    auto buffer = makeConstantStereoBuffer(0.2f, -0.125f);
    processOneBlock(processor, buffer);

    REQUIRE(buffer.getSample(0, 0) == Catch::Approx(0.2f).margin(0.000001f));
    REQUIRE(buffer.getSample(1, 0) == Catch::Approx(-0.125f).margin(0.000001f));

    processor.releaseResources();
}

TEST_CASE("VST3 smart randomize parameter is exposed as automatable but has no processor trigger path", "[integration][vst3][automation][documented-mismatch]")
{
    MorePhiProcessor processor;

    auto& smartRandomize = requireRangedParameter(processor, "smartRandomize");
    setNormalizedWithGesture(smartRandomize, 1.0f);

    REQUIRE(requireRawParameterValue(processor, "smartRandomize") == Catch::Approx(1.0f));
}

} // namespace more_phi::test
