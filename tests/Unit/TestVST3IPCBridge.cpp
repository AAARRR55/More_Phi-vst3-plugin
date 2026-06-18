/*
 * More-Phi — Unit/TestVST3IPCBridge.cpp
 *
 * Unit tests for the VST3 IPC bridge binary protocol and command dispatcher.
 */
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/VST3IPCBridge.h"
#include "Host/ParameterBridge.h"
#include "Plugin/PluginProcessor.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace more_phi;
using json = nlohmann::json;

namespace {

CommandPacket makeSetParameterCommand(uint32_t commandId,
                                      uint32_t paramId,
                                      double normalizedValue)
{
    CommandPacket packet;
    packet.header.command_id = commandId;
    packet.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::SetParameter);
    packet.header.param_id = paramId;
    packet.header.normalized_value = normalizedValue;
    packet.header.payload_length = 0;
    return packet;
}

// A VST3IPCBridge whose hosted-plugin access is an in-memory fake, so
// executeCommand success paths can be exercised without a real hosted plugin
// (the production applySetParameter/captureState/loadPreset return "no hosted
// plugin loaded" when none is present).
class FakePluginBridge : public VST3IPCBridge
{
public:
    std::vector<double> params;
    std::vector<uint8_t> stateBlob;
    std::vector<uint8_t> lastLoadedPreset;
    bool failNext = false;

    explicit FakePluginBridge(MorePhiProcessor& p, int numParams = 8)
        : VST3IPCBridge(p, p.getInstanceIdentity()), params(static_cast<size_t>(numParams), 0.5)
    {
    }

protected:
    bool applySetParameter(uint32_t paramId, double normalizedValue,
                           double& outBefore, double& outAfter, std::string& outError) override
    {
        if (failNext)
        {
            outError = "injected failure";
            return false;
        }
        if (paramId >= params.size())
        {
            outError = "param_id out of range";
            return false;
        }
        const double v = std::min(1.0, std::max(0.0, normalizedValue));
        outBefore = params[paramId];
        params[paramId] = v;
        outAfter = params[paramId];
        return true;
    }

    bool captureState(std::vector<uint8_t>& outPayload, std::string&) override
    {
        outPayload = stateBlob;
        return true;
    }

    bool loadPresetFromPayload(const std::vector<uint8_t>& payload, std::string&) override
    {
        lastLoadedPreset = payload;
        for (auto& pr : params)
            pr = 0.25;
        return true;
    }
};

} // namespace

TEST_CASE("CommandPacketHeader serializes and deserializes", "[vst3-ipc]")
{
    CommandPacket packet;
    packet.header.command_id = 0xA1B2C3D4u;
    packet.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::Batch);
    packet.header.param_id = 42u;
    packet.header.normalized_value = 0.75;
    packet.header.payload_length = 0;

    const auto raw = VST3IPCBridge::serializeCommand(packet);
    REQUIRE(raw.size() == VST3IPCBridge::kCommandHeaderSize);
    REQUIRE(VST3IPCBridge::kCommandHeaderSize == 21);

    CommandPacketHeader restored{};
    REQUIRE(VST3IPCBridge::deserializeCommandHeader(raw.data(), raw.size(), restored));
    REQUIRE(restored.command_id == packet.header.command_id);
    REQUIRE(restored.command_type == packet.header.command_type);
    REQUIRE(restored.param_id == packet.header.param_id);
    REQUIRE(restored.normalized_value == Catch::Approx(packet.header.normalized_value));
    REQUIRE(restored.payload_length == packet.header.payload_length);
}

TEST_CASE("ResultPacketHeader serializes and deserializes", "[vst3-ipc]")
{
    ResultPacket packet;
    packet.header.command_id = 7u;
    packet.header.status = static_cast<uint8_t>(VST3IPCResultStatus::Success);
    packet.header.value_before = 0.1;
    packet.header.value_after = 0.9;
    packet.header.timestamp_ns = 1234567890123456789ull;
    packet.header.payload_length = 0;

    const auto raw = VST3IPCBridge::serializeResult(packet);
    REQUIRE(raw.size() == VST3IPCBridge::kResultHeaderSize);
    REQUIRE(VST3IPCBridge::kResultHeaderSize == 33);

    ResultPacketHeader restored{};
    REQUIRE(VST3IPCBridge::deserializeResultHeader(raw.data(), raw.size(), restored));
    REQUIRE(restored.command_id == packet.header.command_id);
    REQUIRE(restored.status == packet.header.status);
    REQUIRE(restored.value_before == Catch::Approx(packet.header.value_before));
    REQUIRE(restored.value_after == Catch::Approx(packet.header.value_after));
    REQUIRE(restored.timestamp_ns == packet.header.timestamp_ns);
    REQUIRE(restored.payload_length == packet.header.payload_length);
}

TEST_CASE("Command and result packets round-trip with payloads", "[vst3-ipc]")
{
    CommandPacket cmd;
    cmd.header.command_id = 99u;
    cmd.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::Batch);
    cmd.header.param_id = 0u;
    cmd.header.normalized_value = 0.0;
    cmd.header.payload_length = 12;
    cmd.payload = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x01, 0x00, 0x00, 0x00 };

    const auto cmdRaw = VST3IPCBridge::serializeCommand(cmd);
    REQUIRE(cmdRaw.size() == VST3IPCBridge::kCommandHeaderSize + cmd.payload.size());

    CommandPacketHeader cmdHeader{};
    REQUIRE(VST3IPCBridge::deserializeCommandHeader(cmdRaw.data(), cmdRaw.size(), cmdHeader));
    REQUIRE(cmdHeader.payload_length == static_cast<uint32_t>(cmd.payload.size()));

    const std::vector<uint8_t> cmdPayload(cmdRaw.begin() + VST3IPCBridge::kCommandHeaderSize,
                                          cmdRaw.end());
    REQUIRE(cmdPayload == cmd.payload);

    ResultPacket result;
    result.header.command_id = 99u;
    result.header.status = static_cast<uint8_t>(VST3IPCResultStatus::Success);
    result.header.value_before = 0.0;
    result.header.value_after = 1.0;
    result.header.timestamp_ns = 0ull;
    result.header.payload_length = 4;
    result.payload = { 'd', 'o', 'n', 'e' };

    const auto resultRaw = VST3IPCBridge::serializeResult(result);
    REQUIRE(resultRaw.size() == VST3IPCBridge::kResultHeaderSize + result.payload.size());

    ResultPacketHeader resultHeader{};
    REQUIRE(VST3IPCBridge::deserializeResultHeader(resultRaw.data(), resultRaw.size(), resultHeader));
    REQUIRE(resultHeader.payload_length == static_cast<uint32_t>(result.payload.size()));

    const std::vector<uint8_t> resultPayload(resultRaw.begin() + VST3IPCBridge::kResultHeaderSize,
                                             resultRaw.end());
    REQUIRE(resultPayload == result.payload);
}

TEST_CASE("executeCommand returns controlled failure when no plugin is loaded", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    VST3IPCBridge bridge(processor, processor.getInstanceIdentity());

    SECTION("SET_PARAM")
    {
        const auto result = bridge.executeCommand(makeSetParameterCommand(1u, 0u, 0.5));
        REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
        REQUIRE(result.header.command_id == 1u);
    }

    SECTION("GET_STATE")
    {
        CommandPacket cmd;
        cmd.header = { 2u, static_cast<uint8_t>(VST3IPCCommandType::GetState), 0u, 0.0, 0u };
        const auto result = bridge.executeCommand(cmd);
        REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
    }

    SECTION("LOAD_PRESET")
    {
        CommandPacket cmd;
        cmd.header = { 3u, static_cast<uint8_t>(VST3IPCCommandType::LoadPreset), 0u, 0.0, 4u };
        cmd.payload = { 't', 'e', 's', 't' };
        const auto result = bridge.executeCommand(cmd);
        REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
    }
}

TEST_CASE("BatchParamDiff serializes and deserializes little-endian", "[vst3-ipc]")
{
    const std::vector<BatchParamDiff> diffs = { { 1001u, 0.25, 0.50 }, { 5001u, 0.0, 0.75 } };
    const auto bytes = VST3IPCBridge::serializeBatchDiffs(diffs);

    REQUIRE(VST3IPCBridge::kBatchDiffSize == 20u);
    REQUIRE(bytes.size() == diffs.size() * VST3IPCBridge::kBatchDiffSize);

    const auto restored = VST3IPCBridge::deserializeBatchDiffs(bytes.data(), bytes.size());
    REQUIRE(restored.size() == diffs.size());
    REQUIRE(restored[0].paramId == 1001u);
    REQUIRE(restored[0].before == Catch::Approx(0.25));
    REQUIRE(restored[0].after == Catch::Approx(0.50));
    REQUIRE(restored[1].paramId == 5001u);
    REQUIRE(restored[1].before == Catch::Approx(0.0));
    REQUIRE(restored[1].after == Catch::Approx(0.75));
}

TEST_CASE("BATCH payload is parsed as (param_id, normalized_value) pairs", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    VST3IPCBridge bridge(processor, processor.getInstanceIdentity());

    CommandPacket cmd;
    cmd.header.command_id = 4u;
    cmd.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::Batch);
    cmd.header.param_id = 0u;
    cmd.header.normalized_value = 0.0;
    cmd.header.payload_length = 24; // two pairs

    // Pair 1: param_id=0, value=0.25
    cmd.payload.resize(24, 0);
    std::memcpy(cmd.payload.data(), &cmd.header.command_id, sizeof(uint32_t)); // reuse as dummy id
    const double value1 = 0.25;
    std::memcpy(cmd.payload.data() + sizeof(uint32_t), &value1, sizeof(double));

    const uint32_t param2 = 1u;
    const double value2 = 0.75;
    std::memcpy(cmd.payload.data() + 12, &param2, sizeof(uint32_t));
    std::memcpy(cmd.payload.data() + 12 + sizeof(uint32_t), &value2, sizeof(double));

    const auto result = bridge.executeCommand(cmd);
    // No hosted plugin is loaded, so the batch fails on the first parameter.
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
}

TEST_CASE("executeCommand can be called from a non-message thread safely", "[vst3-ipc][threading]")
{
    MorePhiProcessor processor;
    VST3IPCBridge bridge(processor, processor.getInstanceIdentity());

    std::vector<ResultPacket> results(4);
    std::thread worker([&]
    {
        for (size_t i = 0; i < results.size(); ++i)
            results[i] = bridge.executeCommand(makeSetParameterCommand(static_cast<uint32_t>(i), 0u, 0.1 * static_cast<double>(i)));
    });
    worker.join();

    for (const auto& result : results)
    {
        // Controlled failure because no plugin is loaded; the important part is
        // that the call did not touch audio-thread-only state.
        REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
    }
}

TEST_CASE("exportParameterRegistry writes a valid JSON file", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    VST3IPCBridge bridge(processor, processor.getInstanceIdentity());

    const juce::File path = bridge.getParameterRegistryPath();
    path.deleteFile();

    REQUIRE(bridge.exportParameterRegistry());
    REQUIRE(path.existsAsFile());

    const auto text = path.loadFileAsString().toStdString();
    const auto parsed = json::parse(text);
    REQUIRE(parsed.is_array());
}

TEST_CASE("Endpoint and registry paths contain the instance identity", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    processor.startPendingMCPServerForTesting();

    VST3IPCBridge bridge(processor, processor.getInstanceIdentity());

    const juce::String endpoint = bridge.getEndpointPath();
    const juce::String registry = bridge.getParameterRegistryPath().getFullPathName();
    const juce::String instanceId = processor.getInstanceIdentity().instanceId;

    REQUIRE(instanceId.isNotEmpty());
    REQUIRE(endpoint.contains(instanceId));
    REQUIRE(registry.contains(instanceId));
    REQUIRE(registry.contains("_registry.json"));

    processor.releaseResources();
}

TEST_CASE("executeCommand SET_PARAM succeeds and returns verified before/after", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 8); // params default to 0.5

    const auto result = bridge.executeCommand(makeSetParameterCommand(1u, 3u, 0.8));
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Success));
    REQUIRE(result.header.command_id == 1u);
    REQUIRE(result.header.value_before == Catch::Approx(0.5));
    REQUIRE(result.header.value_after == Catch::Approx(0.8));
}

TEST_CASE("executeCommand SET_PARAM out-of-range parameter fails controlled", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 4);

    const auto result = bridge.executeCommand(makeSetParameterCommand(1u, 99u, 0.5));
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
}

TEST_CASE("executeCommand BATCH returns per-param diffs in the result payload", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 8); // params default to 0.5

    CommandPacket cmd;
    cmd.header.command_id = 7u;
    cmd.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::Batch);
    cmd.header.payload_length = 24; // two (u32,double) pairs
    cmd.payload.resize(24, 0);
    const uint32_t p1 = 2u;
    const double v1 = 0.25;
    const uint32_t p2 = 5u;
    const double v2 = 0.75;
    std::memcpy(cmd.payload.data(), &p1, sizeof(uint32_t));
    std::memcpy(cmd.payload.data() + sizeof(uint32_t), &v1, sizeof(double));
    std::memcpy(cmd.payload.data() + 12, &p2, sizeof(uint32_t));
    std::memcpy(cmd.payload.data() + 12 + sizeof(uint32_t), &v2, sizeof(double));

    const auto result = bridge.executeCommand(cmd);
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Success));
    REQUIRE(result.header.payload_length == 2u * VST3IPCBridge::kBatchDiffSize);

    const auto diffs = VST3IPCBridge::deserializeBatchDiffs(result.payload.data(), result.payload.size());
    REQUIRE(diffs.size() == 2);
    REQUIRE(diffs[0].paramId == 2u);
    REQUIRE(diffs[0].before == Catch::Approx(0.5));
    REQUIRE(diffs[0].after == Catch::Approx(0.25));
    REQUIRE(diffs[1].paramId == 5u);
    REQUIRE(diffs[1].before == Catch::Approx(0.5));
    REQUIRE(diffs[1].after == Catch::Approx(0.75));
}

TEST_CASE("executeCommand BATCH failure returns a failure result", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 8);
    bridge.failNext = true;

    CommandPacket cmd;
    cmd.header.command_id = 9u;
    cmd.header.command_type = static_cast<uint8_t>(VST3IPCCommandType::Batch);
    cmd.header.payload_length = 12;
    cmd.payload.resize(12, 0);
    const uint32_t p = 1u;
    const double v = 0.5;
    std::memcpy(cmd.payload.data(), &p, sizeof(uint32_t));
    std::memcpy(cmd.payload.data() + sizeof(uint32_t), &v, sizeof(double));

    const auto result = bridge.executeCommand(cmd);
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Failure));
    REQUIRE(result.header.command_id == 9u);
}

TEST_CASE("executeCommand GET_STATE returns the captured state blob", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 4);
    bridge.stateBlob = { 's', 't', 'a', 't', 'e' };

    CommandPacket cmd;
    cmd.header = { 11u, static_cast<uint8_t>(VST3IPCCommandType::GetState), 0u, 0.0, 0u };
    const auto result = bridge.executeCommand(cmd);
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Success));
    REQUIRE(result.payload == bridge.stateBlob);
}

TEST_CASE("executeCommand LOAD_PRESET applies the preset payload", "[vst3-ipc]")
{
    MorePhiProcessor processor;
    FakePluginBridge bridge(processor, 4);

    CommandPacket cmd;
    cmd.header = { 13u, static_cast<uint8_t>(VST3IPCCommandType::LoadPreset), 0u, 0.0, 4u };
    cmd.payload = { 'W', 'a', 'r', 'm' };
    const auto result = bridge.executeCommand(cmd);
    REQUIRE(result.header.status == static_cast<uint8_t>(VST3IPCResultStatus::Success));
    REQUIRE(bridge.lastLoadedPreset == (std::vector<uint8_t>{ 'W', 'a', 'r', 'm' }));
}
