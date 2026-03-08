#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/IPluginHostManager.h"

#include <cmath>
#include <random>

using Catch::Approx;
using namespace morphsnap;

namespace {

class FakeFabFilterPlugin final : public juce::AudioPluginInstance
{
public:
    FakeFabFilterPlugin()
        : juce::AudioPluginInstance(juce::AudioProcessor::BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        addHostedParameter(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"band1_used", 1}, "Band 1 Used", false));
        addHostedParameter(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"band1_enabled", 1}, "Band 1 Enabled", true));
        addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"band1_frequency", 1}, "Band 1 Frequency",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
        addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"band1_gain", 1}, "Band 1 Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
        addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"band1_q", 1}, "Band 1 Q",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
        addHostedParameter(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"band1_shape", 1}, "Band 1 Shape",
            juce::StringArray{"Bell", "Shelf", "Notch"}, 0));
        addHostedParameter(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"band1_solo", 1}, "Band 1 Solo", false));
        addHostedParameter(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"global_bypass", 1}, "Global Bypass", false));
        addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"output_gain", 1}, "Output Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    }

    const juce::String getName() const override { return "FabFilter Pro-Q 4"; }
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.descriptiveName = getName();
        description.pluginFormatName = "VST3";
        description.manufacturerName = "FabFilter";
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

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

class FakePluginHostManager final : public IPluginHostManager
{
public:
    explicit FakePluginHostManager(std::unique_ptr<juce::AudioPluginInstance> plugin)
        : plugin_(std::move(plugin))
    {
        if (plugin_ != nullptr)
            plugin_->fillInPluginDescription(description_);
    }

    void prepare(double sampleRate, int blockSize, int numChannels) override
    {
        if (plugin_ == nullptr)
            return;

        plugin_->setRateAndBufferSizeDetails(sampleRate, blockSize);
        plugin_->setPlayConfigDetails(numChannels, numChannels, sampleRate, blockSize);
        plugin_->prepareToPlay(sampleRate, blockSize);
    }

    void releaseResources() override
    {
        if (plugin_ != nullptr)
            plugin_->releaseResources();
    }

    bool loadPlugin(const juce::PluginDescription&) override { return false; }
    void unloadPlugin() override {}
    bool hasPlugin() const override { return plugin_ != nullptr; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        if (plugin_ != nullptr)
            plugin_->processBlock(buffer, midi);
    }

    juce::AudioPluginInstance* getPlugin() override { return plugin_.get(); }
    const juce::AudioPluginInstance* getPlugin() const override { return plugin_.get(); }
    const juce::PluginDescription* getLastDescription() const override { return &description_; }

    juce::AudioPluginFormatManager& getFormatManager() override { return formatManager_; }
    juce::KnownPluginList& getKnownPlugins() override { return knownPlugins_; }
    void scanPluginFolders() override {}

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    juce::PluginDescription description_;
    juce::AudioPluginFormatManager formatManager_;
    juce::KnownPluginList knownPlugins_;
};

juce::File createBroadbandInputFile()
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morphsnap_offline_guardrail_input", ".wav");

    juce::AudioBuffer<float> buffer(2, 4096);
    std::mt19937 generator(1337);
    std::uniform_real_distribution<float> distribution(-0.2f, 0.2f);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* writePtr = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            writePtr[sample] = distribution(generator);
    }

    juce::WavAudioFormat wavFormat;
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (stream == nullptr)
        return {};

    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wavFormat.createWriterFor(stream.release(), 48000.0, 2, 24, {}, 0));
    if (writer == nullptr)
        return {};

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
        return {};

    writer.reset();
    return file;
}

} // namespace

TEST_CASE("OfflineBatchRenderer keeps FabFilter trap params out of randomized renders", "[dataset][offline][guardrails]")
{
    const auto inputFile = createBroadbandInputFile();
    REQUIRE(inputFile.existsAsFile());

    const auto outputDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morphsnap_offline_guardrail_output", "");
    REQUIRE(outputDirectory.createDirectory());

    auto plugin = std::make_unique<FakeFabFilterPlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager hostManager(std::move(plugin));

    OfflineBatchConfig config;
    config.inputFile = inputFile;
    config.outputDirectory = outputDirectory;
    config.totalVariations = 4;
    config.parallelWorkers = 1;
    config.renderConfig.sampleRate = 48000.0;
    config.renderConfig.blockSize = 256;
    config.renderConfig.numChannels = 2;
    config.renderConfig.outputDirectory = outputDirectory;
    config.renderConfig.validateOutput = true;

    OfflineBatchRenderer renderer;
    REQUIRE(renderer.setConfig(config));

    bool completed = false;
    bool succeeded = false;
    renderer.onRenderComplete = [&](bool success, const juce::String&)
    {
        completed = true;
        succeeded = success;
    };

    REQUIRE(renderer.startRender(&hostManager));
    while (renderer.isRendering())
        juce::Thread::sleep(10);

    renderer.stopRender();

    REQUIRE(completed);
    REQUIRE(succeeded);
    REQUIRE(outputDirectory.getChildFile("variation_0000.wav").existsAsFile());
    REQUIRE(outputDirectory.getChildFile("variation_0003.wav").existsAsFile());

    const auto& parameters = pluginPtr->getParameters();
    REQUIRE(parameters[0]->getValue() == Approx(1.0f)); // Band 1 Used forced on when mutating the band
    REQUIRE(parameters[1]->getValue() == Approx(1.0f)); // Band 1 Enabled remains on
    REQUIRE(parameters[6]->getValue() == Approx(0.0f)); // Solo untouched
    REQUIRE(parameters[7]->getValue() == Approx(0.0f)); // Global bypass untouched
    REQUIRE(parameters[8]->getValue() == Approx(0.5f)); // Output gain untouched

    const bool safeParamChanged =
        std::abs(parameters[2]->getValue() - 0.5f) > 0.001f
        || std::abs(parameters[3]->getValue() - 0.5f) > 0.001f
        || std::abs(parameters[4]->getValue() - 0.5f) > 0.001f
        || std::abs(parameters[5]->getValue() - 0.0f) > 0.001f;
    REQUIRE(safeParamChanged);

    outputDirectory.deleteRecursively();
    inputFile.deleteFile();
}
