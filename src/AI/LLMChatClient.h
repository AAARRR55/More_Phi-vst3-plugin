/*
 * More-Phi — AI/LLMChatClient.h
 * Runs an LLM chat-with-tools agent loop against the configured provider.
 * Tool calls are executed in-process via MCPToolHandler (no TCP round-trip).
 */
#pragma once

#include "AutomationControlPlane.h"
#include "LLMConnectionValidator.h" // ILLMHttpClient, LLMHttpRequest/Response
#include "LLMSettings.h"

#include <nlohmann/json_fwd.hpp>

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

    /** iteration (1-based), maxIterations, status description */
    using ProgressCallback = std::function<void(int, int, juce::String)>;

    explicit LLMChatClient(MorePhiProcessor& processor);
    LLMChatClient(MorePhiProcessor& processor, std::shared_ptr<ILLMHttpClient> httpClient);
    ~LLMChatClient();

    /**
     * Send userMessage, run the tool agent loop, and call back with the final
     * assistant text and the updated conversation history (both on message thread).
     */
    void chat(const LLMSettings& settings,
              const juce::String& historyJson,
              const juce::String& userMessage,
              ReplyCallback callback,
              ProgressCallback progress = nullptr);

    // ── Visible for testing ────────────────────────────────────────────────
    /** Convert MCPToolHandler::getToolList() JSON into OpenAI tools array JSON. */
    static juce::String mcpToolsToOpenAIJson();

    /** Convert MCPToolHandler::getToolList() JSON into Anthropic tools array JSON. */
    static juce::String mcpToolsToAnthropicJson();

    /** Convert MCPToolHandler::getToolList() JSON into Gemini functionDeclarations JSON. */
    static juce::String mcpToolsToGeminiJson();

    /** Resolve an LLM API-safe tool alias back to the MCP tool name. */
    static juce::String resolveToolNameForTest(const juce::String& apiToolName);

    /** Return the active system prompt used by the chat agent. */
    static juce::String systemPromptForTest();

    /** Return the chat-filtered OpenAI tools array JSON (reduced surface). */
    static juce::String chatToolsOpenAIJsonForTest();

    /** Return the chat-filtered Anthropic tools array JSON (reduced surface). */
    static juce::String chatToolsAnthropicJsonForTest();

    /** Parse an OpenAI-format response body and return extracted tool calls as JSON.
     *  Exposed for testing the NVIDIA inline-token fallback parser. */
    static juce::String parseOpenAIResponseForTest(int statusCode, const juce::String& body);

    /** Parse a Gemini-format response body and return extracted text/tool calls as JSON.
     *  Exposed for testing. */
    static juce::String parseGeminiResponseForTest(int statusCode, const juce::String& body);

    /** Return the max_tokens budget for a given model id. Reasoning models
     *  (DeepSeek-R1, Nemotron, QwQ, ...) need a larger budget because their
     *  reasoning tokens count against it; without it they exhaust the budget
     *  before producing a final answer. Other models keep the default cap. */
    static int maxTokensFor(const juce::String& model);

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
                                               const nlohmann::json& toolsArray);

    static juce::String buildAnthropicRequestBody(const LLMProviderSettings& ps,
                                                   const juce::String& model,
                                                   const std::string& messagesJson,
                                                   const nlohmann::json& toolsArray);

    static juce::String buildGeminiRequestBody(const LLMProviderSettings& ps,
                                                const juce::String& model,
                                                const std::string& messagesJson,
                                                const nlohmann::json& toolsArray);

    static LLMHttpRequest buildHttpRequest(LLMProviderId id,
                                           const LLMProviderSettings& ps,
                                           const juce::String& body);

    // ── Response parsing ───────────────────────────────────────────────────
    static ParsedResponse parseOpenAIResponse(int statusCode, const juce::String& body);
    static ParsedResponse parseAnthropicResponse(int statusCode, const juce::String& body);
    static ParsedResponse parseGeminiResponse(int statusCode, const juce::String& body);

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
    mutable AutomationRuntime       automationRuntime_;
    std::shared_ptr<bool>           alive_ { std::make_shared<bool>(true) };

    static constexpr int kMaxToolIterations  = 8;
    static constexpr int kMaxTokens          = 4096;
    static constexpr int kMaxTokensReasoning = 16384; // reasoning models (CoT counts vs budget)
    static constexpr int kTimeoutMs          = 60000;   // default (OpenAI, Anthropic)
    static constexpr int kTimeoutMsNvidia    = 120000;  // NVIDIA NIM cold-start (reduced from 300s)
    static constexpr int kAgentLoopTimeoutMs = 90000;   // overall agent loop budget

    static const char* const kSystemPrompt;
};

} // namespace more_phi
