/*
 * More-Phi - UI/AIChatPanel.cpp
 */
#include "AIChatPanel.h"
#include "LLMSettingsDialog.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

AIChatPanel::AIChatPanel(MorePhiProcessor& processor)
    : processor_(processor)
    , llmSettings_(LLMSettings::createDefault())
{
    (void) processor_;

    providerLabel_.setJustificationType(juce::Justification::centredLeft);
    statusChip_.setJustificationType(juce::Justification::centred);
    settingsButton_.onClick = [this]() { showLLMSettingsDialog(); };

    prompt_.setMultiLine(false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setTextToShowWhenEmpty("Ask the assistant", juce::Colour(0xff8a93a3));
    prompt_.onReturnKey = [this]() { submitPrompt(); };

    sendButton_.onClick  = [this]() { submitPrompt(); };
    clearButton_.onClick = [this]() { transcript_.clearMessages(); };

    addAndMakeVisible(providerLabel_);
    addAndMakeVisible(statusChip_);
    addAndMakeVisible(settingsButton_);
    addAndMakeVisible(transcript_);
    addAndMakeVisible(prompt_);
    addAndMakeVisible(sendButton_);
    addAndMakeVisible(clearButton_);

    loadLLMSettings();
    refreshLLMToolbar();
}

void AIChatPanel::loadLLMSettings()
{
    juce::String error;
    if (!llmSettingsStore_.load(llmSettings_, error))
    {
        llmSettings_ = LLMSettings::createDefault();
        statusChip_.setText("Failed", juce::dontSendNotification);
        providerLabel_.setText("Provider: None", juce::dontSendNotification);
        transcript_.addMessage(ChatDisplay::Role::Assistant, error);
    }
}

void AIChatPanel::refreshLLMToolbar()
{
    const auto providerName = llmSettings_.getActiveProviderDisplayName();
    providerLabel_.setText("Provider: " + (providerName.isEmpty() ? "None" : providerName),
                           juce::dontSendNotification);
    statusChip_.setText(toDisplayString(llmSettings_.getToolbarStatus()), juce::dontSendNotification);
}

void AIChatPanel::showLLMSettingsDialog()
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "LLM Settings";
    options.dialogBackgroundColour = juce::Colour(0xff121826);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    options.content.setOwned(new LLMSettingsDialog(
        llmSettings_,
        llmSettingsStore_,
        llmValidator_,
        [this](const LLMSettings& savedSettings) {
            llmSettings_ = savedSettings;
            refreshLLMToolbar();
        }));

    options.launchAsync();
}

void AIChatPanel::resized()
{
    auto area = getLocalBounds().reduced(8);

    auto toolbar = area.removeFromTop(30);
    settingsButton_.setBounds(toolbar.removeFromRight(120).reduced(3, 1));
    statusChip_.setBounds(toolbar.removeFromRight(150).reduced(3, 3));
    providerLabel_.setBounds(toolbar.reduced(0, 1));

    area.removeFromTop(6);

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

