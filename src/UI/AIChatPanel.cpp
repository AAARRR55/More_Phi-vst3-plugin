/*
 * More-Phi - UI/AIChatPanel.cpp
 */
#include "AIChatPanel.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

AIChatPanel::AIChatPanel(MorePhiProcessor& processor)
    : processor_(processor)
{
    (void) processor_;

    prompt_.setMultiLine(false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setTextToShowWhenEmpty("Ask the assistant", juce::Colour(0xff8a93a3));
    prompt_.onReturnKey = [this]() { submitPrompt(); };

    sendButton_.onClick = [this]() { submitPrompt(); };
    clearButton_.onClick = [this]() { transcript_.clearMessages(); };

    addAndMakeVisible(transcript_);
    addAndMakeVisible(prompt_);
    addAndMakeVisible(sendButton_);
    addAndMakeVisible(clearButton_);
}

void AIChatPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto inputRow = area.removeFromBottom(30);
    transcript_.setBounds(area.reduced(0, 0));

    clearButton_.setBounds(inputRow.removeFromRight(64).reduced(3, 1));
    sendButton_.setBounds(inputRow.removeFromRight(64).reduced(3, 1));
    prompt_.setBounds(inputRow.reduced(0, 1));
}

void AIChatPanel::submitPrompt()
{
    auto text = prompt_.getText().trim();
    if (text.isEmpty())
        return;

    prompt_.clear();
    transcript_.addMessage(ChatDisplay::Role::User, text);
    transcript_.addMessage(ChatDisplay::Role::Assistant,
                           "Assistant command routing is available through MCP.");
}

} // namespace more_phi
