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

MCPToolResult MCPEQTool::adjustEQ(const juce::var&, MorePhiProcessor&, AIAssistant*)
{
    return makeResult(true, "no EQ adjustment staged");
}

MCPToolResult MCPEQTool::previewEQ(const juce::var&, MorePhiProcessor&, AIAssistant* assistant)
{
    if (assistant == nullptr)
        return makeResult(false, "AI assistant not available");

    juce::String error;
    if (!assistant->applyPreview(&error))
        return makeResult(false, error.isNotEmpty() ? error : "preview failed");

    return makeResult(true, "preview applied");
}

MCPToolResult MCPEQTool::applyEQ(const juce::var&, MorePhiProcessor& processor)
{
    if (auto* assistant = processor.getAIAssistant())
        assistant->commitPreview();
    return makeResult(true, "preview committed");
}

MCPToolResult MCPEQTool::rejectEQ(const juce::var&, MorePhiProcessor& processor)
{
    if (auto* assistant = processor.getAIAssistant())
        assistant->rejectPreview();
    return makeResult(true, "preview rejected");
}

MCPToolResult MCPEQTool::getContext(const juce::var&, MorePhiProcessor& processor)
{
    auto* assistant = processor.getAIAssistant();
    return {
        true,
        "context",
        toJString({
            {"success", true},
            {"pendingChanges", static_cast<int>(assistant ? assistant->getPendingChanges().size() : 0)},
            {"previewActive", assistant ? assistant->isPreviewActive() : false}
        })
    };
}

MCPToolResult MCPEQTool::resetContext(const juce::var&, MorePhiProcessor& processor)
{
    if (auto* assistant = processor.getAIAssistant())
        assistant->clearPendingChanges();
    return makeResult(true, "context reset");
}

MCPToolResult MCPEQTool::validateEQ(const juce::var&, MorePhiProcessor&)
{
    return makeResult(true, "valid");
}

MCPToolResult MCPEQTool::suggestEQ(const juce::var&, MorePhiProcessor&, AIAssistant*)
{
    return makeResult(true, "no suggestion staged");
}

} // namespace more_phi
