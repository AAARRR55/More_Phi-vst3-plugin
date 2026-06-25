// tests/Unit/TestRecallRamp.cpp
//
// C-5 FIX (audit): pins the snapshot-recall ramp behavior. Previously
// SnapshotBank::recallFast → ParameterBridge::applyParameterState snapped
// every hosted parameter to its target in one block — an audible click on
// continuous params (gain, cutoff). The fix routes recall through
// ParameterBridge::startRecallRamp + processRecallRamp, which linearly ramp
// from the plugin's CURRENT values to the target over kRecallRampBlocks.
//
// This test builds a minimal fake hosted plugin + host manager (no real VST3)
// and asserts:
//   1. startRecallRamp captures the plugin's current values as the ramp start.
//   2. A single processRecallRamp block does NOT snap to the target — the
//      output is strictly partway (the click-free invariant).
//   3. After kRecallRampBlocks blocks, the plugin holds exactly the target.
//   4. isRecallRampActive() is true during the ramp and false after.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Host/ParameterBridge.h"
#include "Host/IPluginHostManager.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <memory>

using namespace more_phi;
using Catch::Approx;

namespace {

// Minimal hosted plugin with a few continuous parameters whose getValue() we
// can read back to verify the ramp. We do NOT need real DSP — only the
// AudioProcessorParameter getValue/setValue surface.
class FakeRampPlugin final : public juce::AudioPluginInstance
{
public:
    FakeRampPlugin()
        : juce::AudioPluginInstance(juce::AudioProcessor::BusesProperties()
            .withOutput("Out", juce::AudioChannelSet::stereo(), false))
    {
        // 4 continuous params, all defaulting to 0.0 (the ramp "start").
        for (int i = 0; i < 4; ++i)
            addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{ "p" + juce::String(i), 1 }, "P" + juce::String(i),
                juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    }

    const juce::String getName() const override { return "FakeRamp"; }
    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = getName(); }
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

// Minimal IPluginHostManager — getPlugin() returns the raw pointer (the
// withPlugin fallback path used when cachedConcreteHost_ is null). Sufficient
// for single-threaded unit testing of ParameterBridge.
class FakeHostManager final : public IPluginHostManager
{
public:
    explicit FakeHostManager(std::unique_ptr<juce::AudioPluginInstance> plugin)
        : plugin_(std::move(plugin)) {}

    void prepare(double, int, int) override {}
    void releaseResources() override {}
    bool loadPlugin(const juce::PluginDescription&) override { return false; }
    void unloadPlugin() override {}
    bool hasPlugin() const override { return plugin_ != nullptr; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept override {}
    juce::AudioPluginInstance* getPlugin() override { return plugin_.get(); }
    const juce::AudioPluginInstance* getPlugin() const override { return plugin_.get(); }
    const juce::PluginDescription* getLastDescription() const override { return nullptr; }
    juce::AudioPluginFormatManager& getFormatManager() override { return fm_; }
    juce::KnownPluginList& getKnownPlugins() override { return list_; }
    void scanPluginFolders() override {}

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    juce::AudioPluginFormatManager fm_;
    juce::KnownPluginList list_;
};

float readParam(juce::AudioPluginInstance& p, int index)
{
    return p.getParameters()[static_cast<size_t>(index)]->getValue();
}

} // namespace

TEST_CASE("ParameterBridge recall ramp does not snap and converges to target",
          "[recall][c5]")
{
    auto plugin = std::make_unique<FakeRampPlugin>();
    FakeHostManager host(std::move(plugin));
    ParameterBridge bridge(host);

    auto* pluginPtr = host.getPlugin();
    REQUIRE(pluginPtr != nullptr);
    REQUIRE(pluginPtr->getParameters().size() == 4);

    // Sanity: all params start at 0.0 (the default).
    for (int i = 0; i < 4; ++i)
        REQUIRE(readParam(*pluginPtr, i) == Approx(0.0f).margin(1e-6f));

    // Target = all params to 1.0 (maximal discontinuity from the 0.0 start).
    const float target[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Start the ramp — should capture current (0.0) as the start.
    REQUIRE(bridge.startRecallRamp(target, 4));
    REQUIRE(bridge.isRecallRampActive());

    // Invariant 1: a single block must NOT snap to target. After 1 of N blocks,
    // the value is partway (frac = 1/kRecallRampBlocks). For the default
    // kRecallRampBlocks=8, that's ~0.125 — comfortably between 0 and 1.
    bridge.processRecallRamp();
    REQUIRE(bridge.isRecallRampActive());   // still ramping
    for (int i = 0; i < 4; ++i)
    {
        const float v = readParam(*pluginPtr, i);
        INFO("after 1 ramp block, param " << i << " = " << v);
        REQUIRE(v > 0.0f);        // it moved
        REQUIRE(v < 0.99f);       // but NOT snapped to target
    }

    // Invariant 2: run the remaining (kRecallRampBlocks - 1) blocks. The ramp
    // must complete and land exactly on target.
    const int kBlocks = ParameterBridge::kRecallRampBlocks;
    for (int b = 1; b < kBlocks; ++b)
        bridge.processRecallRamp();

    REQUIRE_FALSE(bridge.isRecallRampActive());
    for (int i = 0; i < 4; ++i)
        REQUIRE(readParam(*pluginPtr, i) == Approx(1.0f).margin(1e-5f));

    // Invariant 3: after completion, processRecallRamp is a harmless no-op.
    bridge.processRecallRamp();
    REQUIRE_FALSE(bridge.isRecallRampActive());
    for (int i = 0; i < 4; ++i)
        REQUIRE(readParam(*pluginPtr, i) == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("ParameterBridge recall ramp captures current values as the start",
          "[recall][c5]")
{
    // If the plugin is already at 0.3 everywhere and we ramp to 0.7, the FIRST
    // block must move FROM 0.3 (not from 0.0) — proving current values were
    // captured, not assumed zero.
    auto plugin = std::make_unique<FakeRampPlugin>();
    FakeHostManager host(std::move(plugin));
    ParameterBridge bridge(host);
    auto* pluginPtr = host.getPlugin();
    REQUIRE(pluginPtr != nullptr);

    for (int i = 0; i < 4; ++i)
        pluginPtr->getParameters()[static_cast<size_t>(i)]->setValue(0.3f);

    const float target[4] = { 0.7f, 0.7f, 0.7f, 0.7f };
    REQUIRE(bridge.startRecallRamp(target, 4));

    bridge.processRecallRamp();   // 1 block: frac = 1/kRecallRampBlocks
    const int kBlocks = ParameterBridge::kRecallRampBlocks;
    const float expectedAfter1 = 0.3f + (0.7f - 0.3f) * (1.0f / static_cast<float>(kBlocks));
    for (int i = 0; i < 4; ++i)
    {
        INFO("param " << i << " after 1 block: " << readParam(*pluginPtr, i)
                      << " expected ~" << expectedAfter1);
        REQUIRE(readParam(*pluginPtr, i) == Approx(expectedAfter1).margin(0.02f));
    }
}

TEST_CASE("ParameterBridge recall ramp rejects bad input",
          "[recall][c5]")
{
    auto plugin = std::make_unique<FakeRampPlugin>();
    FakeHostManager host(std::move(plugin));
    ParameterBridge bridge(host);

    const float target[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    REQUIRE_FALSE(bridge.startRecallRamp(nullptr, 4));
    REQUIRE_FALSE(bridge.startRecallRamp(target, 0));
    REQUIRE_FALSE(bridge.isRecallRampActive());
}
