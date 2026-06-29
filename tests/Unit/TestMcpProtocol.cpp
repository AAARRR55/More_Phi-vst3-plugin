#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Orchestrator/McpProtocol.h"

using namespace more_phi::mcp;

TEST_CASE("parseRequest with valid JSON-RPC request", "[mcp][protocol]")
{
    McpError err;
    auto req = parseRequest(juce::String(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})"), err);
    REQUIRE(req.has_value());
    REQUIRE(req->jsonrpc == "2.0");
    REQUIRE(req->id == 1);
    REQUIRE(req->method == "tools/list");
    REQUIRE(req->params.is_object());
}

TEST_CASE("parseRequest with invalid JSON", "[mcp][protocol]")
{
    McpError err;
    auto req = parseRequest(juce::String("not json at all"), err);
    REQUIRE_FALSE(req.has_value());
    REQUIRE(err.code == -32700);
    REQUIRE(err.message.find("Parse error") != std::string::npos);
}

TEST_CASE("parseRequest with non-object input", "[mcp][protocol]")
{
    McpError err;
    auto req = parseRequest(juce::String("[1,2,3]"), err);
    REQUIRE_FALSE(req.has_value());
    REQUIRE(err.code == -32600);
    REQUIRE(err.message == "Invalid Request");
}

TEST_CASE("validateRequest with valid request", "[mcp][protocol]")
{
    McpRequest req;
    req.jsonrpc = "2.0";
    req.method = "tools/list";
    McpError err;
    REQUIRE(validateRequest(req, err));
    REQUIRE(err.code == 0);
}

TEST_CASE("validateRequest with missing/invalid jsonrpc", "[mcp][protocol]")
{
    McpRequest req;
    req.jsonrpc = "1.0";
    req.method = "tools/list";
    McpError err;
    REQUIRE_FALSE(validateRequest(req, err));
    REQUIRE(err.code == -32600);
    REQUIRE(err.message.find("jsonrpc") != std::string::npos);
}

TEST_CASE("validateRequest with empty method", "[mcp][protocol]")
{
    McpRequest req;
    req.jsonrpc = "2.0";
    req.method = "";
    McpError err;
    REQUIRE_FALSE(validateRequest(req, err));
    REQUIRE(err.code == -32600);
    REQUIRE(err.message.find("method") != std::string::npos);
}

TEST_CASE("validateAuth with matching tokens", "[mcp][protocol]")
{
    juce::String token = "secret-token-123";
    juce::String expected = "secret-token-123";
    REQUIRE(validateAuth(token, expected));
}

TEST_CASE("validateAuth with mismatched tokens", "[mcp][protocol]")
{
    juce::String token = "secret-token-123";
    juce::String expected = "secret-token-456";
    REQUIRE_FALSE(validateAuth(token, expected));
}

TEST_CASE("validateAuth with different-length tokens", "[mcp][protocol]")
{
    juce::String token = "short";
    juce::String expected = "much-longer-token";
    REQUIRE_FALSE(validateAuth(token, expected));
}

TEST_CASE("serializeResponse with result", "[mcp][protocol]")
{
    McpResponse resp;
    resp.jsonrpc = "2.0";
    resp.id = 42;
    resp.result = { { "tools", nlohmann::json::array() } };
    juce::String s = serializeResponse(resp);
    auto j = nlohmann::json::parse(s.toStdString());
    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["id"] == 42);
    REQUIRE(j.contains("result"));
    REQUIRE(j["result"].contains("tools"));
    REQUIRE_FALSE(j.contains("error"));
}

TEST_CASE("serializeResponse with error", "[mcp][protocol]")
{
    McpResponse resp;
    resp.jsonrpc = "2.0";
    resp.id = nullptr;
    resp.error = McpError{ -32600, "Invalid Request" };
    juce::String s = serializeResponse(resp);
    auto j = nlohmann::json::parse(s.toStdString());
    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j.contains("error"));
    REQUIRE(j["error"]["code"] == -32600);
    REQUIRE(j["error"]["message"] == "Invalid Request");
    REQUIRE_FALSE(j.contains("result"));
}

TEST_CASE("makeErrorResponse factory", "[mcp][protocol]")
{
    auto resp = makeErrorResponse(123, -32601, "Method not found", { { "method", "foo" } });
    REQUIRE(resp.jsonrpc == "2.0");
    REQUIRE(resp.id == 123);
    REQUIRE(resp.error.has_value());
    REQUIRE(resp.error->code == -32601);
    REQUIRE(resp.error->message == "Method not found");
    REQUIRE(resp.error->data["method"] == "foo");
}
