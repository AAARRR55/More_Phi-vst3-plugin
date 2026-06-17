/*
 * More-Phi — AI/AIAssistant.h
 * Minimal assistant edit-state bridge used by MCP EQ tooling.
 */
#pragma once

#include "AutomationControlPlane.h"
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

struct ParamChange
{
    int paramIndex = -1;
    int index = -1;
    juce::String stableId;
    juce::String name;
    float currentValue = 0.0f;
    float newValue = 0.0f;
};

struct AssistantWorkflowPlan
{
    bool handled = false;
    bool valid = false;
    juce::String summary;
    juce::String error;
    nlohmann::json preview = nlohmann::json::object();
    nlohmann::json workflowSubmitParams = nlohmann::json::object();
};

struct AssistantWorkflowResult
{
    bool handled = false;
    bool success = false;
    bool awaitingApproval = false;
    juce::String message;
    juce::String workflowRunId;
    juce::String approvalId;
    juce::String transactionId;
    juce::String rawResponse;
    nlohmann::json preview = nlohmann::json::object();
    nlohmann::json response = nlohmann::json::object();
};

class AIAssistant
{
public:
    explicit AIAssistant(MorePhiProcessor& processor);
    ~AIAssistant();

    void stagePendingChanges(std::vector<ParamChange> changes);
    const std::vector<ParamChange>& getPendingChanges() const noexcept { return pendingChanges_; }
    void clearPendingChanges();

    bool applyPreview(juce::String* errorMessage = nullptr);
    void rejectPreview();
    void commitPreview();
    bool isPreviewActive() const noexcept { return previewActive_; }

    AssistantWorkflowPlan planLocalWorkflowPrompt(const juce::String& text) const;
    AssistantWorkflowResult executeLocalWorkflowPrompt(const juce::String& text);
    AssistantWorkflowResult undoLastAssistantWorkflow();
    AssistantWorkflowResult recordFeedbackForLastWorkflow(const juce::String& text);

    /** Exposes the assistant's persistent AutomationRuntime so MCP tool tests
     *  can target the same memory/event bus used by executeLocalWorkflowPrompt(). */
    AutomationRuntime& getAutomationRuntime() const noexcept { return automationRuntime_; }

    static bool detectsLocalWorkflowPrompt(const juce::String& text);
    static bool detectsLocalFeedbackPrompt(const juce::String& text);
    static bool detectsLocalUndoPrompt(const juce::String& text);

private:
    static int canonicalIndex(const ParamChange& change) noexcept;

    MorePhiProcessor& processor_;
    std::vector<ParamChange> pendingChanges_;
    bool previewActive_ = false;
    juce::String lastWorkflowRunId_;
    juce::String lastRollbackTransactionId_;
    mutable AutomationRuntime automationRuntime_;
};

} // namespace more_phi
