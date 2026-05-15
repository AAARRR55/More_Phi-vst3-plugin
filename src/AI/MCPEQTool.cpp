/*
 * More-Phi — AI/MCPEQTool.cpp
 */
#include "MCPEQTool.h"
#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>

namespace more_phi {

namespace {

juce::String toJString(const nlohmann::json& json)
{
    return juce::String(json.dump());
}

} // namespace

MCPToolResult MCPEQTool::makeResult(bool success, const juce::String& message)
{
    return {
        success,
        message,
        toJString({
            {"success", success},
            {"message", message.toStdString()}
        })
    };
}

MCPToolResult MCPEQTool::adjustEQ(const juce::var&, MorePhiProcessor&, AIAssistant&)
{
    return makeResult(true, "no EQ adjustment staged");
}

MCPToolResult MCPEQTool::previewEQ(const juce::var&, MorePhiProcessor&, AIAssistant& assistant)
{
    juce::String error;
    if (!assistant.applyPreview(&error))
        return makeResult(false, error.isNotEmpty() ? error : "preview failed");

    return makeResult(true, "preview applied");
}

MCPToolResult MCPEQTool::applyEQ(const juce::var&, MorePhiProcessor& processor)
{
    processor.getAIAssistant().commitPreview();
    return makeResult(true, "preview committed");
}

MCPToolResult MCPEQTool::rejectEQ(const juce::var&, MorePhiProcessor& processor)
{
    processor.getAIAssistant().rejectPreview();
    return makeResult(true, "preview rejected");
}

MCPToolResult MCPEQTool::getContext(const juce::var&, MorePhiProcessor& processor)
{
    return {
        true,
        "context",
        toJString({
            {"success", true},
            {"pendingChanges", static_cast<int>(processor.getAIAssistant().getPendingChanges().size())},
            {"previewActive", processor.getAIAssistant().isPreviewActive()}
        })
    };
}

MCPToolResult MCPEQTool::resetContext(const juce::var&, MorePhiProcessor& processor)
{
    processor.getAIAssistant().clearPendingChanges();
    return makeResult(true, "context reset");
}

MCPToolResult MCPEQTool::validateEQ(const juce::var&, MorePhiProcessor&)
{
    return makeResult(true, "valid");
}

MCPToolResult MCPEQTool::suggestEQ(const juce::var&, MorePhiProcessor&, AIAssistant&)
{
    return makeResult(true, "no suggestion staged");
}

} // namespace more_phi
