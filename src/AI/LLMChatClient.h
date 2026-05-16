/*
 * More-Phi — AI/LLMChatClient.h
 * Runs an LLM chat-with-tools agent loop against the configured provider.
 * Tool calls are executed in-process via MCPToolHandler (no TCP round-trip).
 */
#pragma once

#include "LLMConnectionValidator.h" // ILLMHttpClient, LLMHttpRequest/Response
#include "LLMSettings.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

/**
 * Async LLM chat client with MCP tool loop.
 *
 * Usage:
 *   client.chat(settings, historyJson, userMessage,
 *               [](text, error, updatedHistoryJson) { ... });
 *
 * The callback is always called on the JUCE message thread.
 * historyJson is a JSON array of OpenAI-style role/content objects.
 * Pass "{}" or "" to start a fresh conversation.
 */
class LLMChatClient final
{
public:
    /** text, errorMessage, updatedHistoryJson */
    using ReplyCallback = std::function<void(juce::String, juce::String, juce::String)>;

    explicit LLMChatClient(MorePhiProcessor& processor);
    LLMChatClient(MorePhiProcessor& processor, std::shared_ptr<ILLMHttpClient> httpClient);

    /**
     * Send userMessage, run the tool agent loop, and call back with the final
     * assistant text and the updated conversation history (both on message thread).
     */
    void chat(const LLMSettings& settings,
              const juce::String& historyJson,
              const juce::String& userMessage,
              ReplyCallback callback);

    // ── Visible for testing ────────────────────────────────────────────────
    /** Convert MCPToolHandler::getToolList() JSON into OpenAI tools array JSON. */
    static juce::String mcpToolsToOpenAIJson();

    /** Convert MCPToolHandler::getToolList() JSON into Anthropic tools array JSON. */
    static juce::String mcpToolsToAnthropicJson();

private:
    struct ToolCall
    {
        juce::String id;
        juce::String name;
        juce::String argumentsJson; // raw JSON string
    };

    struct ParsedResponse
    {
        juce::String    textContent;
        std::vector<ToolCall> toolCalls;
        juce::String    errorMessage;
    };

    // ── Request building ───────────────────────────────────────────────────
    static juce::String buildOpenAIRequestBody(const LLMProviderSettings& ps,
                                               const juce::String& model,
                                               const std::string& messagesJson,
                                               const juce::String& toolsJson);

    static juce::String buildAnthropicRequestBody(const LLMProviderSettings& ps,
                                                   const juce::String& model,
                                                   const std::string& messagesJson,
                                                   const juce::String& toolsJson);

    static LLMHttpRequest buildHttpRequest(LLMProviderId id,
                                           const LLMProviderSettings& ps,
                                           const juce::String& body);

    // ── Response parsing ───────────────────────────────────────────────────
    static ParsedResponse parseOpenAIResponse(int statusCode, const juce::String& body);
    static ParsedResponse parseAnthropicResponse(int statusCode, const juce::String& body);

    // ── Anthropic message format conversion ────────────────────────────────
    /** Convert OpenAI-style message array (nlohmann::json) to Anthropic format.
     *  Returns {anthropicMessages, systemPromptText}. */
    static std::pair<std::string, std::string>
    convertToAnthropicMessages(const std::string& openAIMessagesJson);

    // ── In-process tool execution ──────────────────────────────────────────
    juce::String executeTool(const juce::String& name, const juce::String& argumentsJson);

    // ── Helpers ────────────────────────────────────────────────────────────
    static juce::String baseUrlFor(LLMProviderId id, const LLMProviderSettings& ps);
    static juce::String authHeadersFor(LLMProviderId id, const juce::String& apiKey);

    MorePhiProcessor&               processor_;
    std::shared_ptr<ILLMHttpClient> httpClient_;

    static constexpr int kMaxToolIterations = 8;
    static constexpr int kMaxTokens         = 4096;
    static constexpr int kTimeoutMs         = 60000;

    static const char* const kSystemPrompt;
};

} // namespace more_phi
