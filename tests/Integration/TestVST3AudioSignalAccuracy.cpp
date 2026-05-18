#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Core/NeuralMasteringSafetyPolicy.h"
#include "Plugin/PluginProcessor.h"

#include <algorithm>
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

TEST_CASE("VST3 audio signal accuracy preserves invalid neural plan state", "[integration][vst3][audio][NeuralMasteringController]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    ValidatedNeuralMasteringPlan validPlan;
    validPlan.valid = true;
    validPlan.sourcePlanId = 90;
    validPlan.appliedMask.eq = true;
    validPlan.projectedTargets.eq[0] = 0.1f;
    REQUIRE(processor.getAutoMasteringEngine().applyValidatedPlan(validPlan));

    ValidatedNeuralMasteringPlan invalidPlan;
    invalidPlan.valid = false;
    invalidPlan.sourcePlanId = 91;
    invalidPlan.fallbackMode = NeuralMasteringFallbackMode::Reject;
    CHECK_FALSE(processor.getAutoMasteringEngine().applyValidatedPlan(invalidPlan));

    CHECK(processor.hasLastSafeNeuralMasteringPlan());
    CHECK(processor.getLastSafeNeuralMasteringPlan().sourcePlanId == 90);

    auto buffer = makeStereoSineBuffer();
    processOneBlock(processor, buffer);
    requireFiniteSamples(buffer);

    processor.releaseResources();
}

TEST_CASE("VST3 neural mastering plan transitions keep signal bounded and mono-compatible", "[integration][vst3][audio][NeuralMasteringController]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    auto& engine = processor.getAutoMasteringEngine();
    engine.setActive(true);

    ValidatedNeuralMasteringPlan initialPlan;
    initialPlan.valid = true;
    initialPlan.sourcePlanId = 100;
    initialPlan.appliedMask.eq = true;
    initialPlan.appliedMask.dynamics = true;
    initialPlan.appliedMask.stereo = true;
    initialPlan.appliedMask.limiter = true;
    initialPlan.appliedMask.loudness = true;
    initialPlan.projectedTargets.eq[0] = 0.05f;
    initialPlan.projectedTargets.dynamics[0] = -0.05f;
    initialPlan.projectedTargets.stereo[0] = 0.02f;
    initialPlan.projectedTargets.limiter[0] = -0.10f;
    initialPlan.projectedTargets.loudness[0] = 0.0f;
    REQUIRE(engine.applyValidatedPlan(initialPlan));

    auto before = makeStereoConstantBuffer(0.08f, 0.08f);
    processOneBlock(processor, before);
    requireFiniteSamples(before);

    ValidatedNeuralMasteringPlan transitionedPlan = initialPlan;
    transitionedPlan.sourcePlanId = 101;
    transitionedPlan.projectedTargets.eq[0] = 0.10f;
    transitionedPlan.projectedTargets.dynamics[0] = 0.02f;
    transitionedPlan.projectedTargets.stereo[0] = 0.04f;
    transitionedPlan.projectedTargets.limiter[0] = -0.20f;
    transitionedPlan.projectedTargets.loudness[0] = -0.05f;
    REQUIRE(engine.applyValidatedPlan(transitionedPlan));

    auto after = makeStereoConstantBuffer(0.08f, 0.08f);
    processOneBlock(processor, after);
    requireFiniteSamples(after);

    float maxAbs = 0.0f;
    float maxDiscontinuity = 0.0f;
    for (int sample = 0; sample < after.getNumSamples(); ++sample)
    {
        const float left = after.getSample(0, sample);
        const float right = after.getSample(1, sample);
        maxAbs = std::max(maxAbs, std::max(std::abs(left), std::abs(right)));
        maxDiscontinuity = std::max(maxDiscontinuity, std::abs(left - before.getSample(0, sample)));
        CHECK(left == Catch::Approx(right).margin(0.0001f));
    }

    CHECK(maxAbs <= 1.0f);
    CHECK(maxDiscontinuity < 0.25f);
    CHECK(engine.getTruePeak_dBTP() <= 0.0f);

    setNormalizedWithGesture(requireParameter(processor, "bypass"), 1.0f);
    auto bypassed = makeStereoSineBuffer();
    const auto bypassBefore = copySamples(bypassed);
    processOneBlock(processor, bypassed);
    const auto bypassAfter = copySamples(bypassed);
    REQUIRE(bypassAfter.size() == bypassBefore.size());
    for (size_t i = 0; i < bypassBefore.size(); ++i)
        CHECK(bypassAfter[i] == Catch::Approx(bypassBefore[i]).margin(0.0f));

    processor.releaseResources();
}

} // namespace more_phi::test
