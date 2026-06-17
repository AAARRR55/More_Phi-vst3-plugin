#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nlohmann/json.hpp>

#include "AI/StandaloneMcp/IZotopeIPCAssistant.h"
#include "AI/StandaloneMcp/IZotopeIPCDiscovery.h"
#include "AI/StandaloneMcp/OzonePluginBackend.h"
#include "AI/StandaloneMcp/StandaloneMcpServer.h"
#include "Host/IPluginHostManager.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

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

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept override
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

more_phi::standalone_mcp::StandaloneMcpServer makeServer(
    FakePluginHostManager& host,
    std::unique_ptr<more_phi::standalone_mcp::IZotopeIPCDiscovery> discovery)
{
    return more_phi::standalone_mcp::StandaloneMcpServer(
        std::make_unique<more_phi::standalone_mcp::HostedOzonePluginBackend>(host),
        std::move(discovery));
}

more_phi::standalone_mcp::StandaloneMcpServer makeServer(
    FakePluginHostManager& host,
    std::unique_ptr<more_phi::standalone_mcp::IZotopeIPCDiscovery> discovery,
    std::unique_ptr<more_phi::standalone_mcp::IZotopeIPCAssistant> assistant)
{
    return more_phi::standalone_mcp::StandaloneMcpServer(
        std::make_unique<more_phi::standalone_mcp::HostedOzonePluginBackend>(host),
        std::move(discovery),
        std::move(assistant));
}

void writeU16LE(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value & 0xffu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeU32LE(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes[offset + static_cast<size_t>(i)] = static_cast<uint8_t>((value >> (8u * i)) & 0xffu);
}

void writeU64LE(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        bytes[offset + static_cast<size_t>(i)] = static_cast<uint8_t>((value >> (8u * i)) & 0xffu);
}

void writeF32LE(std::vector<uint8_t>& bytes, size_t offset, float value)
{
    static_assert(sizeof(float) == 4);
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    writeU32LE(bytes, offset, raw);
}

std::vector<uint8_t> createFakeIpcMemory()
{
    std::vector<uint8_t> bytes(256, 0);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i & 0xffu);

    constexpr size_t frameOffset = 32;
    writeU32LE(bytes, frameOffset + 0, 0x495A4F54u);
    writeU16LE(bytes, frameOffset + 4, 3);
    writeU16LE(bytes, frameOffset + 6, 0x0021);
    writeU32LE(bytes, frameOffset + 8, 0x12345678u);
    writeU32LE(bytes, frameOffset + 12, 0xffffffffu);
    writeU32LE(bytes, frameOffset + 16, 6);
    writeU64LE(bytes, frameOffset + 20, 123456789ull);

    bytes[frameOffset + 28] = 2;
    bytes[frameOffset + 29] = 0;
    bytes[frameOffset + 30] = 7;
    bytes[frameOffset + 31] = 0;
    bytes[frameOffset + 32] = 0xcd;
    bytes[frameOffset + 33] = 0xcc;
    return bytes;
}

std::string base64Slice(const std::vector<uint8_t>& bytes, size_t offset, size_t size)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(size <= bytes.size() - offset);
    return juce::Base64::toBase64(static_cast<const void*>(bytes.data() + offset), size).toStdString();
}

std::vector<uint8_t> createFakeAssistantMemory()
{
    constexpr uint32_t ozoneId = 0x01020304u;
    constexpr uint32_t observerId = 0xDEADBEEFu;
    constexpr size_t registryOffset = 64;
    constexpr size_t ringWriteOffset = 260;
    constexpr size_t ringDataOffset = 512;
    constexpr size_t resultRingOffset = 128;
    constexpr size_t frameHeaderSize = 28;

    std::vector<uint8_t> bytes(2048, 0);
    writeU32LE(bytes, registryOffset + 0, ozoneId);
    const std::string name = "Ozone 11";
    std::memcpy(bytes.data() + registryOffset + 8, name.data(), name.size());
    writeU32LE(bytes, registryOffset + 36, 1);

    writeU32LE(bytes, ringWriteOffset, 170u);

    const size_t frameOffset = ringDataOffset + resultRingOffset;
    writeU32LE(bytes, frameOffset + 0, 0x495A4F54u);
    writeU16LE(bytes, frameOffset + 4, 3);
    writeU16LE(bytes, frameOffset + 6, 0x0021);
    writeU32LE(bytes, frameOffset + 8, ozoneId);
    writeU32LE(bytes, frameOffset + 12, observerId);
    writeU32LE(bytes, frameOffset + 16, 14);
    writeU64LE(bytes, frameOffset + 20, 1234);

    const size_t payloadOffset = frameOffset + frameHeaderSize;
    writeU16LE(bytes, payloadOffset + 0, 2);
    writeU16LE(bytes, payloadOffset + 2, 0);
    writeF32LE(bytes, payloadOffset + 4, 0.62f);
    writeU16LE(bytes, payloadOffset + 8, 2);
    writeF32LE(bytes, payloadOffset + 10, 0.91f);
    return bytes;
}

juce::File createAssistantManifestFile(bool /*unused*/ = false, size_t maxEntries = 4)
{
    json schema = {
        {"readPtrOff",        256},
        {"writePtrOff",       260},
        {"pluginRegOff",      64},
        {"ringOff",           512},
        {"ringSize",          512},
        {"entrySize",         64},
        {"maxEntries",        maxEntries},
        {"frameHdrSize",      28},
        {"mapped_size_bytes", 2048}
    };

    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_ipc_schema", ".json");
    REQUIRE(file.replaceWithText(juce::String(schema.dump())));
    return file;
}

uint16_t readU16LEFromVector(const std::vector<uint8_t>& bytes, size_t offset)
{
    REQUIRE(offset + 2 <= bytes.size());
    return static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8u);
}

uint32_t readU32LEFromVector(const std::vector<uint8_t>& bytes, size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8u)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16u)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

uint32_t readU32LEUnchecked(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8u)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16u)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

std::vector<uint8_t> makeAssistantResultPayload(const std::vector<std::pair<int, float>>& params)
{
    std::vector<uint8_t> payload(2 + params.size() * 6, 0);
    writeU16LE(payload, 0, static_cast<uint16_t>(params.size()));
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto entryOffset = 2 + i * 6;
        writeU16LE(payload, entryOffset, static_cast<uint16_t>(params[i].first));
        writeF32LE(payload, entryOffset + 2, params[i].second);
    }
    return payload;
}

void appendBytesAtCurrentRingWrite(std::vector<uint8_t>& bytes,
                                   const std::vector<uint8_t>& appended,
                                   size_t ringWriteOffset = 260,
                                   size_t ringDataOffset = 512,
                                   size_t ringSize = 512)
{
    const auto writePos = readU32LEUnchecked(bytes, ringWriteOffset) % static_cast<uint32_t>(ringSize);
    for (size_t i = 0; i < appended.size(); ++i)
        bytes[ringDataOffset + ((writePos + i) % ringSize)] = appended[i];

    writeU32LE(bytes, ringWriteOffset, static_cast<uint32_t>((writePos + appended.size()) % ringSize));
}

void appendIpcFrameAtCurrentRingWrite(std::vector<uint8_t>& bytes,
                                      uint16_t messageType,
                                      uint32_t senderId,
                                      uint32_t targetId,
                                      const std::vector<uint8_t>& payload,
                                      size_t ringWriteOffset = 260,
                                      size_t ringDataOffset = 512,
                                      size_t ringSize = 512)
{
    std::vector<uint8_t> frame(28 + payload.size(), 0);
    writeU32LE(frame, 0, 0x495A4F54u);
    writeU16LE(frame, 4, 3);
    writeU16LE(frame, 6, messageType);
    writeU32LE(frame, 8, senderId);
    writeU32LE(frame, 12, targetId);
    writeU32LE(frame, 16, static_cast<uint32_t>(payload.size()));
    writeU64LE(frame, 20, 1234);
    std::copy(payload.begin(), payload.end(), frame.begin() + 28);

    appendBytesAtCurrentRingWrite(bytes, frame, ringWriteOffset, ringDataOffset, ringSize);
}

void appendAssistantResultAtCurrentRingWrite(std::vector<uint8_t>& bytes,
                                             const std::vector<std::pair<int, float>>& params,
                                             uint32_t ozoneId = 0x01020304u,
                                             uint32_t observerId = 0xDEADBEEFu,
                                             size_t ringWriteOffset = 260,
                                             size_t ringDataOffset = 512,
                                             size_t ringSize = 512)
{
    appendIpcFrameAtCurrentRingWrite(
        bytes,
        0x0021,
        ozoneId,
        observerId,
        makeAssistantResultPayload(params),
        ringWriteOffset,
        ringDataOffset,
        ringSize);
}

template <typename Invoke, typename Writer>
json invokeWithDelayedIpcWriter(more_phi::standalone_mcp::IZotopeIPCAssistant& assistant,
                                const std::string& segmentName,
                                Invoke invoke,
                                Writer writer,
                                size_t ringWriteOffset = 260)
{
    const auto* constMemory = assistant.getFakeSegmentForTests(segmentName);
    REQUIRE(constMemory != nullptr);
    auto* memory = const_cast<std::vector<uint8_t>*>(constMemory);
    const auto initialWrite = readU32LEUnchecked(*memory, ringWriteOffset);
    std::atomic<bool> done{false};

    std::thread responder([&, initialWrite]() {
        for (size_t attempt = 0; attempt < 1000 && !done.load(std::memory_order_acquire); ++attempt)
        {
            if (readU32LEUnchecked(*memory, ringWriteOffset) != initialWrite)
            {
                writer(*memory);
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    auto result = invoke();
    done.store(true, std::memory_order_release);
    responder.join();
    return result;
}

void setTestEnv(const char* key, const char* value)
{
#if JUCE_WINDOWS
    if (value == nullptr || value[0] == '\0')
        SetEnvironmentVariableA(key, nullptr);
    else
        SetEnvironmentVariableA(key, value);
#else
    if (value == nullptr || std::strlen(value) == 0)
        unsetenv(key);
    else
        setenv(key, value, 1);
#endif
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
    bool foundIpcAttach = false;
    bool foundIpcStatus = false;
    bool foundIpcSnapshot = false;
    bool foundIpcDump = false;
    bool foundIpcCapture = false;
    bool foundRunAssistantIpc = false;

    for (const auto& tool : listed["result"]["tools"])
    {
        REQUIRE(tool.contains("annotations"));
        const auto name = tool["name"].get<std::string>();
        foundGetParameters = foundGetParameters || name == "ozone_get_parameters";
        foundSetParameter = foundSetParameter || name == "ozone_set_parameter";
        foundAssistant = foundAssistant || name == "ozone_run_master_assistant";
        foundGetState = foundGetState || name == "ozone_get_state";
        foundSetState = foundSetState || name == "ozone_set_state";
        foundIpcAttach = foundIpcAttach || name == "izotope_ipc_attach";
        foundIpcStatus = foundIpcStatus || name == "izotope_ipc_status";
        foundIpcSnapshot = foundIpcSnapshot || name == "izotope_ipc_snapshot";
        foundIpcDump = foundIpcDump || name == "izotope_ipc_dump";
        foundIpcCapture = foundIpcCapture || name == "izotope_ipc_capture";
        foundRunAssistantIpc = foundRunAssistantIpc || name == "ozone_run_assistant";
    }

    REQUIRE(foundGetParameters);
    REQUIRE(foundSetParameter);
    REQUIRE(foundAssistant);
    REQUIRE(foundGetState);
    REQUIRE(foundSetState);
    REQUIRE(foundIpcAttach);
    REQUIRE(foundIpcStatus);
    REQUIRE(foundIpcSnapshot);
    REQUIRE(foundIpcDump);
    REQUIRE(foundIpcCapture);
    REQUIRE(foundRunAssistantIpc);
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

TEST_CASE("Standalone MCP iZotope IPC tools inspect fake read-only memory", "[mcp][standalone][ipc]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto discovery = std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>();
    discovery->setFakeSegmentForTests("fake_izotope_ipc", createFakeIpcMemory());
    auto server = makeServer(host, std::move(discovery));

    const auto attach = server.processJson(request(20, "tools/call", {
        {"name", "izotope_ipc_attach"},
        {"arguments", {{"segment_name", "fake_izotope_ipc"}}}
    }));
    REQUIRE_FALSE(attach["result"]["isError"].get<bool>());
    REQUIRE(attach["result"]["structuredContent"]["attached"].get<bool>());
    REQUIRE(attach["result"]["structuredContent"]["mapped_size_bytes"].get<size_t>() == 256);

    const auto status = server.processJson(request(21, "tools/call", {
        {"name", "izotope_ipc_status"},
        {"arguments", json::object()}
    }));
    REQUIRE_FALSE(status["result"]["isError"].get<bool>());
    REQUIRE(status["result"]["structuredContent"]["attached"].get<bool>());
    REQUIRE(status["result"]["structuredContent"]["platform"].get<std::string>() == "test");

    const auto snapshot = server.processJson(request(22, "tools/call", {
        {"name", "izotope_ipc_snapshot"},
        {"arguments", {{"offset", 0}, {"size_bytes", 96}, {"max_frames", 4}}}
    }));
    REQUIRE_FALSE(snapshot["result"]["isError"].get<bool>());
    const auto& snapshotBody = snapshot["result"]["structuredContent"];
    REQUIRE(snapshotBody["data_hex"].get<std::string>().size() == 192);
    REQUIRE(snapshotBody["data_base64"].is_string());
    REQUIRE(snapshotBody["frame_candidates"].size() == 1);
    REQUIRE(snapshotBody["frame_candidates"][0]["offset"].get<int>() == 32);
    REQUIRE(snapshotBody["frame_candidates"][0]["message_type"].get<int>() == 0x0021);
    REQUIRE(snapshotBody["frame_candidates"][0]["payload_size"].get<int>() == 6);

    const auto dumpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_ipc_dump", ".bin");
    const auto dumped = server.processJson(request(23, "tools/call", {
        {"name", "izotope_ipc_dump"},
        {"arguments", {
            {"output_path", dumpFile.getFullPathName().toStdString()},
            {"offset", 32},
            {"size_bytes", 34}
        }}
    }));
    REQUIRE_FALSE(dumped["result"]["isError"].get<bool>());
    REQUIRE(dumpFile.existsAsFile());
    REQUIRE(dumpFile.getSize() == 34);
    dumpFile.deleteFile();

    const auto detached = server.processJson(request(24, "tools/call", {
        {"name", "izotope_ipc_detach"},
        {"arguments", json::object()}
    }));
    REQUIRE_FALSE(detached["result"]["isError"].get<bool>());
    REQUIRE_FALSE(detached["result"]["structuredContent"]["attached"].get<bool>());
}

TEST_CASE("Standalone MCP iZotope IPC tools return tool errors for unsafe ranges and missing segments", "[mcp][standalone][ipc]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto discovery = std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>();
    discovery->setFakeSegmentForTests("fake_izotope_ipc", createFakeIpcMemory());
    auto server = makeServer(host, std::move(discovery));

    const auto missingSegment = server.processJson(request(30, "tools/call", {
        {"name", "izotope_ipc_attach"},
        {"arguments", json::object()}
    }));
    REQUIRE(missingSegment["result"]["isError"].get<bool>());
    REQUIRE(missingSegment["result"]["structuredContent"]["error"].get<std::string>() == "missing_segment_name");

    const auto attach = server.processJson(request(31, "tools/call", {
        {"name", "izotope_ipc_attach"},
        {"arguments", {{"segment_name", "fake_izotope_ipc"}}}
    }));
    REQUIRE_FALSE(attach["result"]["isError"].get<bool>());

    const auto badRange = server.processJson(request(32, "tools/call", {
        {"name", "izotope_ipc_snapshot"},
        {"arguments", {{"offset", 250}, {"size_bytes", 32}}}
    }));
    REQUIRE(badRange["result"]["isError"].get<bool>());
    REQUIRE(badRange["result"]["structuredContent"]["error"].get<std::string>() == "invalid_range");

    const auto malformed = server.processJson(request(33, "tools/call", {
        {"name", "izotope_ipc_snapshot"},
        {"arguments", {{"offset", -1}, {"size_bytes", 32}}}
    }));
    REQUIRE(malformed.contains("error"));
    REQUIRE(malformed["error"]["code"].get<int>() == -32602);
}

TEST_CASE("Standalone MCP iZotope IPC capture reports baseline diffs and JSONL events", "[mcp][standalone][ipc]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto baseline = createFakeIpcMemory();
    auto changed = baseline;
    changed[62] = 0x55;

    auto discovery = std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>();
    discovery->setFakeSegmentForTests("fake_izotope_ipc", changed);
    auto server = makeServer(host, std::move(discovery));

    const auto attach = server.processJson(request(40, "tools/call", {
        {"name", "izotope_ipc_attach"},
        {"arguments", {{"segment_name", "fake_izotope_ipc"}}}
    }));
    REQUIRE_FALSE(attach["result"]["isError"].get<bool>());

    const auto captureFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_ipc_capture", ".jsonl");

    const auto captured = server.processJson(request(41, "tools/call", {
        {"name", "izotope_ipc_capture"},
        {"arguments", {
            {"offset", 32},
            {"size_bytes", 34},
            {"duration_ms", 0},
            {"baseline_base64", base64Slice(baseline, 32, 34)},
            {"include_changed_bytes", true},
            {"output_path", captureFile.getFullPathName().toStdString()}
        }}
    }));

    REQUIRE_FALSE(captured["result"]["isError"].get<bool>());
    const auto& body = captured["result"]["structuredContent"];
    REQUIRE(body["sample_count"].get<int>() == 1);
    REQUIRE(body["changes_recorded"].get<int>() == 1);
    REQUIRE(body["total_changed_bytes"].get<int>() == 1);
    REQUIRE_FALSE(body["truncated"].get<bool>());
    REQUIRE(body["changes"].size() == 1);
    REQUIRE(body["changes"][0]["changed_ranges"].size() == 1);
    REQUIRE(body["changes"][0]["changed_ranges"][0]["offset"].get<int>() == 62);
    REQUIRE(body["changes"][0]["changed_ranges"][0]["length"].get<int>() == 1);
    REQUIRE(body["changes"][0]["changed_ranges"][0]["previous_hex"].get<std::string>() == "07");
    REQUIRE(body["changes"][0]["changed_ranges"][0]["current_hex"].get<std::string>() == "55");
    REQUIRE(body["changes"][0]["frame_candidates"].size() == 1);

    REQUIRE(captureFile.existsAsFile());
    const auto jsonl = captureFile.loadFileAsString().toStdString();
    REQUIRE(jsonl.find("\"changed_bytes\":1") != std::string::npos);
    captureFile.deleteFile();
}

TEST_CASE("Standalone MCP iZotope IPC capture validates bounds and baseline size", "[mcp][standalone][ipc]")
{
    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto discovery = std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>();
    discovery->setFakeSegmentForTests("fake_izotope_ipc", createFakeIpcMemory());
    auto server = makeServer(host, std::move(discovery));

    const auto attach = server.processJson(request(50, "tools/call", {
        {"name", "izotope_ipc_attach"},
        {"arguments", {{"segment_name", "fake_izotope_ipc"}}}
    }));
    REQUIRE_FALSE(attach["result"]["isError"].get<bool>());

    const auto mismatchedBaseline = server.processJson(request(51, "tools/call", {
        {"name", "izotope_ipc_capture"},
        {"arguments", {
            {"offset", 32},
            {"size_bytes", 34},
            {"duration_ms", 0},
            {"baseline_base64", base64Slice(createFakeIpcMemory(), 32, 8)}
        }}
    }));
    REQUIRE(mismatchedBaseline["result"]["isError"].get<bool>());
    REQUIRE(mismatchedBaseline["result"]["structuredContent"]["error"].get<std::string>() == "baseline_size_mismatch");

    const auto oversized = server.processJson(request(52, "tools/call", {
        {"name", "izotope_ipc_capture"},
        {"arguments", {{"offset", 0}, {"size_bytes", 2 * 1024 * 1024}}}
    }));
    REQUIRE(oversized["result"]["isError"].get<bool>());
    REQUIRE(oversized["result"]["structuredContent"]["error"].get<std::string>() == "invalid_range");

    const auto malformed = server.processJson(request(53, "tools/call", {
        {"name", "izotope_ipc_capture"},
        {"arguments", {{"duration_ms", -1}}}
    }));
    REQUIRE(malformed.contains("error"));
    REQUIRE(malformed["error"]["code"].get<int>() == -32602);
}

TEST_CASE("Standalone MCP Ozone IPC assistant enforces write gates", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemory());
    auto server = makeServer(
        host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto disabled = server.processJson(request(60, "tools/call", {
        {"name", "ozone_run_assistant"},
        {"arguments", {
            {"schema_path", manifestFile.getFullPathName().toStdString()},
            {"segment_name", "fake_izotope_assistant_1234"},
            {"allow_unsafe_write", true}
        }}
    }));

    REQUIRE(disabled["result"]["isError"].get<bool>());
    REQUIRE(disabled["result"]["structuredContent"]["error"].get<std::string>().find("blocked") != std::string::npos);

    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");
    const auto missingArgGate = server.processJson(request(61, "tools/call", {
        {"name", "ozone_run_assistant"},
        {"arguments", {
            {"schema_path", manifestFile.getFullPathName().toStdString()},
            {"segment_name", "fake_izotope_assistant_1234"},
            {"allow_unsafe_write", false}
        }}
    }));

    REQUIRE(missingArgGate["result"]["isError"].get<bool>());
    REQUIRE(missingArgGate["result"]["structuredContent"]["error"].get<std::string>().find("blocked") != std::string::npos);
    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("Standalone MCP Ozone IPC assistant writes request frame and parses fake result", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemory());
    auto server = makeServer(
        host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(70, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"plugin_name_query", "ozone"},
                    {"timeout_ms", 500},
                    {"observer_id", 0xDEADBEEFu},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.62f}, {2, 0.91f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE((int)body["parameters"].size() == 2);
    REQUIRE(body["parameters"][0]["index"].get<int>() == 0);
    REQUIRE(body["parameters"][0]["value"].get<float>() == Approx(0.62f));
    REQUIRE(body["parameters"][1]["index"].get<int>() == 2);
    REQUIRE(body["parameters"][1]["value"].get<float>() == Approx(0.91f));

    const auto* memoryPtr = assistantPtr->getFakeSegmentForTests("fake_izotope_assistant_1234");
    REQUIRE(memoryPtr != nullptr);
    const auto& memory = *memoryPtr;
    REQUIRE(readU32LEFromVector(memory, 260) == 240u);
    REQUIRE(readU32LEFromVector(memory, 512 + 170) == 0x495A4F54u);
    REQUIRE(readU16LEFromVector(memory, 512 + 176) == 0x0020);
    REQUIRE(readU32LEFromVector(memory, 512 + 178) == 0xDEADBEEFu);
    REQUIRE(readU32LEFromVector(memory, 512 + 182) == 0x01020304u);
    REQUIRE(readU32LEFromVector(memory, 512 + 186) == 0u);

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("Standalone MCP Ozone IPC assistant validates schema and timeouts", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemory());
    auto server = makeServer(
        host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto badSchemaFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_bad_ipc_schema", ".json");
    REQUIRE(badSchemaFile.replaceWithText("{\"mapped_size_bytes\":2048}"));

    const auto badSchema = server.processJson(request(80, "tools/call", {
        {"name", "ozone_run_assistant"},
        {"arguments", {
            {"schema_path", badSchemaFile.getFullPathName().toStdString()},
            {"segment_name", "fake_izotope_assistant_1234"},
            {"allow_unsafe_write", true}
        }}
    }));
    REQUIRE(badSchema["result"]["isError"].get<bool>());
    REQUIRE(badSchema["result"]["structuredContent"]["error"].get<std::string>().find("schema bounds") != std::string::npos);

    auto timeoutMemory = createFakeAssistantMemory();
    std::fill(timeoutMemory.begin() + 512 + 128, timeoutMemory.begin() + 512 + 128 + 42, static_cast<uint8_t>(0));
    auto timeoutAssistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    timeoutAssistant->setFakeSegmentForTests("fake_izotope_timeout_1234", timeoutMemory);
    auto timeoutServer = makeServer(
        host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(timeoutAssistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto timeout = timeoutServer.processJson(request(81, "tools/call", {
        {"name", "ozone_run_assistant"},
        {"arguments", {
            {"schema_path", manifestFile.getFullPathName().toStdString()},
            {"segment_name", "fake_izotope_timeout_1234"},
            {"timeout_ms", 50},
            {"allow_unsafe_write", true}
        }}
    }));
    REQUIRE(timeout["result"]["isError"].get<bool>());
    REQUIRE(timeout["result"]["structuredContent"]["error"].get<std::string>().find("timeout") != std::string::npos);

    badSchemaFile.deleteFile();
    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

std::vector<uint8_t> createFakeAssistantMemoryForWrapTest()
{
    constexpr uint32_t ozoneId = 0x01020304u;
    constexpr uint32_t observerId = 0xDEADBEEFu;
    constexpr size_t registryOffset = 64;
    constexpr size_t ringWriteOffset = 260;
    constexpr size_t ringDataOffset = 512;
    constexpr size_t frameHeaderSize = 28;
    // Place AssistantResult at ring position 50, well clear of the wrap zone [502..17]
    constexpr size_t resultRingOffset = 50;
    // Write pointer starts 10 bytes before the end of the 512-byte ring capacity
    constexpr uint32_t initialWritePtr = 502;

    std::vector<uint8_t> bytes(2048, 0);
    writeU32LE(bytes, registryOffset + 0, ozoneId);
    const std::string name = "Ozone 11";
    std::memcpy(bytes.data() + registryOffset + 8, name.data(), name.size());
    writeU32LE(bytes, registryOffset + 36, 1);

    writeU32LE(bytes, ringWriteOffset, initialWritePtr);
    writeU32LE(bytes, 256, 19u);  // readPtr=19: after request write (writePtr=18<readPtr), available=512-19+18=511

    const size_t frameOffset = ringDataOffset + resultRingOffset;
    writeU32LE(bytes, frameOffset + 0,  0x495A4F54u);
    writeU16LE(bytes, frameOffset + 4,  3);
    writeU16LE(bytes, frameOffset + 6,  0x0021);
    writeU32LE(bytes, frameOffset + 8,  ozoneId);
    writeU32LE(bytes, frameOffset + 12, observerId);
    writeU32LE(bytes, frameOffset + 16, 8);
    writeU64LE(bytes, frameOffset + 20, 1234);

    const size_t payloadOffset = frameOffset + frameHeaderSize;
    writeU16LE(bytes, payloadOffset + 0, 1);
    writeU16LE(bytes, payloadOffset + 2, 0);
    writeF32LE(bytes, payloadOffset + 4, 0.75f);
    return bytes;
}

std::vector<uint8_t> createFakeAssistantMemoryWithPreambleFrames()
{
    constexpr uint32_t ozoneId = 0x01020304u;
    constexpr uint32_t observerId = 0xDEADBEEFu;
    constexpr size_t registryOffset = 64;
    constexpr size_t ringWriteOffset = 260;
    constexpr size_t ringDataOffset = 512;
    constexpr size_t frameHeaderSize = 28;

    std::vector<uint8_t> bytes(2048, 0);
    writeU32LE(bytes, registryOffset + 0, ozoneId);
    const std::string name = "Ozone 11";
    std::memcpy(bytes.data() + registryOffset + 8, name.data(), name.size());
    writeU32LE(bytes, registryOffset + 36, 1);
    writeU32LE(bytes, ringWriteOffset, 112u);

    // SpectralData frame (0x0010) at ring position 0, payload = 12 bytes → total 40 bytes
    constexpr size_t preamble0 = ringDataOffset;
    writeU32LE(bytes, preamble0 + 0,  0x495A4F54u);
    writeU16LE(bytes, preamble0 + 4,  3);
    writeU16LE(bytes, preamble0 + 6,  0x0010);
    writeU32LE(bytes, preamble0 + 8,  ozoneId);
    writeU32LE(bytes, preamble0 + 12, observerId);
    writeU32LE(bytes, preamble0 + 16, 12);
    writeU64LE(bytes, preamble0 + 20, 111);

    // LoudnessData frame (0x0011) at ring position 40, payload = 8 bytes → total 36 bytes
    constexpr size_t preamble1 = ringDataOffset + 40;
    writeU32LE(bytes, preamble1 + 0,  0x495A4F54u);
    writeU16LE(bytes, preamble1 + 4,  3);
    writeU16LE(bytes, preamble1 + 6,  0x0011);
    writeU32LE(bytes, preamble1 + 8,  ozoneId);
    writeU32LE(bytes, preamble1 + 12, observerId);
    writeU32LE(bytes, preamble1 + 16, 8);
    writeU64LE(bytes, preamble1 + 20, 222);

    // AssistantResult (0x0021) at ring position 76, payload = 8 bytes → 1 param
    constexpr size_t resultFrame = ringDataOffset + 76;
    writeU32LE(bytes, resultFrame + 0,  0x495A4F54u);
    writeU16LE(bytes, resultFrame + 4,  3);
    writeU16LE(bytes, resultFrame + 6,  0x0021);
    writeU32LE(bytes, resultFrame + 8,  ozoneId);
    writeU32LE(bytes, resultFrame + 12, observerId);
    writeU32LE(bytes, resultFrame + 16, 8);
    writeU64LE(bytes, resultFrame + 20, 333);

    const size_t payloadOffset = resultFrame + frameHeaderSize;
    writeU16LE(bytes, payloadOffset + 0, 1);
    writeU16LE(bytes, payloadOffset + 2, 1);
    writeF32LE(bytes, payloadOffset + 4, 0.44f);
    return bytes;
}

std::vector<uint8_t> createFakeAssistantMemoryAllSlotsOccupied()
{
    // All 4 registry slots filled with non-Ozone entries (no entry named "Ozone")
    constexpr size_t registryOffset = 64;
    constexpr size_t ringWriteOffset = 260;

    std::vector<uint8_t> bytes(2048, 0);

    const char* pluginNames[] = { "Neutron", "Nectar", "Insight" };
    for (int i = 0; i < 3; ++i)
    {
        const size_t entryBase = registryOffset + static_cast<size_t>(i) * 64;
        writeU32LE(bytes, entryBase + 0,  static_cast<uint32_t>(0x0100 + i));
        std::memcpy(bytes.data() + entryBase + 8, pluginNames[i], std::strlen(pluginNames[i]));
        writeU32LE(bytes, entryBase + 36, 1);
    }
    // Slot 3 intentionally inactive (active = 0) to keep it from overlapping ring write ptr
    writeU32LE(bytes, ringWriteOffset, 0);
    return bytes;
}

TEST_CASE("Standalone MCP Ozone IPC assistant handles ring wrap-around write", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryForWrapTest());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(90, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"plugin_name_query", "ozone"},
                    {"timeout_ms", 50},
                    {"observer_id", 0xDEADBEEFu},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.75f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE(body["parameters"][0]["index"].get<int>() == 0);
    REQUIRE(body["parameters"][0]["value"].get<float>() == Approx(0.75f));

    // Verify wrap-around: request frame magic bytes 0-3 = 0x495A4F54 LE = [0x54, 0x4F, 0x5A, 0x49]
    // With initial write ptr = 502 and ring data at offset 512:
    //   frame byte 0 → memory[512 + 502], frame bytes 10-27 → memory[512 + 0..17]
    const auto* memPtr = assistantPtr->getFakeSegmentForTests("fake_izotope_assistant_1234");
    REQUIRE(memPtr != nullptr);
    const auto& memory = *memPtr;
    REQUIRE(memory[512 + 502] == 0x54u);  // 'T' — low byte of magic LE
    REQUIRE(memory[512 + 503] == 0x4Fu);  // 'O'
    REQUIRE(memory[512 + 504] == 0x5Au);  // 'Z'
    REQUIRE(memory[512 + 505] == 0x49u);  // 'I'
    // Write pointer should include the wrapped request and delayed result.
    REQUIRE(readU32LEFromVector(memory, 260) == 54u);

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}


TEST_CASE("Standalone MCP Ozone IPC assistant deregisters observer when manifest supports cleanup", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemory());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile(true);
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(94, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"plugin_name_query", "ozone"},
                    {"timeout_ms", 50},
                    {"observer_id", 0xDEADBEEFu},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.62f}, {2, 0.91f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto* memPtr = assistantPtr->getFakeSegmentForTests("fake_izotope_assistant_1234");
    REQUIRE(memPtr != nullptr);
    const auto& memory = *memPtr;
    REQUIRE(readU32LEFromVector(memory, 260) == 240u);
    bool observerFound = false;
    for (int s = 0; s < 4; ++s)
        if (readU32LEFromVector(memory, 64 + s * 64) == 0xDEADBEEFu) { observerFound = true; break; }
    REQUIRE_FALSE(observerFound);

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("Standalone MCP Ozone IPC assistant skips corrupt frames and finds valid result", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    // Corrupt the magic of the first 28 bytes of the ring, then place a valid frame further in
    auto memory = createFakeAssistantMemory();
    // createFakeAssistantMemory puts AssistantResult at ring position 128 (memory offset 640).
    // Overwrite ring positions 0-27 with invalid magic to simulate corruption.
    std::fill(memory.begin() + 512, memory.begin() + 512 + 28, static_cast<uint8_t>(0xAA));

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", memory);
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(91, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"plugin_name_query", "ozone"},
                    {"timeout_ms", 50},
                    {"observer_id", 0xDEADBEEFu},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendBytesAtCurrentRingWrite(memory, std::vector<uint8_t>(7, 0xAA));
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.62f}, {2, 0.91f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE((int)body["parameters"].size() == 2);
    REQUIRE(body["parameters"][0]["index"].get<int>() == 0);

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("Standalone MCP Ozone IPC assistant skips non-result frames before AssistantResult", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryWithPreambleFrames());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(92, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"plugin_name_query", "ozone"},
                    {"timeout_ms", 50},
                    {"observer_id", 0xDEADBEEFu},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendIpcFrameAtCurrentRingWrite(memory, 0x0010, 0x01020304u, 0xDEADBEEFu, std::vector<uint8_t>(12, 0xAA));
            appendIpcFrameAtCurrentRingWrite(memory, 0x0011, 0x01020304u, 0xDEADBEEFu, std::vector<uint8_t>(8, 0xBB));
            appendAssistantResultAtCurrentRingWrite(memory, {{1, 0.44f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE((int)body["parameters"].size() == 1);
    REQUIRE(body["parameters"][0]["index"].get<int>() == 1);
    REQUIRE(body["parameters"][0]["value"].get<float>() == Approx(0.44f));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("Standalone MCP Ozone IPC assistant returns not-found when all registry slots are occupied by non-Ozone plugins", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryAllSlotsOccupied());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = server.processJson(request(93, "tools/call", {
        {"name", "ozone_run_assistant"},
        {"arguments", {
            {"schema_path", manifestFile.getFullPathName().toStdString()},
            {"segment_name", "fake_izotope_assistant_1234"},
            {"plugin_name_query", "Ozone"},
            {"allow_unsafe_write", true}
        }}
    }));

    REQUIRE(result["result"]["isError"].get<bool>());
    REQUIRE(result["result"]["structuredContent"]["error"].get<std::string>().find("not found") != std::string::npos);

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

// Returns an AssistantResult with parameter indices 0 and 1 — valid for the 3-param FakeOzonePlugin.
std::vector<uint8_t> createFakeAssistantMemoryWithValidIndices()
{
    constexpr uint32_t ozoneId = 0x01020304u;
    constexpr uint32_t observerId = 0xDEADBEEFu;
    constexpr size_t registryOffset = 64;
    constexpr size_t ringWriteOffset = 260;
    constexpr size_t ringDataOffset = 512;
    constexpr size_t resultRingOffset = 128;
    constexpr size_t frameHeaderSize = 28;

    std::vector<uint8_t> bytes(2048, 0);
    writeU32LE(bytes, registryOffset + 0, ozoneId);
    const std::string name = "Ozone 11";
    std::memcpy(bytes.data() + registryOffset + 8, name.data(), name.size());
    writeU32LE(bytes, registryOffset + 36, 1);

    writeU32LE(bytes, ringWriteOffset, 170u);

    const size_t frameOffset = ringDataOffset + resultRingOffset;
    writeU32LE(bytes, frameOffset + 0, 0x495A4F54u);
    writeU16LE(bytes, frameOffset + 4, 3);
    writeU16LE(bytes, frameOffset + 6, 0x0021);
    writeU32LE(bytes, frameOffset + 8, ozoneId);
    writeU32LE(bytes, frameOffset + 12, observerId);
    writeU32LE(bytes, frameOffset + 16, 14);
    writeU64LE(bytes, frameOffset + 20, 1234);

    const size_t payloadOffset = frameOffset + frameHeaderSize;
    writeU16LE(bytes, payloadOffset + 0, 2);
    writeU16LE(bytes, payloadOffset + 2, 0);
    writeF32LE(bytes, payloadOffset + 4, 0.8f);
    writeU16LE(bytes, payloadOffset + 8, 1);
    writeF32LE(bytes, payloadOffset + 10, 0.3f);
    return bytes;
}

std::vector<uint8_t> createFakeAssistantMemoryWithInvalidValue()
{
    auto bytes = createFakeAssistantMemoryWithValidIndices();
    constexpr size_t payloadOffset = 512 + 128 + 28;
    writeF32LE(bytes, payloadOffset + 4, 1.25f);
    return bytes;
}

TEST_CASE("ozone_run_assistant returns IPC results without applying by default", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryWithValidIndices());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const float defaultGain = pluginPtr->getParameters()[0]->getValue();
    const float defaultAnalyze = pluginPtr->getParameters()[1]->getValue();

    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(94, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"timeout_ms", 50},
                    {"allow_unsafe_write", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.8f}, {1, 0.3f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE((int)body["parameters"].size() == 2);
    REQUIRE_FALSE(body.contains("apply_result"));
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(defaultGain));
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(defaultAnalyze));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("ozone_run_assistant applies IPC results only when requested", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryWithValidIndices());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(95, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"timeout_ms", 50},
                    {"allow_unsafe_write", true},
                    {"apply_result", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.8f}, {1, 0.3f}});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE(body["apply_result"]["applied"].get<bool>());
    REQUIRE(body["apply_result"]["requested_count"].get<int>() == 2);
    REQUIRE(body["apply_result"]["applied_count"].get<int>() == 2);
    REQUIRE(body["apply_result"]["errors"].empty());
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(0.8f));
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(0.3f));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("ozone_run_assistant rejects out-of-range IPC parameter indices when applying without partial apply", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    auto memory = createFakeAssistantMemoryWithValidIndices();
    constexpr size_t payloadOffset = 512 + 128 + 28;
    writeU16LE(memory, payloadOffset + 8, 99);
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", memory);
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const float defaultGain = pluginPtr->getParameters()[0]->getValue();
    const float defaultAnalyze = pluginPtr->getParameters()[1]->getValue();

    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(96, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"timeout_ms", 50},
                    {"allow_unsafe_write", true},
                    {"apply_result", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 0.8f}, {99, 0.3f}});
        });

    REQUIRE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE(body["error"].get<std::string>() == "assistant_apply_failed");
    REQUIRE_FALSE(body["apply_result"]["applied"].get<bool>());
    REQUIRE(body["apply_result"]["applied_count"].get<int>() == 0);
    REQUIRE(body["apply_result"]["errors"].size() == 1);
    REQUIRE(body["apply_result"]["errors"][0]["code"].get<std::string>() == "parameter_index_out_of_range");
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(defaultGain));
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(defaultAnalyze));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

TEST_CASE("ozone_run_assistant rejects invalid normalized values when applying without partial apply", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryWithInvalidValue());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const float defaultGain = pluginPtr->getParameters()[0]->getValue();
    const float defaultAnalyze = pluginPtr->getParameters()[1]->getValue();

    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(97, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"timeout_ms", 50},
                    {"allow_unsafe_write", true},
                    {"apply_result", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {{0, 1.25f}, {1, 0.3f}});
        });

    REQUIRE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE(body["error"].get<std::string>() == "assistant_apply_failed");
    REQUIRE_FALSE(body["apply_result"]["applied"].get<bool>());
    REQUIRE(body["apply_result"]["applied_count"].get<int>() == 0);
    REQUIRE(body["apply_result"]["errors"].size() == 1);
    REQUIRE(body["apply_result"]["errors"][0]["code"].get<std::string>() == "parameter_value_out_of_range");
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(defaultGain));
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(defaultAnalyze));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}

std::vector<uint8_t> createFakeAssistantMemoryWithEmptyResult()
{
    auto bytes = createFakeAssistantMemoryWithValidIndices();
    constexpr size_t frameOffset = 512 + 128;
    constexpr size_t payloadOffset = frameOffset + 28;
    writeU32LE(bytes, frameOffset + 16, 2);
    writeU16LE(bytes, payloadOffset + 0, 0);
    return bytes;
}

TEST_CASE("ozone_run_assistant succeeds with empty AssistantResult parameter list", "[mcp][standalone][ipc]")
{
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1");

    auto plugin = std::make_unique<FakeOzonePlugin>();
    auto* pluginPtr = plugin.get();
    FakePluginHostManager host(std::move(plugin));
    host.prepare(44100.0, 512, 2);

    auto assistant = std::make_unique<more_phi::standalone_mcp::IZotopeIPCAssistant>();
    auto* assistantPtr = assistant.get();
    assistant->setFakeSegmentForTests("fake_izotope_assistant_1234", createFakeAssistantMemoryWithEmptyResult());
    auto server = makeServer(host,
        std::make_unique<more_phi::standalone_mcp::IZotopeIPCDiscovery>(),
        std::move(assistant));

    const auto manifestFile = createAssistantManifestFile();
    const float defaultGain = pluginPtr->getParameters()[0]->getValue();
    const float defaultAnalyze = pluginPtr->getParameters()[1]->getValue();

    const auto result = invokeWithDelayedIpcWriter(
        *assistantPtr,
        "fake_izotope_assistant_1234",
        [&]() {
            return server.processJson(request(98, "tools/call", {
                {"name", "ozone_run_assistant"},
                {"arguments", {
                    {"schema_path", manifestFile.getFullPathName().toStdString()},
                    {"segment_name", "fake_izotope_assistant_1234"},
                    {"timeout_ms", 50},
                    {"allow_unsafe_write", true},
                    {"apply_result", true}
                }}
            }));
        },
        [](std::vector<uint8_t>& memory) {
            appendAssistantResultAtCurrentRingWrite(memory, {});
        });

    REQUIRE_FALSE(result["result"]["isError"].get<bool>());
    const auto& body = result["result"]["structuredContent"];
    REQUIRE((int)body["parameters"].size() == 0);
    REQUIRE(body["apply_result"]["applied"].get<bool>());
    REQUIRE(body["apply_result"]["requested_count"].get<int>() == 0);
    REQUIRE(body["apply_result"]["applied_count"].get<int>() == 0);
    REQUIRE(body["apply_result"]["parameters"].empty());
    REQUIRE(body["apply_result"]["errors"].empty());
    REQUIRE(pluginPtr->getParameters()[0]->getValue() == Approx(defaultGain));
    REQUIRE(pluginPtr->getParameters()[1]->getValue() == Approx(defaultAnalyze));

    manifestFile.deleteFile();
    setTestEnv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "");
}
