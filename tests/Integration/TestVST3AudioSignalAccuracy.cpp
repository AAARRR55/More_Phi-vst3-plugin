#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Plugin/PluginProcessor.h"

#include <cmath>
#include <vector>

namespace more_phi::test {

namespace {

juce::RangedAudioParameter& requireParameter(MorePhiProcessor& processor, const char* parameterId)
{
    auto* parameter = processor.getAPVTS().getParameter(parameterId);
    INFO("parameterId=" << parameterId);
    REQUIRE(parameter != nullptr);
    return *parameter;
}

void setNormalizedWithGesture(juce::RangedAudioParameter& parameter, float value)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(value);
    parameter.endChangeGesture();
}

juce::AudioBuffer<float> makeStereoSineBuffer(int numSamples = 256)
{
    juce::AudioBuffer<float> buffer(2, numSamples);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float value = 0.125f * std::sin(static_cast<float>(sample) * 0.05f);
        buffer.setSample(0, sample, value);
        buffer.setSample(1, sample, value);
    }

    return buffer;
}

juce::AudioBuffer<float> makeStereoConstantBuffer(float left, float right, int numSamples = 256)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        buffer.setSample(0, sample, left);
        buffer.setSample(1, sample, right);
    }
    return buffer;
}

std::vector<float> copySamples(const juce::AudioBuffer<float>& buffer)
{
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(buffer.getNumChannels() * buffer.getNumSamples()));

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            samples.push_back(buffer.getSample(channel, sample));

    return samples;
}

void processOneBlock(MorePhiProcessor& processor, juce::AudioBuffer<float>& buffer)
{
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

void requireFiniteSamples(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            REQUIRE(std::isfinite(buffer.getSample(channel, sample)));
}

} // namespace

TEST_CASE("VST3 audio signal accuracy keeps processed samples finite", "[integration][vst3][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    auto buffer = makeStereoSineBuffer();
    processOneBlock(processor, buffer);

    requireFiniteSamples(buffer);

    processor.releaseResources();
}

TEST_CASE("VST3 audio signal accuracy preserves silence with no hosted plugin", "[integration][vst3][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    juce::AudioBuffer<float> buffer(2, 256);
    buffer.clear();
    processOneBlock(processor, buffer);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            REQUIRE(buffer.getSample(channel, sample) == Catch::Approx(0.0f).margin(0.0f));

    processor.releaseResources();
}

TEST_CASE("VST3 audio signal accuracy applies output gain at 0 dB and nonzero gains", "[integration][vst3][audio]")
{
    struct Case
    {
        float normalized;
        float expectedDb;
    };

    const Case cases[] = {
        { 0.5f, 0.0f },
        { 0.25f, -12.0f },
        { 0.75f, 12.0f },
    };

    for (const auto testCase : cases)
    {
        CAPTURE(testCase.normalized, testCase.expectedDb);

        MorePhiProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        setNormalizedWithGesture(requireParameter(processor, "outputGain"), testCase.normalized);

        auto buffer = makeStereoConstantBuffer(0.05f, -0.05f);
        processOneBlock(processor, buffer);

        const float expectedGain = juce::Decibels::decibelsToGain(testCase.expectedDb);
        REQUIRE(buffer.getSample(0, 0) == Catch::Approx(0.05f * expectedGain).margin(0.0001f));
        REQUIRE(buffer.getSample(1, 0) == Catch::Approx(-0.05f * expectedGain).margin(0.0001f));

        processor.releaseResources();
    }
}

TEST_CASE("VST3 audio signal accuracy bypass is bit-transparent without hosted plugin", "[integration][vst3][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    setNormalizedWithGesture(requireParameter(processor, "bypass"), 1.0f);
    setNormalizedWithGesture(requireParameter(processor, "outputGain"), 1.0f);

    auto buffer = makeStereoSineBuffer();
    const auto before = copySamples(buffer);
    processOneBlock(processor, buffer);
    const auto after = copySamples(buffer);

    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
        REQUIRE(after[i] == Catch::Approx(before[i]).margin(0.0f));

    processor.releaseResources();
}

TEST_CASE("VST3 audio signal accuracy is deterministic under neutral settings", "[integration][vst3][audio]")
{
    auto processCopy = []()
    {
        MorePhiProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        auto buffer = makeStereoSineBuffer();
        processOneBlock(processor, buffer);
        processor.releaseResources();
        return copySamples(buffer);
    };

    const auto first = processCopy();
    const auto second = processCopy();

    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i)
        REQUIRE(second[i] == Catch::Approx(first[i]).margin(0.0f));
}

TEST_CASE("VST3 audio signal accuracy preserves stereo consistency for identical input", "[integration][vst3][audio]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    auto buffer = makeStereoSineBuffer();
    processOneBlock(processor, buffer);

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        REQUIRE(buffer.getSample(0, sample) == Catch::Approx(buffer.getSample(1, sample)).margin(0.0f));

    processor.releaseResources();
}

} // namespace more_phi::test
