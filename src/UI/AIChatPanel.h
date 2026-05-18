/*
 * More-Phi - UI/AIChatPanel.h
 * Assistant tab container.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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

    void resized() override;

    static bool detectsLocalMcpToolInventoryPromptForTest(const juce::String& text);
    static juce::String buildLocalMcpToolInventoryReplyForTest();
    static juce::String formatChatErrorForTest(const juce::String& error);

private:
    void submitPrompt();
    void trimConversationHistory();
    bool tryHandleLocalDiagnosticPrompt(const juce::String& text);
    static bool isLocalMcpToolInventoryPrompt(const juce::String& text);
    static juce::String buildLocalMcpToolInventoryReply();
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
    LLMChatClient     llmChatClient_;

    /** JSON array of OpenAI-style role/content objects — persists across turns. */
    juce::String conversationHistory_;
    bool         chatPending_   = false;
    juce::int64  chatStartMs_   = 0;
    /** Shared with the reply callback so a cancelled request is silently ignored. */
    std::shared_ptr<std::atomic<bool>> chatCancelled_{std::make_shared<std::atomic<bool>>(false)};

    juce::Label       providerLabel_;
    juce::Label       statusChip_;
    juce::TextButton  settingsButton_{"LLM Settings"};

    ChatDisplay       transcript_;
    juce::TextEditor  prompt_;
    juce::TextButton  sendButton_{"Send"};
    juce::TextButton  cancelButton_{"Cancel"};
    juce::TextButton  clearButton_{"Clear"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AIChatPanel)
};

} // namespace more_phi
