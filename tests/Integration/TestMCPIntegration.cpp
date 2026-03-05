/*
 * MorphSnap — MCP Integration Tests (Catch2)
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSE_SOCKET closesocket
    #define SOCKET_TYPE SOCKET
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define CLOSE_SOCKET close
    #define SOCKET_TYPE int
    #define INVALID_SOCKET_VALUE -1
#endif

using Catch::Approx;
using json = nlohmann::json;
using namespace morphsnap;

namespace {

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

} // namespace

TEST_CASE("MCP initialize rejects missing bearer token", "[integration][mcp]")
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    const int port = processor.getMCPServer().getPort();
    REQUIRE(waitForServer(port));

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
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();
    REQUIRE(waitForServer(port));

    SocketClient client;
    REQUIRE(client.connectTo(port));
    initializeSession(client, token);

    processor.releaseResources();
}

TEST_CASE("MCP set/get morph state uses authenticated flow and public accessors", "[integration][mcp]")
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    processor.setMorphX(0.5f);
    processor.setMorphY(0.5f);

    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();
    REQUIRE(waitForServer(port));

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

    processor.releaseResources();
}

TEST_CASE("MCP rate limit is enforced through token optimizer bookkeeping", "[integration][mcp]")
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    processor.getTokenOptimizer().setRateLimit(1);

    const int port = processor.getMCPServer().getPort();
    const auto token = processor.getMCPServer().getAuthToken();
    REQUIRE(waitForServer(port));

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
