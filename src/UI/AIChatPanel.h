/*
 * More-Phi - UI/AIChatPanel.h
 * Assistant tab container.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AI/AIAssistant.h"
#include "AI/LLMChatClient.h"
#include "AI/LLMConnectionValidator.h"
#include "AI/LLMSettingsStore.h"
#include "ChatDisplay.h"

namespace more_phi {

class MorePhiProcessor;

class AIChatPanel final : public juce::Component
{
public:
    explicit AIChatPanel(MorePhiProcessor& processor);

    void paint(juce::Graphics& g) override;
    void resized() override;

    static bool detectsLocalMcpToolInventoryPromptForTest(const juce::String& text);
    static bool detectsLocalWorkflowPromptForTest(const juce::String& text);
    static bool detectsLocalWorkflowFeedbackPromptForTest(const juce::String& text);
    static bool detectsLocalUndoPromptForTest(const juce::String& text);
    static juce::String buildLocalMcpToolInventoryReplyForTest();
    static juce::String buildWorkflowTimelineTextForTest(const AssistantWorkflowResult& result);
    static juce::String buildApprovalQueueTextForTest(const nlohmann::json& approvalResponse);
    static juce::String formatChatErrorForTest(const juce::String& error);

private:
    void submitPrompt();
    void trimConversationHistory();
    bool tryHandleLocalWorkflowPrompt(const juce::String& text);
    void handleWorkflowFeedbackButton(const juce::String& feedbackText);
    void handleWorkflowUndoButton();
    void updateWorkflowPanel(const AssistantWorkflowResult& result);
    void setWorkflowPanelVisible(bool visible);
    void refreshApprovalPanel(bool announceWhenEmpty = false);
    void handleApprovalDecision(bool approve);
    void updateApprovalPanelFromQueue(const nlohmann::json& approvalResponse);
    void setApprovalPanelVisible(bool visible);
    bool tryHandleLocalDiagnosticPrompt(const juce::String& text);
    static bool isLocalMcpToolInventoryPrompt(const juce::String& text);
    static juce::String buildLocalMcpToolInventoryReply();
    static juce::String buildWorkflowTimelineText(const AssistantWorkflowResult& result);
    static juce::String buildApprovalQueueText(const nlohmann::json& approvalResponse);
    static juce::String formatChatErrorForDisplay(const juce::String& error);
    void cancelChat();
    void loadLLMSettings();
    void refreshLLMToolbar();
    void showLLMSettingsDialog();
    void onChatReply(juce::String text, juce::String error, juce::String updatedHistory);
    /** Called every 2s while a request is in flight to show elapsed time. */
    void scheduleThinkingUpdate();

    MorePhiProcessor& processor_;
    LLMSettingsStore  llmSettingsStore_;
    LLMConnectionValidator llmValidator_;
    LLMSettings       llmSettings_;
    AIAssistant       localAssistant_;
    LLMChatClient     llmChatClient_;

    /** JSON array of OpenAI-style role/content objects — persists across turns. */
    juce::String conversationHistory_;
    bool         chatPending_   = false;
    juce::int64  chatStartMs_   = 0;
    /** Shared with the reply callback so a cancelled request is silently ignored. */
    std::shared_ptr<std::atomic<bool>> chatCancelled_{std::make_shared<std::atomic<bool>>(false)};

    juce::Label       providerLabel_;
    juce::Label       statusChip_;
    juce::TextButton  approvalsButton_{"Approvals"};
    juce::TextButton  settingsButton_{"LLM Settings"};

    ChatDisplay       transcript_;
    AssistantWorkflowResult lastWorkflowResult_;
    juce::String      currentApprovalId_;
    nlohmann::json    lastApprovalQueue_ = nlohmann::json::object();
    juce::GroupComponent approvalPanel_{"assistantApprovalPanel", "Approval"};
    juce::Label       approvalStatusLabel_;
    juce::Label       approvalDetailLabel_;
    juce::TextButton  approvalApproveButton_{"Approve"};
    juce::TextButton  approvalRejectButton_{"Reject"};
    juce::TextButton  approvalRefreshButton_{"Refresh"};
    juce::GroupComponent workflowPanel_{"assistantWorkflowPanel", "Workflow"};
    juce::Label       workflowStatusLabel_;
    juce::Label       workflowDetailLabel_;
    juce::TextButton  workflowAcceptedButton_{"Accepted"};
    juce::TextButton  workflowRejectedButton_{"Rejected"};
    juce::TextButton  workflowBetterButton_{"Better"};
    juce::TextButton  workflowTooMuchButton_{"Too Much"};
    juce::TextButton  workflowUndoButton_{"Undo"};
    juce::TextEditor  prompt_;
    juce::TextButton  sendButton_{"Send"};
    juce::TextButton  cancelButton_{"Cancel"};
    juce::TextButton  clearButton_{"Clear"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AIChatPanel)
};

} // namespace more_phi
