/*
 * MorphSnap — Integration Tests for MCP Server
 * Tests the Model Context Protocol server functionality.
 *
 * Build: cmake -DMORPHSNAP_BUILD_TESTS=ON && cmake --build
 * Run: ./MorphSnap_IntegrationTests
 */
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>

// Platform-specific socket includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSE_SOCKET closesocket
    #define SOCKET_TYPE SOCKET
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define CLOSE_SOCKET close
    #define SOCKET_TYPE int
    #define INVALID_SOCKET_VALUE -1
#endif

#include "AI/MCPServer.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {
namespace test {

// ── Test Framework (Minimal) ───────────────────────────────────────────────────

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    testsRun++; \
    try { \
        test_##name(); \
        testsPassed++; \
        std::cout << "PASSED" << std::endl; \
    } catch (const std::exception& e) { \
        testsFailed++; \
        std::cout << "FAILED: " << e.what() << std::endl; \
    } \
} while(0)

#define ASSERT(condition) do { \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        throw std::runtime_error("Assertion failed: " #a " != " #b); \
    } \
} while(0)

// ── Socket Helper ──────────────────────────────────────────────────────────────

class SocketClient
{
public:
    SocketClient() : socket_(INVALID_SOCKET_VALUE)
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

    bool connect(int port)
    {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET_VALUE)
            return false;

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

        if (::connect(socket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0)
        {
            CLOSE_SOCKET(socket_);
            socket_ = INVALID_SOCKET_VALUE;
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

    bool send(const std::string& data)
    {
        return ::send(socket_, data.c_str(), static_cast<int>(data.length()), 0) > 0;
    }

    std::string receive(int timeoutMs = 1000)
    {
        std::string result;
        char buffer[4096];

        // Set socket timeout
#ifdef _WIN32
        DWORD tv = timeoutMs;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        int bytesReceived = recv(socket_, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0';
            result = buffer;
        }
        return result;
    }

private:
    SOCKET_TYPE socket_;
};

// ── JSON-RPC Helper ────────────────────────────────────────────────────────────

std::string createJsonRpcRequest(const std::string& method, const std::string& params = "{}")
{
    return R"({"jsonrpc":"2.0","method":")" + method + R"(","params":)" + params + R"(,"id":1})";
}

bool containsJsonField(const std::string& json, const std::string& field)
{
    return json.find("\"" + field + "\"") != std::string::npos;
}

// ── MCP Server Tests ───────────────────────────────────────────────────────────

TEST(mcp_server_starts)
{
    MorphSnapProcessor processor;

    // MCP server should start automatically in prepareToPlay
    processor.prepareToPlay(48000.0, 256);

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT(processor.getMCPServer().isRunning());

    processor.releaseResources();
}

TEST(mcp_server_accepts_connection)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SocketClient client;
    bool connected = client.connect(30001);

    ASSERT(connected);

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_initialize_request)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SocketClient client;
    ASSERT(client.connect(30001));

    // Send initialize request
    std::string request = createJsonRpcRequest("initialize",
        R"({"protocolVersion":"2024-11-05","capabilities":{}})");

    ASSERT(client.send(request + "\n"));

    std::string response = client.receive(2000);

    // Should get a valid response
    ASSERT(!response.empty());
    ASSERT(containsJsonField(response, "jsonrpc"));

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_tools_list)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SocketClient client;
    ASSERT(client.connect(30001));

    // Send tools/list request
    std::string request = createJsonRpcRequest("tools/list");

    ASSERT(client.send(request + "\n"));

    std::string response = client.receive(2000);

    ASSERT(!response.empty());
    // Should contain morph tool
    ASSERT(containsJsonField(response, "morph") ||
           containsJsonField(response, "tools"));

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_morph_tool)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Set initial morph position
    processor.morphX.store(0.5f);
    processor.morphY.store(0.5f);

    SocketClient client;
    ASSERT(client.connect(30001));

    // Send morph command
    std::string request = createJsonRpcRequest("tools/call",
        R"({"name":"morph","arguments":{"x":0.25,"y":0.75}})");

    ASSERT(client.send(request + "\n"));

    std::string response = client.receive(2000);

    ASSERT(!response.empty());

    // Give time for command to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify morph position changed (may not be exact due to physics)
    float newX = processor.morphX.load();
    float newY = processor.morphY.load();

    // Position should have moved from 0.5
    ASSERT(std::abs(newX - 0.5f) > 0.01f || std::abs(newY - 0.5f) > 0.01f);

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_get_state)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SocketClient client;
    ASSERT(client.connect(30001));

    // Send getMorphState command
    std::string request = createJsonRpcRequest("tools/call",
        R"({"name":"getMorphState","arguments":{}})");

    ASSERT(client.send(request + "\n"));

    std::string response = client.receive(2000);

    ASSERT(!response.empty());
    // Should contain state information
    ASSERT(containsJsonField(response, "x") ||
           containsJsonField(response, "y") ||
           containsJsonField(response, "morph"));

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_invalid_request)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SocketClient client;
    ASSERT(client.connect(30001));

    // Send invalid JSON
    ASSERT(client.send("{invalid json}\n"));

    std::string response = client.receive(2000);

    // Should get an error response
    ASSERT(!response.empty());
    ASSERT(containsJsonField(response, "error"));

    client.disconnect();
    processor.releaseResources();
}

TEST(mcp_multiple_connections)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Create multiple clients
    SocketClient client1, client2;

    ASSERT(client1.connect(30001));
    ASSERT(client2.connect(30001));

    // Both should be able to send commands
    std::string request = createJsonRpcRequest("tools/list");

    ASSERT(client1.send(request + "\n"));
    ASSERT(client2.send(request + "\n"));

    std::string response1 = client1.receive(2000);
    std::string response2 = client2.receive(2000);

    ASSERT(!response1.empty());
    ASSERT(!response2.empty());

    client1.disconnect();
    client2.disconnect();
    processor.releaseResources();
}

// ── State Persistence Tests ────────────────────────────────────────────────────

TEST(plugin_state_save_load)
{
    MorphSnapProcessor processor1;
    MorphSnapProcessor processor2;

    processor1.prepareToPlay(48000.0, 256);
    processor2.prepareToPlay(48000.0, 256);

    // Set some state
    processor1.morphX.store(0.75f);
    processor1.morphY.store(0.25f);
    processor1.faderPos.store(0.5f);

    // Save state
    juce::MemoryBlock state;
    processor1.getStateInformation(state);

    // Load into second processor
    processor2.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    // Verify state loaded
    ASSERT_EQ(processor2.morphX.load(), 0.75f);
    ASSERT_EQ(processor2.morphY.load(), 0.25f);
    ASSERT_EQ(processor2.faderPos.load(), 0.5f);

    processor1.releaseResources();
    processor2.releaseResources();
}

TEST(plugin_latency_report)
{
    MorphSnapProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    // Plugin should report zero latency
    ASSERT_EQ(processor.getLatencySamples(), 0);

    processor.releaseResources();
}

TEST(plugin_sample_rate_changes)
{
    MorphSnapProcessor processor;

    // Test different sample rates
    processor.prepareToPlay(44100.0, 256);
    ASSERT_EQ(processor.getSampleRate(), 44100.0);

    processor.releaseResources();
    processor.prepareToPlay(48000.0, 512);
    ASSERT_EQ(processor.getSampleRate(), 48000.0);

    processor.releaseResources();
    processor.prepareToPlay(96000.0, 128);
    ASSERT_EQ(processor.getSampleRate(), 96000.0);

    processor.releaseResources();
}

// ── Main Test Runner ───────────────────────────────────────────────────────────

int runIntegrationTests()
{
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "           MORPHSNAP MCP INTEGRATION TESTS\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n\n";

    // MCP Server Tests
    std::cout << "--- MCP Server Tests ---\n";
    RUN_TEST(mcp_server_starts);
    RUN_TEST(mcp_server_accepts_connection);
    RUN_TEST(mcp_initialize_request);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_morph_tool);
    RUN_TEST(mcp_get_state);
    RUN_TEST(mcp_invalid_request);
    RUN_TEST(mcp_multiple_connections);

    std::cout << "\n--- State Persistence Tests ---\n";
    RUN_TEST(plugin_state_save_load);
    RUN_TEST(plugin_latency_report);
    RUN_TEST(plugin_sample_rate_changes);

    // Summary
    std::cout << "\n═══════════════════════════════════════════════════════════════\n";
    std::cout << "RESULTS: " << testsPassed << "/" << testsRun << " tests passed\n";
    if (testsFailed > 0)
        std::cout << "         " << testsFailed << " FAILED\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";

    return testsFailed > 0 ? 1 : 0;
}

} // namespace test
} // namespace morphsnap

int main()
{
    return morphsnap::test::runIntegrationTests();
}
