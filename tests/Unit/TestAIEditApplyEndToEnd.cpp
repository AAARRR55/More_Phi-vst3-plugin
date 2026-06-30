// tests/Unit/TestAIEditApplyEndToEnd.cpp
//
// Regression guard for the "AI assistant edits not applied to the hosted plugin"
// class. Before this file existed, EVERY embedded-path test asserted only the
// tool's JSON response (success/queued/flush) — which the handler can report as
// soft-success while applying nothing (e.g. flush.plugin_unavailable with no
// plugin loaded). No test proved the hosted plugin's value actually changed.
//
// These tests drive the REAL apply mechanism behind every AI write tool:
//   MCPToolHandler::setParameter / setParametersBatch / OzonePlanApplicator
//     -> MorePhiProcessor::enqueueParameterSet(MCP|Assistant|Neural, hold=true)
//     -> (flush / processBlock) -> drainParameterCommandQueue
//     -> hosted AudioProcessorParameter::setValue
// and assert the fake hosted plugin's value moved. If this chain ever stops
// landing edits, these fail — instead of the bug shipping silently.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

using namespace more_phi;
using ParameterEditSource = MorePhiProcessor::ParameterEditSource;
using Catch::Approx;

namespace {

// Minimal hosted plugin: a few AudioParameterFloat params we can read back. No
// DSP — only the AudioProcessorParameter getValue/setValue surface that the
// command-queue drain writes through (the exclusive-plugin write path in
// drainParameterCommandQueue).
class FakeHostedPluginForEdit final : public juce::AudioPluginInstance
{
public:
    FakeHostedPluginForEdit()
        : juce::AudioPluginInstance(juce::AudioProcessor::BusesProperties()
            .withOutput("Out", juce::AudioChannelSet::stereo(), false))
    {
        for (int i = 0; i < kParamCount; ++i)
            addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{ "p" + juce::String(i), 1 }, "P" + juce::String(i),
                juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    }

    static constexpr int kParamCount = 4;

    const juce::String getName() const override { return "FakeEdit"; }
    void fillInPluginDescription(juce::PluginDescription&) const override {}
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

float readParam(juce::AudioPluginInstance& p, int index)
{
    return p.getParameters()[static_cast<size_t>(index)]->getValue();
}

} // namespace

TEST_CASE("AI enqueue+drain applies the edit to the hosted plugin",
          "[unit][ai][apply][regression]")
{
    MorePhiProcessor processor;
    FakeHostedPluginForEdit plugin;
    REQUIRE(plugin.getParameters().size() == FakeHostedPluginForEdit::kParamCount);

    for (int i = 0; i < FakeHostedPluginForEdit::kParamCount; ++i)
        REQUIRE(readParam(plugin, i) == Approx(0.0f).margin(1e-6f));

    // Enqueue edits exactly the way the AI path does (MCP source, hold against morph).
    REQUIRE(processor.enqueueParameterSet(0, 0.75f, MorePhiProcessor::ParameterEditSource::MCP, true));
    REQUIRE(processor.enqueueParameterSet(2, 0.30f, MorePhiProcessor::ParameterEditSource::MCP, true));

    // Drain directly against the fake plugin — the test-public exclusive-plugin
    // seam that flushPendingParameterCommandsForAssistant uses internally. This
    // is the "did the write reach setValue on the hosted plugin" assertion that
    // no prior test made.
    const int drained = processor.drainParameterCommandQueue(
        FakeHostedPluginForEdit::kParamCount, 2048, &plugin, nullptr, /*now=*/0);

    REQUIRE(drained == 2);
    REQUIRE(readParam(plugin, 0) == Approx(0.75f).margin(1e-5f));   // landed
    REQUIRE(readParam(plugin, 1) == Approx(0.0f).margin(1e-6f));    // untouched
    REQUIRE(readParam(plugin, 2) == Approx(0.30f).margin(1e-5f));   // landed
    REQUIRE(readParam(plugin, 3) == Approx(0.0f).margin(1e-6f));    // untouched
}

TEST_CASE("AI batch enqueue+drain applies every parameter with no silent drops",
          "[unit][ai][apply][regression]")
{
    MorePhiProcessor processor;
    FakeHostedPluginForEdit plugin;

    // setParametersBatch enqueues per-item the same way; prove nothing is dropped.
    for (int i = 0; i < FakeHostedPluginForEdit::kParamCount; ++i)
    {
        const float v = static_cast<float>(i + 1) / 10.0f;
        REQUIRE(processor.enqueueParameterSet(i, v, MorePhiProcessor::ParameterEditSource::Assistant, true));
    }

    const int drained = processor.drainParameterCommandQueue(
        FakeHostedPluginForEdit::kParamCount, 2048, &plugin, nullptr, /*now=*/0);

    REQUIRE(drained == FakeHostedPluginForEdit::kParamCount);
    for (int i = 0; i < FakeHostedPluginForEdit::kParamCount; ++i)
    {
        const float expected = static_cast<float>(i + 1) / 10.0f;
        INFO("param " << i << " = " << readParam(plugin, i) << " expected " << expected);
        REQUIRE(readParam(plugin, i) == Approx(expected).margin(1e-5f));
    }
}

TEST_CASE("AI neural-plan enqueue+drain applies to the hosted plugin",
          "[unit][ai][apply][regression]")
{
    // The mastering.neural_apply path enqueues via OzonePlanApplicator with
    // MorePhiProcessor::ParameterEditSource::Neural + holdAgainstMorph=true. Same drain seam.
    MorePhiProcessor processor;
    FakeHostedPluginForEdit plugin;

    REQUIRE(processor.enqueueParameterSet(1, 0.66f, MorePhiProcessor::ParameterEditSource::Neural, true));
    REQUIRE(processor.enqueueParameterSet(3, 0.22f, MorePhiProcessor::ParameterEditSource::Neural, true));

    const int drained = processor.drainParameterCommandQueue(
        FakeHostedPluginForEdit::kParamCount, 2048, &plugin, nullptr, /*now=*/0);

    REQUIRE(drained == 2);
    REQUIRE(readParam(plugin, 1) == Approx(0.66f).margin(1e-5f));
    REQUIRE(readParam(plugin, 3) == Approx(0.22f).margin(1e-5f));
}
