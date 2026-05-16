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
    , llmChatClient_(processor)
{
    providerLabel_.setJustificationType(juce::Justification::centredLeft);
    statusChip_.setJustificationType(juce::Justification::centred);
    settingsButton_.onClick = [this]() { showLLMSettingsDialog(); };

    prompt_.setMultiLine(false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setTextToShowWhenEmpty("Ask the assistant", juce::Colour(0xff8a93a3));
    prompt_.onReturnKey = [this]() { submitPrompt(); };

    sendButton_.onClick   = [this]() { submitPrompt(); };
    cancelButton_.onClick = [this]() { cancelChat(); };
    cancelButton_.setEnabled(false);
    clearButton_.onClick = [this]()
    {
        transcript_.clearMessages();
        conversationHistory_.clear();
    };

    addAndMakeVisible(providerLabel_);
    addAndMakeVisible(statusChip_);
    addAndMakeVisible(settingsButton_);
    addAndMakeVisible(transcript_);
    addAndMakeVisible(prompt_);
    addAndMakeVisible(sendButton_);
    addAndMakeVisible(cancelButton_);
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
        transcript_.addMessage(ChatDisplay::Role::System, error);
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
        [this](const LLMSettings& savedSettings)
        {
            llmSettings_ = savedSettings;
            refreshLLMToolbar();
        }));

    options.launchAsync();
}

void AIChatPanel::submitPrompt()
{
    if (chatPending_)
        return;

    auto text = prompt_.getText().trim();
    if (text.isEmpty())
        return;

    prompt_.clear();
    transcript_.addMessage(ChatDisplay::Role::User, text);
    transcript_.addMessage(ChatDisplay::Role::Assistant, "Thinking...");

    chatPending_ = true;
    chatStartMs_ = juce::Time::currentTimeMillis();
    chatCancelled_ = std::make_shared<std::atomic<bool>>(false);
    sendButton_.setEnabled(false);
    cancelButton_.setEnabled(true);

    auto cancelled = chatCancelled_;
    llmChatClient_.chat(llmSettings_, conversationHistory_, text,
        [this, cancelled](juce::String replyText, juce::String errorMsg, juce::String updatedHistory)
        {
            if (*cancelled) return; // request was cancelled by the user
            onChatReply(std::move(replyText), std::move(errorMsg), std::move(updatedHistory));
        });

    scheduleThinkingUpdate();
}

void AIChatPanel::cancelChat()
{
    if (!chatPending_) return;
    *chatCancelled_ = true;
    chatPending_ = false;
    sendButton_.setEnabled(true);
    cancelButton_.setEnabled(false);
    transcript_.updateLastMessage("(cancelled)");
}

void AIChatPanel::scheduleThinkingUpdate()
{
    if (!chatPending_) return;
    const int secs = static_cast<int>((juce::Time::currentTimeMillis() - chatStartMs_) / 1000);
    transcript_.updateLastMessage("Thinking... (" + juce::String(secs) + "s)");

    juce::Component::SafePointer<AIChatPanel> safeThis(this);
    juce::Timer::callAfterDelay(2000, [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->scheduleThinkingUpdate();
    });
}

void AIChatPanel::onChatReply(juce::String text, juce::String error, juce::String updatedHistory)
{
    // Called on message thread
    chatPending_ = false;
    sendButton_.setEnabled(true);
    cancelButton_.setEnabled(false);

    if (!error.isEmpty())
    {
        transcript_.updateLastMessage("[Error] " + error);
        return;
    }

    conversationHistory_ = updatedHistory;
    transcript_.updateLastMessage(text.isEmpty() ? "(empty response)" : text);
}

void AIChatPanel::resized()
{
    auto area = getLocalBounds().reduced(8);

    // ── Toolbar row ──────────────────────────────────────────────────────────
    auto toolbar = area.removeFromTop(28);
    settingsButton_.setBounds(toolbar.removeFromRight(110).reduced(2, 1));
    statusChip_.setBounds(toolbar.removeFromRight(90).reduced(4, 3));
    providerLabel_.setBounds(toolbar.reduced(0, 1));

    area.removeFromTop(4);

    // ── Input row ────────────────────────────────────────────────────────────
    auto inputRow = area.removeFromBottom(34);
    inputRow.removeFromTop(4);
    clearButton_.setBounds(inputRow.removeFromRight(60).reduced(2, 1));
    cancelButton_.setBounds(inputRow.removeFromRight(64).reduced(2, 1));
    sendButton_ .setBounds(inputRow.removeFromRight(60).reduced(2, 1));
    prompt_.setBounds(inputRow.reduced(0, 1));

    // ── Transcript fills the rest ─────────────────────────────────────────────
    transcript_.setBounds(area);
}

} // namespace more_phi

