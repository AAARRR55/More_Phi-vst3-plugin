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

private:
    void submitPrompt();
    void loadLLMSettings();
    void refreshLLMToolbar();
    void showLLMSettingsDialog();
    void onChatReply(juce::String text, juce::String error, juce::String updatedHistory);

    MorePhiProcessor& processor_;
    LLMSettingsStore  llmSettingsStore_;
    LLMConnectionValidator llmValidator_;
    LLMSettings       llmSettings_;
    LLMChatClient     llmChatClient_;

    /** JSON array of OpenAI-style role/content objects — persists across turns. */
    juce::String conversationHistory_;
    bool         chatPending_ = false;

    juce::Label       providerLabel_;
    juce::Label       statusChip_;
    juce::TextButton  settingsButton_{"LLM Settings"};

    ChatDisplay       transcript_;
    juce::TextEditor  prompt_;
    juce::TextButton  sendButton_{"Send"};
    juce::TextButton  clearButton_{"Clear"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AIChatPanel)
};

} // namespace more_phi

