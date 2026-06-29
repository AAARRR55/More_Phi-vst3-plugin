// src/AI/Orchestrator/McpProtocol.cpp
#include "AI/Orchestrator/McpProtocol.h"

#include <juce_core/juce_core.h>

namespace more_phi::mcp {

bool validateRequest(const McpRequest& req, McpError& outError)
{
    if (req.jsonrpc != "2.0")
    {
        outError = { -32600, "Invalid Request: jsonrpc must be \"2.0\"" };
        return false;
    }
    if (req.method.empty())
    {
        outError = { -32600, "Invalid Request: method must be a non-empty string" };
        return false;
    }
    return true;
}

bool validateAuth(const juce::String& token, const juce::String& expectedToken)
{
    if (token.length() != expectedToken.length())
        return false;

    const char* a = token.toRawUTF8();
    const char* b = expectedToken.toRawUTF8();
    volatile uint8_t diff = 0;
    for (int i = 0; i < expectedToken.length(); ++i)
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

juce::String serializeResponse(const McpResponse& resp)
{
    nlohmann::json j;
    j["jsonrpc"] = resp.jsonrpc;
    j["id"] = resp.id;
    if (resp.error.has_value())
    {
        nlohmann::json errObj;
        errObj["code"] = resp.error->code;
        errObj["message"] = resp.error->message;
        if (!resp.error->data.is_null())
            errObj["data"] = resp.error->data;
        j["error"] = errObj;
    }
    else
    {
        j["result"] = resp.result;
    }
    return juce::String(j.dump());
}

std::optional<McpRequest> parseRequest(const juce::String& jsonStr, McpError& outError)
{
    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(jsonStr.toStdString());
    }
    catch (const nlohmann::json::parse_error& e)
    {
        outError = { -32700, std::string("Parse error: ") + e.what() };
        return std::nullopt;
    }

    if (!parsed.is_object())
    {
        outError = { -32600, "Invalid Request" };
        return std::nullopt;
    }

    McpRequest req;
    req.jsonrpc = parsed.value("jsonrpc", "");

    if (parsed.contains("id"))
        req.id = parsed["id"];
    else
        req.id = nullptr;

    req.method = parsed.value("method", "");

    if (parsed.contains("params"))
        req.params = parsed["params"];
    else
        req.params = nlohmann::json::object();

    return req;
}

McpResponse makeErrorResponse(const nlohmann::json& id,
                              int code,
                              const std::string& message,
                              const nlohmann::json& data)
{
    McpResponse resp;
    resp.jsonrpc = "2.0";
    resp.id = id;
    resp.error = McpError{ code, message, data };
    return resp;
}

} // namespace more_phi::mcp
