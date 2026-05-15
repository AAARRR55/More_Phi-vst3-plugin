#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nlohmann/json.hpp>

#include "AI/StandaloneMcp/OzonePluginBackend.h"
#include "AI/StandaloneMcp/StandaloneMcpServer.h"
#include "Host/IPluginHostManager.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstring>
#include <memory>
#include <sstream>

using Catch::Approx;
using json = nlohmann::json;

namespace {

class FakeOzonePlugin final : public juce::AudioPluginInstance
{
public:
    explicit FakeOzonePlugin(bool includeAssistant = true)
        : juce::AudioPluginInstance(juce::AudioProcessor::BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"eq_gain", 1}, "EQ Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));

        if (includeAssistant)
        {
            addHostedParameter(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{"assistant_analyze", 1}, "Master Assistant Analyze",
                juce::NormalisableRange<float>(0.0f, 1.0f), 0.25f));
        }

        addHostedParameter(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"maximizer_enabled", 1}, "Maximizer Enabled", true));
    }

    const juce::String getName() const override { return "Ozone 11"; }

    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.descriptiveName = getName();
        description.pluginFormatName = "VST3";
        description.manufacturerName = "iZotope";
        description.fileOrIdentifier = "Fake/Ozone 11.vst3";
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        ++processBlockCalls;
        buffer.applyGain(0.75f);
    }

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

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.setSize(state.size());
        if (!state.empty())
            std::memcpy(destData.getData(), state.data(), state.size());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        state.assign(static_cast<const char*>(data), static_cast<size_t>(sizeInBytes));
    }

    int processBlockCalls = 0;
    std::string state = "initial-state";
};

class FakePluginHostManager final : public more_phi::IPluginHostManager
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

json request(int id, const std::string& method, json params = json::object())
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", std::move(params)}
    };
}

more_phi::standalone_mcp::StandaloneMcpServer makeServer(FakePluginHostManager& host)
{
    return more_phi::standalone_mcp::StandaloneMcpServer(
        std::make_unique<more_phi::standalone_mcp::HostedOzonePluginBackend>(host));
}

juce::File createAssistantInputFile()
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_standalone_mcp_assistant_input", ".wav");

    juce::AudioBuffer<float> buffer(2, 2048);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            samples[i] = (i % 64) < 32 ? 0.2f : -0.2f;
    }

    juce::WavAudioFormat wavFormat;
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    REQUIRE(stream != nullptr);

    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wavFormat.createWriterFor(stream.release(), 44100.0, 2, 24, {}, 0));
    REQUIRE(writer != nullptr);
    REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()));
    writer.reset();

    return file;
}

} // namespace

TEST_CASE("Standalone Ozone MCP server lists hosted-plugin tools", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto server = makeServer(host);

    const auto init = server.processJson(request(1, "initialize"));
    REQUIRE(init["result"]["protocolVersion"].get<std::string>() == "2025-06-18");
    REQUIRE(init["result"]["serverInfo"]["name"].get<std::string>() == "morephi-ozone-plugin-mcp");

    const auto listed = server.processJson(request(2, "tools/list"));
    REQUIRE(listed["result"]["tools"].is_array());

    bool foundGetParameters = false;
    bool foundSetParameter = false;
    bool foundAssistant = false;
    bool foundGetState = false;
    bool foundSetState = false;

    for (const auto& tool : listed["result"]["tools"])
    {
        REQUIRE(tool.contains("annotations"));
        const auto name = tool["name"].get<std::string>();
        foundGetParameters = foundGetParameters || name == "ozone_get_parameters";
        foundSetParameter = foundSetParameter || name == "ozone_set_parameter";
        foundAssistant = foundAssistant || name == "ozone_run_master_assistant";
        foundGetState = foundGetState || name == "ozone_get_state";
        foundSetState = foundSetState || name == "ozone_set_state";
    }

    REQUIRE(foundGetParameters);
    REQUIRE(foundSetParameter);
    REQUIRE(foundAssistant);
    REQUIRE(foundGetState);
    REQUIRE(foundSetState);
}

TEST_CASE("Standalone Ozone MCP server reads and writes hosted parameters", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    const auto listed = server.processJson(request(3, "tools/call", {
        {"name", "ozone_get_parameters"},
        {"arguments", {{"include_values", true}}}
    }));
    REQUIRE_FALSE(listed["result"]["isError"].get<bool>());
    REQUIRE(listed["result"]["structuredContent"]["count"].get<int>() == 3);
    REQUIRE(listed["result"]["structuredContent"]["parameters"][0]["name"].get<std::string>() == "EQ Gain");

    const auto filtered = server.processJson(request(4, "tools/call", {
        {"name", "ozone_get_parameters"},
        {"arguments", {{"query", "assistant"}, {"include_values", false}}}
    }));
    REQUIRE_FALSE(filtered["result"]["isError"].get<bool>());
    REQUIRE(filtered["result"]["structuredContent"]["returned"].get<int>() == 1);
    REQUIRE_FALSE(filtered["result"]["structuredContent"]["parameters"][0].contains("value"));

    const auto set = server.processJson(request(5, "tools/call", {
        {"name", "ozone_set_parameter"},
        {"arguments", {{"index", 0}, {"value", 0.75}}}
    }));
    REQUIRE_FALSE(set["result"]["isError"].get<bool>());
    REQUIRE(set["result"]["structuredContent"]["parameter"]["value"].get<float>() == Approx(0.75f));
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(0.75f));
}

TEST_CASE("Standalone Ozone MCP server captures and restores plugin state", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    const auto captured = server.processJson(request(6, "tools/call", {
        {"name", "ozone_get_state"},
        {"arguments", json::object()}
    }));
    REQUIRE_FALSE(captured["result"]["isError"].get<bool>());
    REQUIRE(captured["result"]["structuredContent"]["size_bytes"].get<int>() > 0);
    REQUIRE(captured["result"]["structuredContent"]["state_base64"].is_string());

    const std::string restoredState = "restored-state";
    const auto restoredBase64 = juce::Base64::toBase64(
        restoredState.data(), restoredState.size()).toStdString();
    const auto restored = server.processJson(request(7, "tools/call", {
        {"name", "ozone_set_state"},
        {"arguments", {{"state_base64", restoredBase64}}}
    }));
    REQUIRE_FALSE(restored["result"]["isError"].get<bool>());
    REQUIRE(pluginPtr->state == "restored-state");
}

TEST_CASE("Standalone Ozone MCP server triggers assistant parameter and renders input audio", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    const auto inputFile = createAssistantInputFile();

    const auto assistant = server.processJson(request(8, "tools/call", {
        {"name", "ozone_run_master_assistant"},
        {"arguments", {
            {"input_audio_path", inputFile.getFullPathName().toStdString()},
            {"analysis_seconds", 0.05}
        }}
    }));

    REQUIRE_FALSE(assistant["result"]["isError"].get<bool>());
    REQUIRE(assistant["result"]["structuredContent"]["assistant_parameter_index"].get<int>() == 1);
    REQUIRE(assistant["result"]["structuredContent"]["rendered_samples"].get<int>() > 0);
    REQUIRE(pluginPtr->processBlockCalls > 0);
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(0.0f));

    inputFile.deleteFile();
}

TEST_CASE("Standalone Ozone MCP server reports assistant discovery failure as a tool error", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>(false);
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    const auto assistant = server.processJson(request(9, "tools/call", {
        {"name", "ozone_run_master_assistant"},
        {"arguments", json::object()}
    }));

    REQUIRE(assistant["result"]["isError"].get<bool>());
    REQUIRE(assistant["result"]["structuredContent"]["error"].get<std::string>() == "assistant_parameter_not_found");
}

TEST_CASE("Standalone Ozone MCP server uses protocol errors for malformed tool params", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    const auto badSet = server.processJson(request(10, "tools/call", {
        {"name", "ozone_set_parameter"},
        {"arguments", {{"index", 0}, {"value", 1.5}}}
    }));

    REQUIRE(badSet.contains("error"));
    REQUIRE(badSet["error"]["code"].get<int>() == -32602);

    const auto badState = server.processJson(request(11, "tools/call", {
        {"name", "ozone_set_state"},
        {"arguments", json::object()}
    }));

    REQUIRE(badState.contains("error"));
    REQUIRE(badState["error"]["code"].get<int>() == -32602);
}

TEST_CASE("Standalone Ozone MCP stdio run handles newline-delimited JSON", "[mcp][standalone]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);
    auto server = makeServer(host);

    std::stringstream input;
    std::stringstream output;
    input << request(1, "initialize").dump() << "\n";
    input << request(2, "tools/list").dump() << "\n";

    server.run(input, output);

    std::string line;
    std::getline(output, line);
    REQUIRE(json::parse(line)["result"]["serverInfo"]["name"].get<std::string>() == "morephi-ozone-plugin-mcp");

    std::getline(output, line);
    REQUIRE(json::parse(line)["result"]["tools"].is_array());
}
