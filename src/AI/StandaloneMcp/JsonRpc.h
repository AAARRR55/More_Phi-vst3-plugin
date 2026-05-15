#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace more_phi::standalone_mcp::JsonRpc {

inline nlohmann::json makeResult(const nlohmann::json& id, const nlohmann::json& result)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

inline nlohmann::json makeError(const nlohmann::json& id, int code, const std::string& message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", code},
            {"message", message}
        }}
    };
}

inline nlohmann::json makeToolResult(const nlohmann::json& id,
                                     const nlohmann::json& structuredContent,
                                     bool isError)
{
    const auto text = isError && structuredContent.contains("error")
        ? structuredContent["error"].get<std::string>()
        : structuredContent.dump();

    return makeResult(id, {
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", text}}
        })},
        {"structuredContent", structuredContent},
        {"isError", isError}
    });
}

inline std::string serializeLine(const nlohmann::json& message)
{
    return message.dump() + "\n";
}

} // namespace more_phi::standalone_mcp::JsonRpc
