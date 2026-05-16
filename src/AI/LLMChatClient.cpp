/*
 * More-Phi — AI/LLMChatClient.cpp
 * LLM chat agent loop with in-process MCP tool execution.
 */
#include "LLMChatClient.h"
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>
#include <juce_events/juce_events.h>
#include <thread>

namespace more_phi {

using json = nlohmann::json;

// ── System prompt ──────────────────────────────────────────────────────────

const char* const LLMChatClient::kSystemPrompt =
    "You are an AI assistant embedded in More-Phi, a VST3 parameter morphing plugin. "
    "You can inspect and control the hosted plugin's parameters and More-Phi's morphing "
    "engine using the available tools. "
    "Be concise and precise. Call list_parameters before setting any parameter so you "
    "know the available controls and their valid range (0.0–1.0 normalized). "
    "Confirm with the user before making large or irreversible changes. "
    "When reporting parameter values, include the parameter name and normalized value.";

// ── Construction ───────────────────────────────────────────────────────────

LLMChatClient::LLMChatClient(MorePhiProcessor& processor)
    : processor_(processor)
    , httpClient_(std::make_shared<JuceLLMHttpClient>())
{
}

LLMChatClient::LLMChatClient(MorePhiProcessor& processor,
                               std::shared_ptr<ILLMHttpClient> httpClient)
    : processor_(processor)
    , httpClient_(std::move(httpClient))
{
}

// ── Tool list conversion ───────────────────────────────────────────────────

juce::String LLMChatClient::mcpToolsToOpenAIJson()
{
    try
    {
        const auto toolListStr = MCPToolHandler::getToolList();
        const auto root = json::parse(toolListStr.toStdString());
        if (!root.contains("tools") || !root["tools"].is_array())
            return "[]";

        json openAITools = json::array();
        for (const auto& tool : root["tools"])
        {
            const auto name        = tool.value("name", "");
            const auto description = tool.value("description", "");
            const auto inputSchema = tool.contains("inputSchema") ? tool["inputSchema"]
                                                                    : json{{"type", "object"},
                                                                            {"properties", json::object()}};
            openAITools.push_back({
                {"type", "function"},
                {"function",
                 {{"name", name},
                  {"description", description},
                  {"parameters", inputSchema}}}
            });
        }
        return juce::String(openAITools.dump());
    }
    catch (...)
    {
        return "[]";
    }
}

juce::String LLMChatClient::mcpToolsToAnthropicJson()
{
    try
    {
        const auto toolListStr = MCPToolHandler::getToolList();
        const auto root = json::parse(toolListStr.toStdString());
        if (!root.contains("tools") || !root["tools"].is_array())
            return "[]";

        json anthropicTools = json::array();
        for (const auto& tool : root["tools"])
        {
            const auto name        = tool.value("name", "");
            const auto description = tool.value("description", "");
            const auto inputSchema = tool.contains("inputSchema") ? tool["inputSchema"]
                                                                    : json{{"type", "object"},
                                                                            {"properties", json::object()}};
            anthropicTools.push_back({
                {"name", name},
                {"description", description},
                {"input_schema", inputSchema}
            });
        }
        return juce::String(anthropicTools.dump());
    }
    catch (...)
    {
        return "[]";
    }
}

// ── Anthropic message conversion ───────────────────────────────────────────

std::pair<std::string, std::string>
LLMChatClient::convertToAnthropicMessages(const std::string& openAIMessagesJson)
{
    std::string systemText;
    json anthropicMessages = json::array();

    json messages;
    try
    {
        messages = json::parse(openAIMessagesJson);
    }
    catch (...)
    {
        return {"[]", ""};
    }

    std::size_t i = 0;
    while (i < messages.size())
    {
        const auto& msg  = messages[i];
        const auto  role = msg.value("role", "");

        if (role == "system")
        {
            if (msg.contains("content") && msg["content"].is_string())
            {
                if (!systemText.empty()) systemText += "\n";
                systemText += msg["content"].get<std::string>();
            }
            ++i;
        }
        else if (role == "user")
        {
            // Plain user text message — check content type
            const auto& content = msg["content"];
            if (content.is_string())
                anthropicMessages.push_back({{"role", "user"}, {"content", content}});
            else
                anthropicMessages.push_back({{"role", "user"}, {"content", content}});
            ++i;
        }
        else if (role == "assistant")
        {
            // Does it have tool calls?
            bool hasToolCalls = msg.contains("tool_calls")
                                && msg["tool_calls"].is_array()
                                && !msg["tool_calls"].empty();

            json contentBlocks = json::array();

            // Any text content first
            if (msg.contains("content") && msg["content"].is_string()
                && !msg["content"].get<std::string>().empty())
            {
                contentBlocks.push_back({{"type", "text"}, {"text", msg["content"]}});
            }

            if (hasToolCalls)
            {
                for (const auto& tc : msg["tool_calls"])
                {
                    json input;
                    try
                    {
                        const auto args = tc["function"]["arguments"].get<std::string>();
                        input = args.empty() ? json::object() : json::parse(args);
                    }
                    catch (...)
                    {
                        input = json::object();
                    }
                    contentBlocks.push_back({
                        {"type", "tool_use"},
                        {"id",   tc.value("id", "")},
                        {"name", tc["function"].value("name", "")},
                        {"input", input}
                    });
                }
            }

            if (contentBlocks.empty())
                contentBlocks.push_back({{"type", "text"}, {"text", ""}});

            anthropicMessages.push_back({{"role", "assistant"}, {"content", contentBlocks}});
            ++i;
        }
        else if (role == "tool")
        {
            // Collect consecutive tool results into a single user message
            json toolResults = json::array();
            while (i < messages.size() && messages[i].value("role", "") == "tool")
            {
                const auto& t = messages[i];
                toolResults.push_back({
                    {"type",        "tool_result"},
                    {"tool_use_id", t.value("tool_call_id", "")},
                    {"content",     t.contains("content") && t["content"].is_string()
                                        ? t["content"].get<std::string>()
                                        : t["content"].dump()}
                });
                ++i;
            }
            anthropicMessages.push_back({{"role", "user"}, {"content", toolResults}});
        }
        else
        {
            ++i;
        }
    }

    return {anthropicMessages.dump(), systemText};
}

// ── HTTP helpers ───────────────────────────────────────────────────────────

juce::String LLMChatClient::baseUrlFor(LLMProviderId id, const LLMProviderSettings& ps)
{
    const auto& def = getLLMProviderDefinition(id);
    auto base = def.customBaseUrlAllowed ? ps.customBaseUrl.trim() : def.fixedBaseUrl;
    return base.trimCharactersAtEnd("/");
}

juce::String LLMChatClient::authHeadersFor(LLMProviderId id, const juce::String& apiKey)
{
    if (id == LLMProviderId::Anthropic)
        return "x-api-key: " + apiKey
               + "\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n";

    return "Authorization: Bearer " + apiKey + "\r\ncontent-type: application/json\r\n";
}

LLMHttpRequest LLMChatClient::buildHttpRequest(LLMProviderId id,
                                                const LLMProviderSettings& ps,
                                                const juce::String& body)
{
    const auto base    = baseUrlFor(id, ps);
    const auto path    = (id == LLMProviderId::Anthropic) ? "/v1/messages" : "/chat/completions";
    const auto headers = authHeadersFor(id, ps.apiKey.trim());
    return {LLMHttpMethod::Post, base + path, headers, body, kTimeoutMs};
}

// ── Request body builders ──────────────────────────────────────────────────

juce::String LLMChatClient::buildOpenAIRequestBody(const LLMProviderSettings& /*ps*/,
                                                    const juce::String& model,
                                                    const std::string& messagesJson,
                                                    const juce::String& toolsJson)
{
    try
    {
        json body;
        body["model"]      = model.toStdString();
        body["max_tokens"] = kMaxTokens;
        body["messages"]   = json::parse(messagesJson);

        const auto tools = json::parse(toolsJson.toStdString());
        if (!tools.empty())
        {
            body["tools"]       = tools;
            body["tool_choice"] = "auto";
        }

        return juce::String(body.dump());
    }
    catch (...)
    {
        return "{}";
    }
}

juce::String LLMChatClient::buildAnthropicRequestBody(const LLMProviderSettings& /*ps*/,
                                                        const juce::String& model,
                                                        const std::string& messagesJson,
                                                        const juce::String& toolsJson)
{
    try
    {
        auto [anthropicMsgsJson, systemText] = convertToAnthropicMessages(messagesJson);

        json body;
        body["model"]      = model.toStdString();
        body["max_tokens"] = kMaxTokens;
        body["messages"]   = json::parse(anthropicMsgsJson);

        if (!systemText.empty())
            body["system"] = systemText;

        const auto tools = json::parse(toolsJson.toStdString());
        if (!tools.empty())
            body["tools"] = tools;

        return juce::String(body.dump());
    }
    catch (...)
    {
        return "{}";
    }
}

// ── Response parsers ───────────────────────────────────────────────────────

LLMChatClient::ParsedResponse
LLMChatClient::parseOpenAIResponse(int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return {{}, {}, "Authentication failed. Check your API key in LLM Settings."};
    if (statusCode <= 0)
        return {{}, {}, "Network timeout or connection refused."};
    if (statusCode < 200 || statusCode >= 300)
    {
        juce::String detail;
        try
        {
            const auto root = json::parse(body.toStdString());
            if (root.contains("error") && root["error"].is_object())
                detail = juce::String(root["error"].value("message", ""));
        }
        catch (...) {}
        return {{}, {}, "LLM request failed (HTTP " + juce::String(statusCode) + ")"
                             + (detail.isEmpty() ? "" : ": " + detail)};
    }

    try
    {
        const auto root = json::parse(body.toStdString());
        if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty())
            return {{}, {}, "No choices in LLM response."};

        const auto& choice = root["choices"].front();
        if (!choice.contains("message") || !choice["message"].is_object())
            return {{}, {}, "Missing message in LLM response."};

        const auto& message = choice["message"];
        juce::String textContent;
        if (message.contains("content") && message["content"].is_string())
            textContent = juce::String(message["content"].get<std::string>());

        std::vector<ToolCall> toolCalls;
        if (message.contains("tool_calls") && message["tool_calls"].is_array())
        {
            for (const auto& tc : message["tool_calls"])
            {
                ToolCall call;
                call.id   = juce::String(tc.value("id", ""));
                if (tc.contains("function") && tc["function"].is_object())
                {
                    call.name         = juce::String(tc["function"].value("name", ""));
                    call.argumentsJson = juce::String(tc["function"].value("arguments", "{}"));
                }
                if (!call.name.isEmpty())
                    toolCalls.push_back(std::move(call));
            }
        }

        return {textContent, std::move(toolCalls), {}};
    }
    catch (const std::exception& e)
    {
        return {{}, {}, "Failed to parse LLM response: " + juce::String(e.what())};
    }
}

LLMChatClient::ParsedResponse
LLMChatClient::parseAnthropicResponse(int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return {{}, {}, "Authentication failed. Check your API key in LLM Settings."};
    if (statusCode <= 0)
        return {{}, {}, "Network timeout or connection refused."};
    if (statusCode < 200 || statusCode >= 300)
    {
        juce::String detail;
        try
        {
            const auto root = json::parse(body.toStdString());
            if (root.contains("error") && root["error"].is_object())
                detail = juce::String(root["error"].value("message", ""));
        }
        catch (...) {}
        return {{}, {}, "LLM request failed (HTTP " + juce::String(statusCode) + ")"
                             + (detail.isEmpty() ? "" : ": " + detail)};
    }

    try
    {
        const auto root = json::parse(body.toStdString());
        if (!root.contains("content") || !root["content"].is_array())
            return {{}, {}, "No content in Anthropic response."};

        juce::String textContent;
        std::vector<ToolCall> toolCalls;

        for (const auto& block : root["content"])
        {
            if (!block.is_object()) continue;
            const auto type = block.value("type", "");

            if (type == "text")
            {
                textContent += juce::String(block.value("text", ""));
            }
            else if (type == "tool_use")
            {
                ToolCall call;
                call.id   = juce::String(block.value("id", ""));
                call.name = juce::String(block.value("name", ""));
                if (block.contains("input") && !block["input"].is_null())
                    call.argumentsJson = juce::String(block["input"].dump());
                else
                    call.argumentsJson = "{}";

                if (!call.name.isEmpty())
                    toolCalls.push_back(std::move(call));
            }
        }

        return {textContent, std::move(toolCalls), {}};
    }
    catch (const std::exception& e)
    {
        return {{}, {}, "Failed to parse Anthropic response: " + juce::String(e.what())};
    }
}

// ── In-process tool execution ──────────────────────────────────────────────

juce::String LLMChatClient::executeTool(const juce::String& name,
                                         const juce::String& argumentsJson)
{
    // Parse arguments into juce::var so MCPToolHandler can consume them
    juce::var params;
    if (argumentsJson.isNotEmpty())
    {
        juce::var parsed = juce::JSON::parse(argumentsJson);
        if (!parsed.isUndefined() && !parsed.isVoid())
            params = parsed;
    }

    // Use the processor's instance identity for auth/identity context
    const auto& identity = processor_.getInstanceIdentity();

    try
    {
        return MCPToolHandler::handle(name, params, processor_, identity);
    }
    catch (const std::exception& e)
    {
        return juce::String("{\"error\":\"Tool execution failed: ") + e.what() + "\"}";
    }
    catch (...)
    {
        return "{\"error\":\"Tool execution failed with unknown exception\"}";
    }
}

// ── Main chat entry point ──────────────────────────────────────────────────

void LLMChatClient::chat(const LLMSettings& settings,
                          const juce::String& historyJson,
                          const juce::String& userMessage,
                          ReplyCallback callback)
{
    // Validate provider configuration before spawning a thread
    if (!settings.activeProvider.has_value())
    {
        juce::MessageManager::callAsync([callback, historyJson]()
        {
            callback({},
                     "No LLM provider configured. Open \u2699 LLM Settings and configure a provider.",
                     historyJson);
        });
        return;
    }

    const auto providerId      = *settings.activeProvider;
    const auto providerSettings = settings.getProvider(providerId);
    const auto model            = providerSettings.selectedModel.trim();

    if (providerSettings.apiKey.trim().isEmpty())
    {
        juce::MessageManager::callAsync([callback, historyJson]()
        {
            callback({}, "No API key configured. Open \u2699 LLM Settings.", historyJson);
        });
        return;
    }
    if (model.isEmpty())
    {
        juce::MessageManager::callAsync([callback, historyJson]()
        {
            callback({}, "No model selected. Open \u2699 LLM Settings and choose a model.", historyJson);
        });
        return;
    }

    // Prepare tool lists once
    const bool isAnthropic  = (providerId == LLMProviderId::Anthropic);
    const juce::String tools = isAnthropic ? mcpToolsToAnthropicJson() : mcpToolsToOpenAIJson();

    // Capture everything the background thread needs
    std::thread([this,
                 providerId,
                 providerSettings,
                 model,
                 historyJson,
                 userMessage,
                 tools,
                 isAnthropic,
                 callback]() mutable
    {
        // ── Parse / initialise message history ──────────────────────────
        json messages = json::array();
        if (historyJson.isNotEmpty())
        {
            try { messages = json::parse(historyJson.toStdString()); }
            catch (...) { messages = json::array(); }
        }

        // Insert system prompt if this is the first turn
        if (messages.empty())
            messages.push_back({{"role", "system"}, {"content", kSystemPrompt}});

        // Append user message
        messages.push_back({{"role", "user"}, {"content", userMessage.toStdString()}});

        juce::String finalText;
        juce::String errorText;

        // ── Agent loop ──────────────────────────────────────────────────
        for (int iteration = 0; iteration < kMaxToolIterations; ++iteration)
        {
            // Build request body
            const std::string messagesStr = messages.dump();
            const juce::String body =
                isAnthropic
                    ? buildAnthropicRequestBody(providerSettings, model, messagesStr, tools)
                    : buildOpenAIRequestBody   (providerSettings, model, messagesStr, tools);

            if (body == "{}")
            {
                errorText = "Failed to build LLM request.";
                break;
            }

            // Execute HTTP request
            const auto request  = buildHttpRequest(providerId, providerSettings, body);
            const auto response = httpClient_->execute(request);

            // Parse response
            const auto parsed = isAnthropic
                                     ? parseAnthropicResponse(response.statusCode, response.body)
                                     : parseOpenAIResponse(response.statusCode, response.body);

            if (!parsed.errorMessage.isEmpty())
            {
                errorText = parsed.errorMessage;
                break;
            }

            if (parsed.toolCalls.empty())
            {
                // Final text response — done
                finalText = parsed.textContent;
                // Append assistant reply to history
                messages.push_back({{"role", "assistant"}, {"content", finalText.toStdString()}});
                break;
            }

            // ── Has tool calls — append assistant message + execute tools ──
            json toolCallsJson = json::array();
            for (const auto& tc : parsed.toolCalls)
            {
                toolCallsJson.push_back({
                    {"id", tc.id.toStdString()},
                    {"type", "function"},
                    {"function",
                     {{"name", tc.name.toStdString()},
                      {"arguments", tc.argumentsJson.toStdString()}}}
                });
            }

            // Any text before tool calls becomes part of the assistant message
            const auto preText = parsed.textContent.toStdString();
            json assistantMsg  = {
                {"role", "assistant"},
                {"content", preText.empty() ? json(nullptr) : json(preText)},
                {"tool_calls", toolCallsJson}
            };
            messages.push_back(assistantMsg);

            // Execute each tool call and append results
            for (const auto& tc : parsed.toolCalls)
            {
                const auto result = executeTool(tc.name, tc.argumentsJson);
                messages.push_back({
                    {"role",         "tool"},
                    {"tool_call_id", tc.id.toStdString()},
                    {"content",      result.toStdString()}
                });
            }

            // If on last allowed iteration, note that
            if (iteration == kMaxToolIterations - 1)
            {
                finalText = "Reached the maximum tool-call limit. Partial work may have been applied.";
                messages.push_back({{"role", "assistant"}, {"content", finalText.toStdString()}});
            }
        }

        const juce::String updatedHistory(messages.dump());

        // ── Post result to message thread ───────────────────────────────
        juce::MessageManager::callAsync([callback,
                                          ft = finalText,
                                          et = errorText,
                                          uh = updatedHistory]() mutable
        {
            callback(ft, et, uh);
        });
    }).detach();
}

} // namespace more_phi
