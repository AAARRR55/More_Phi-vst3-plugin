// src/AI/Orchestrator/McpProtocol.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace more_phi::mcp {

/**
 * JSON-RPC 2.0 error object.
 */
struct McpError
{
    int code = 0;
    std::string message;
    nlohmann::json data = nlohmann::json::object();
};

/**
 * JSON-RPC 2.0 request object.
 */
struct McpRequest
{
    std::string jsonrpc;
    nlohmann::json id = nullptr;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

/**
 * JSON-RPC 2.0 response object.
 * Exactly one of 'result' or 'error' is present on the wire.
 */
struct McpResponse
{
    std::string jsonrpc;
    nlohmann::json id = nullptr;
    nlohmann::json result = nlohmann::json::object();
    std::optional<McpError> error;
};

/**
 * JSON-RPC 2.0 notification object (no id).
 */
struct McpNotification
{
    std::string jsonrpc;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

/**
 * Validate that a parsed request conforms to JSON-RPC 2.0 basics.
 * @return true if valid; outError is populated otherwise.
 */
bool validateRequest(const McpRequest& req, McpError& outError);

/**
 * Constant-time bearer-token comparison.
 * @return true if tokens are identical (fixed-length compare).
 */
bool validateAuth(const juce::String& token, const juce::String& expectedToken);

/**
 * Serialize an McpResponse to a JSON string.
 */
juce::String serializeResponse(const McpResponse& resp);

/**
 * Parse a JSON string into an McpRequest.
 * @return std::nullopt on parse failure; outError is populated.
 */
std::optional<McpRequest> parseRequest(const juce::String& jsonStr, McpError& outError);

/**
 * Helper to build a standard JSON-RPC error response.
 */
McpResponse makeErrorResponse(const nlohmann::json& id,
                              int code,
                              const std::string& message,
                              const nlohmann::json& data = nlohmann::json::object());

} // namespace more_phi::mcp
