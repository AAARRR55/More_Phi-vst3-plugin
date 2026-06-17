/*
 * More-Phi — AI/MCPEQTool.h
 * MCP-facing EQ assistant tool helpers.
 */
#pragma once

#include "AIAssistant.h"
#include <juce_core/juce_core.h>

namespace more_phi {

class MorePhiProcessor;

struct MCPToolResult
{
    bool success = false;
    juce::String message;
    juce::String jsonResult;
};

class MCPEQTool
{
public:
    static MCPToolResult adjustEQ(const juce::var& params, MorePhiProcessor& processor, AIAssistant* assistant);
    static MCPToolResult previewEQ(const juce::var& params, MorePhiProcessor& processor, AIAssistant* assistant);
    static MCPToolResult applyEQ(const juce::var& params, MorePhiProcessor& processor);
    static MCPToolResult rejectEQ(const juce::var& params, MorePhiProcessor& processor);
    static MCPToolResult getContext(const juce::var& params, MorePhiProcessor& processor);
    static MCPToolResult resetContext(const juce::var& params, MorePhiProcessor& processor);
    static MCPToolResult validateEQ(const juce::var& params, MorePhiProcessor& processor);
    static MCPToolResult suggestEQ(const juce::var& params, MorePhiProcessor& processor, AIAssistant* assistant);

private:
    static MCPToolResult makeResult(bool success, const juce::String& message);
};

} // namespace more_phi
