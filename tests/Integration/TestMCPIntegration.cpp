/*
 * More-Phi — MCP Integration Tests (Catch2)
 */

// On Windows, WIN32_LEAN_AND_MEAN must be defined before ANY includes
// to prevent <windows.h> (pulled in by <thread> etc.) from including
// <winsock.h>, which conflicts with <winsock2.h>.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSE_SOCKET closesocket
    #define SOCKET_TYPE SOCKET
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define CLOSE_SOCKET close
    #define SOCKET_TYPE int
    #define INVALID_SOCKET_VALUE -1
#endif

#include "Plugin/PluginProcessor.h"
#include "AI/AutomationControlPlane.h"
#include "AI/TrackAssistantStore.h"

using Catch::Approx;
using json = nlohmann::json;
using namespace more_phi;

namespace {

struct ScopedTrackAssistantStore
{
    explicit ScopedTrackAssistantStore(const char* suffix)
    {
        directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile(juce::String("morephi_track_assistant_store_") + suffix, "");
        directory.createDirectory();
        TrackAssistantStore::setStoreDirectoryOverrideForTests(directory);
    }

    ~ScopedTrackAssistantStore()
    {
        TrackAssistantStore::clearStoreDirectoryOverrideForTests();
        directory.deleteRecursively();
    }

    juce::File directory;
};

struct ScopedAutomationStore
{
    explicit ScopedAutomationStore(const char* suffix)
    {
        directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile(juce::String("morephi_automation_store_") + suffix, "");
        directory.createDirectory();
        AutomationRuntime::setStoreDirectoryOverrideForTests(directory);
    }

    ~ScopedAutomationStore()
    {
        AutomationRuntime::clearStoreDirectoryOverrideForTests();
        directory.deleteRecursively();
    }

    juce::File directory;
};

class SocketClient
{
public:
    SocketClient()
        : socket_(INVALID_SOCKET_VALUE)
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }

    ~SocketClient()
    {
        disconnect();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool connectTo(int port)
    {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET_VALUE)
            return false;

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

        if (::connect(socket_, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
        {
            disconnect();
            return false;
        }

        return true;
    }

    void disconnect()
    {
        if (socket_ != INVALID_SOCKET_VALUE)
        {
            CLOSE_SOCKET(socket_);
            socket_ = INVALID_SOCKET_VALUE;
        }
    }

    bool sendLine(const std::string& line)
    {
        if (socket_ == INVALID_SOCKET_VALUE)
            return false;
        const std::string payload = line + "\n";
        return ::send(socket_, payload.c_str(), static_cast<int>(payload.size()), 0) > 0;
    }

    std::string receiveLine(int timeoutMs = 2000)
    {
        if (socket_ == INVALID_SOCKET_VALUE)
            return {};

#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeoutMs);
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        std::string result;
        char buffer[4096];
        while (result.find('\n') == std::string::npos)
        {
            const int bytesRead = recv(socket_, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0)
                break;
            buffer[bytesRead] = '\0';
            result += buffer;
        }

        if (const auto newlinePos = result.find('\n'); newlinePos != std::string::npos)
            result.resize(newlinePos);

        return result;
    }

private:
    SOCKET_TYPE socket_;
};

bool localTcpProviderAvailable()
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;
#endif

    const SOCKET_TYPE probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET_VALUE)
    {
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    const bool ok = ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0
        && ::listen(probe, 1) == 0;

    CLOSE_SOCKET(probe);
#ifdef _WIN32
    WSACleanup();
#endif
    return ok;
}

bool waitForServer(int port, int maxWaitMs = 3000)
{
    const int attempts = maxWaitMs / 50;
    for (int i = 0; i < attempts; ++i)
    {
        SocketClient client;
        if (client.connectTo(port))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool waitForDeferredMCPServer(MorePhiProcessor& processor, int maxWaitMs = 3000)
{
    const int attempts = maxWaitMs / 50;
    for (int i = 0; i < attempts; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);

#if MORE_PHI_TEST_MODE
        processor.startPendingMCPServerForTesting();
#endif

        const int port = processor.getMCPServer().getPort();
        if (port > 0 && processor.getMCPServer().isHealthy() && waitForServer(port, 50))
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    return false;
}

json rpcRequest(const std::string& method, const json& params, int id)
{
    return json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", id}
    };
}

json sendRpc(SocketClient& client, const json& request)
{
    REQUIRE(client.sendLine(request.dump()));
    const std::string response = client.receiveLine();
    REQUIRE_FALSE(response.empty());
    return json::parse(response);
}

void initializeSession(SocketClient& client, const juce::String& token)
{
    const auto response = sendRpc(client, rpcRequest(
        "initialize",
        {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"bearer_token", token.toStdString()}
        },
        1));

    REQUIRE(response.contains("result"));
    REQUIRE(response["result"].contains("instanceId"));
}

juce::File createMCPRenderInputFile()
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_mcp_render_input", ".wav");

    juce::AudioBuffer<float> buffer(2, 2048);
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 440.0;
    constexpr double twoPi = 6.28318530717958647692;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(0.15 * std::sin(twoPi * frequency * static_cast<double>(sample) / sampleRate));
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample(channel, sample, value);
    }

    juce::WavAudioFormat wavFormat;
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (stream == nullptr)
        return {};

    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wavFormat.createWriterFor(stream.release(), sampleRate, 2, 24, {}, 0));
    if (writer == nullptr)
        return {};

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
        return {};

    writer.reset();
    return file;
}

} // namespace

TEST_CASE("MCP initialize rejects missing bearer token", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();

    SocketClient client;
    REQUIRE(client.connectTo(port));

    const auto response = sendRpc(client, rpcRequest(
        "initialize",
        {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()}},
        1));

    REQUIRE(response.contains("error"));
    REQUIRE(response["error"]["message"].get<std::string>().find("Unauthorized") != std::string::npos);

    processor.releaseResources();
}

TEST_CASE("MCP initialize succeeds with instance auth token", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    processor.releaseResources();
}

TEST_CASE("MCP tools/list and tools/call wrappers work with legacy handlers", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    const auto listResponse = sendRpc(client, rpcRequest("tools/list", json::object(), 2));
    REQUIRE(listResponse.contains("result"));
    REQUIRE(listResponse["result"].contains("tools"));
    REQUIRE(listResponse["result"]["tools"].is_array());

    bool foundSummary = false;
    bool foundIpcAssistant = false;
    for (const auto& tool : listResponse["result"]["tools"])
    {
        if (tool["name"].get<std::string>() == "analysis.get_summary")
            foundSummary = true;
        if (tool["name"].get<std::string>() == "morephi_ipc_run_assistant")
            foundIpcAssistant = true;
    }
    REQUIRE(foundSummary);
    REQUIRE(foundIpcAssistant);

    const auto callResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "hosted_plugin.info"},
            {"arguments", json::object()}
        },
        3));

    REQUIRE(callResponse.contains("result"));
    REQUIRE(callResponse["result"].contains("structuredContent"));
    REQUIRE(callResponse["result"]["structuredContent"]["name"].get<std::string>() == "More-Phi");
    REQUIRE_FALSE(callResponse["result"]["isError"].get<bool>());

    processor.releaseResources();
}

TEST_CASE("MCP tools/call can edit More-Phi runtime parameters", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    ScopedAutomationStore scopedAutomation("edit");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    const auto callResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "more_phi.set_parameter"},
            {"arguments", {
                {"parameter_id", "bypass"},
                {"value", 1.0}
            }}
        },
        4));

    REQUIRE(callResponse.contains("result"));
    REQUIRE(callResponse["result"]["structuredContent"]["success"].get<bool>());
    REQUIRE(callResponse["result"]["structuredContent"]["parameter_id"].get<std::string>() == "bypass");
    REQUIRE_FALSE(callResponse["result"]["isError"].get<bool>());
    REQUIRE(processor.getAPVTS().getParameter("bypass")->getValue() == Approx(1.0f));

    processor.releaseResources();
}

TEST_CASE("MCP mastering render job can be started, polled, and selected", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    ScopedAutomationStore scopedAutomation("render");
    ScopedTrackAssistantStore scopedStore("render");

    const auto inputFile = createMCPRenderInputFile();
    REQUIRE(inputFile.existsAsFile());

    const auto outputDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_mcp_render_output", "");
    REQUIRE(outputDirectory.createDirectory());

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    json renderArguments{
        {"dry_run", false},
        {"allow_passthrough", true},
        {"input_path", inputFile.getFullPathName().toStdString()},
        {"output_path", outputDirectory.getFullPathName().toStdString()},
        {"candidate_count", 1},
        {"duration_seconds", 0.05},
        {"block_size", 128},
        {"channels", 2},
        {"parallel_workers", 1}
    };

    auto startResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "mastering.render_batch"},
            {"arguments", renderArguments}
        },
        4));

    REQUIRE(startResponse.contains("result"));
    if (startResponse["result"]["structuredContent"].value("approval_required", false))
    {
        const auto approvalId = startResponse["result"]["structuredContent"]["approval_request"]["id"].get<std::string>();
        const auto approvalResponse = sendRpc(client, rpcRequest(
            "tools/call",
            {
                {"name", "permission.approve"},
                {"arguments", {{"approval_id", approvalId}}}
            },
            404));
        REQUIRE(approvalResponse["result"]["structuredContent"]["success"].get<bool>());

        renderArguments["approval_id"] = approvalId;
        startResponse = sendRpc(client, rpcRequest(
            "tools/call",
            {
                {"name", "mastering.render_batch"},
                {"arguments", renderArguments}
            },
            405));
        REQUIRE(startResponse.contains("result"));
    }

    const auto jobId = startResponse["result"]["structuredContent"]["job_id"].get<std::string>();
    const auto trackId = startResponse["result"]["structuredContent"]["track_id"].get<std::string>();
    REQUIRE_FALSE(jobId.empty());
    REQUIRE(TrackAssistantStore::isValidTrackId(juce::String(trackId)));

    json statusResponse;
    bool completed = false;
    for (int attempt = 0; attempt < 100 && !completed; ++attempt)
    {
        statusResponse = sendRpc(client, rpcRequest(
            "tools/call",
            {
                {"name", "mastering.render_status"},
                {"arguments", {{"job_id", jobId}}}
            },
            5 + attempt));

        REQUIRE(statusResponse.contains("result"));
        completed = statusResponse["result"]["structuredContent"]["completed"].get<bool>();
        if (!completed)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(completed);
    const auto& status = statusResponse["result"]["structuredContent"];
    REQUIRE(status["candidates"].is_array());
    REQUIRE(status["candidates"].size() == 1);

    const auto candidateId = status["candidates"][0]["id"].get<std::string>();
    const auto outputPath = status["candidates"][0]["output_path"].get<std::string>();
    REQUIRE(juce::File(outputPath).existsAsFile());

    const auto selectResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "mastering.select_candidate"},
            {"arguments", {{"candidate_id", candidateId}}}
        },
        200));

    REQUIRE(selectResponse.contains("result"));
    REQUIRE(selectResponse["result"]["structuredContent"]["selected"].get<bool>());
    REQUIRE(selectResponse["result"]["structuredContent"]["output_path"].get<std::string>() == outputPath);
    REQUIRE(selectResponse["result"]["structuredContent"]["track_id"].get<std::string>() == trackId);
    REQUIRE(selectResponse["result"]["structuredContent"]["track_status"].get<std::string>() == "mastering_complete");

    const auto searchResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "ozone_track_search"},
            {"arguments", {{"query", inputFile.getFileNameWithoutExtension().toStdString()}}}
        },
        201));

    REQUIRE(searchResponse.contains("result"));
    const auto& search = searchResponse["result"]["structuredContent"];
    REQUIRE(search["success"].get<bool>());
    REQUIRE(search["total"].get<int>() == 1);
    REQUIRE(search["results"][0]["track_id"].get<std::string>() == trackId);

    const auto infoResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "ozone_track_get_info"},
            {"arguments", {{"track_id", trackId}, {"include_history", true}}}
        },
        202));

    REQUIRE(infoResponse.contains("result"));
    const auto& info = infoResponse["result"]["structuredContent"];
    REQUIRE(info["success"].get<bool>());
    REQUIRE(info["status"].get<std::string>() == "mastering_complete");
    REQUIRE(info["history"].is_array());

    const auto invalidUpdateResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "ozone_track_update_status"},
            {"arguments", {{"track_id", trackId}, {"new_status", "on_hold"}}}
        },
        204));

    REQUIRE(invalidUpdateResponse.contains("result"));
    REQUIRE(invalidUpdateResponse["result"]["isError"].get<bool>());
    REQUIRE(invalidUpdateResponse["result"]["structuredContent"]["error"].get<std::string>() == "reason_required");

    const auto analyzeResponse = sendRpc(client, rpcRequest(
        "tools/call",
        {
            {"name", "ozone_track_analyze"},
            {"arguments", {{"track_id", trackId}, {"analysis_profile", "streaming"}}}
        },
        205));

    REQUIRE(analyzeResponse.contains("result"));
    const auto& analysis = analyzeResponse["result"]["structuredContent"];
    REQUIRE(analysis["success"].get<bool>());
    REQUIRE(analysis["track_id"].get<std::string>() == trackId);
    REQUIRE(analysis["analysis"]["profile"].get<std::string>() == "streaming");
    REQUIRE(analysis["analysis"]["selected_candidate_id"].get<std::string>() == candidateId);

    processor.releaseResources();
    outputDirectory.deleteRecursively();
    inputFile.deleteFile();
}

TEST_CASE("MCP set/get morph state uses authenticated flow and public accessors", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    ScopedAutomationStore scopedAutomation("morph");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    processor.setMorphX(0.5f);
    processor.setMorphY(0.5f);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    const auto setResponse = sendRpc(client, rpcRequest(
        "set_morph_position",
        {{"x", 0.25}, {"y", 0.75}},
        2));
    REQUIRE(setResponse.contains("result"));
    REQUIRE(setResponse["result"]["success"].get<bool>());

    const auto getResponse = sendRpc(client, rpcRequest("get_morph_state", json::object(), 3));
    REQUIRE(getResponse.contains("result"));
    REQUIRE(getResponse["result"].contains("x"));
    REQUIRE(getResponse["result"].contains("y"));
    REQUIRE(getResponse["result"]["x"].get<float>() == Approx(0.25f).margin(0.01f));
    REQUIRE(getResponse["result"]["y"].get<float>() == Approx(0.75f).margin(0.01f));
    REQUIRE(processor.getMorphX() == Approx(0.25f).margin(0.01f));
    REQUIRE(processor.getMorphY() == Approx(0.75f).margin(0.01f));

    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    REQUIRE(processor.getMorphX() == Approx(0.25f).margin(0.01f));
    REQUIRE(processor.getMorphY() == Approx(0.75f).margin(0.01f));

    processor.releaseResources();
}

TEST_CASE("MCP rate limit is enforced through token optimizer bookkeeping", "[integration][mcp]")
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    if (!localTcpProviderAvailable())
        SKIP("Local TCP provider unavailable; MCP integration coverage blocked");

    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    processor.getTokenOptimizer().setRateLimit(1);

    REQUIRE(waitForDeferredMCPServer(processor));
    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    const auto first = sendRpc(client, rpcRequest("get_plugin_info", json::object(), 2));
    REQUIRE(first.contains("result"));
    REQUIRE(first["result"].contains("name"));

    const auto second = sendRpc(client, rpcRequest("get_plugin_info", json::object(), 3));
    REQUIRE(second.contains("error"));
    REQUIRE(second["error"]["code"].get<int>() == -32000);

    processor.releaseResources();
}
