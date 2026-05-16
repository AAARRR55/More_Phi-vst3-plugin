#include "LLMSettingsDialog.h"

namespace more_phi {

LLMSettingsDialog::LLMSettingsDialog(LLMSettings initialSettings,
                                     LLMSettingsStore& store,
                                     LLMConnectionValidator& validator,
                                     SavedCallback onSaved)
    : draftSettings_(std::move(initialSettings))
    , store_(store)
    , validator_(validator)
    , onSaved_(std::move(onSaved))
{
    configureControls();
    populateProviderCombo();

    const auto initialProvider = draftSettings_.activeProvider.value_or(LLMProviderId::Anthropic);
    providerCombo_.setSelectedId(static_cast<int>(llmProviderIndex(initialProvider)) + 1, juce::dontSendNotification);
    loadProviderIntoControls(initialProvider);

    setSize(520, 360);
}

juce::StringArray LLMSettingsDialog::providerNamesForMenu()
{
    juce::StringArray names;
    for (const auto& provider : getLLMProviderDefinitions())
        names.add(provider.displayName);
    return names;
}

bool LLMSettingsDialog::isBaseUrlEditableForProvider(LLMProviderId providerId)
{
    return getLLMProviderDefinition(providerId).customBaseUrlAllowed;
}

void LLMSettingsDialog::configureControls()
{
    titleLabel_.setText("LLM Settings", juce::dontSendNotification);
    titleLabel_.setJustificationType(juce::Justification::centredLeft);

    providerLabel_.setText("Provider", juce::dontSendNotification);
    apiKeyLabel_.setText("API Key", juce::dontSendNotification);
    baseUrlLabel_.setText("Base URL", juce::dontSendNotification);
    modelLabel_.setText("Model", juce::dontSendNotification);

    apiKeyEditor_.setPasswordCharacter(juce::CharPointer_UTF8("\xe2\x80\xa2").getAndAdvance());
    apiKeyEditor_.setMultiLine(false);
    baseUrlEditor_.setMultiLine(false);
    statusLabel_.setJustificationType(juce::Justification::centredLeft);

    providerCombo_.onChange = [this]() {
        saveControlsIntoSelectedProvider();
        loadProviderIntoControls(selectedProviderId());
    };
    modelCombo_.onChange = [this]() {
        selectedDraftProvider().selectedModel = modelCombo_.getText();
        refreshButtonStates();
    };
    apiKeyEditor_.onTextChange = [this]() {
        selectedDraftProvider().apiKey = apiKeyEditor_.getText();
        refreshButtonStates();
    };
    baseUrlEditor_.onTextChange = [this]() {
        selectedDraftProvider().customBaseUrl = baseUrlEditor_.getText();
        refreshButtonStates();
    };

    fetchModelsButton_.onClick     = [this]() { fetchModels(); };
    testConnectionButton_.onClick  = [this]() { testConnection(); };
    saveButton_.onClick            = [this]() { saveAndClose(); };
    cancelButton_.onClick          = [this]() { cancelAndClose(); };

    for (juce::Component* component : std::initializer_list<juce::Component*>{
             &titleLabel_, &providerLabel_, &providerCombo_,
             &apiKeyLabel_, &apiKeyEditor_, &baseUrlLabel_, &baseUrlEditor_,
             &fetchModelsButton_, &modelLabel_, &modelCombo_, &testConnectionButton_,
             &saveButton_, &cancelButton_, &statusLabel_ })
        addAndMakeVisible(component);
}

void LLMSettingsDialog::populateProviderCombo()
{
    providerCombo_.clear(juce::dontSendNotification);
    int itemId = 1;
    for (const auto& provider : getLLMProviderDefinitions())
        providerCombo_.addItem(provider.displayName, itemId++);
}

LLMProviderId LLMSettingsDialog::selectedProviderId() const
{
    const auto index = juce::jlimit(0, 5, providerCombo_.getSelectedId() - 1);
    return getLLMProviderDefinitions()[static_cast<std::size_t>(index)].id;
}

LLMProviderSettings& LLMSettingsDialog::selectedDraftProvider()
{
    return draftSettings_.getProvider(selectedProviderId());
}

const LLMProviderSettings& LLMSettingsDialog::selectedDraftProvider() const
{
    return draftSettings_.getProvider(selectedProviderId());
}

void LLMSettingsDialog::saveControlsIntoSelectedProvider()
{
    auto& provider = selectedDraftProvider();
    provider.apiKey = apiKeyEditor_.getText();
    provider.selectedModel = modelCombo_.getText();
    if (isBaseUrlEditableForProvider(selectedProviderId()))
        provider.customBaseUrl = baseUrlEditor_.getText();
}

void LLMSettingsDialog::loadProviderIntoControls(LLMProviderId providerId)
{
    const auto& provider = draftSettings_.getProvider(providerId);
    apiKeyEditor_.setText(provider.apiKey, juce::dontSendNotification);
    baseUrlEditor_.setText(draftSettings_.getBaseUrl(providerId), juce::dontSendNotification);
    populateModelCombo(provider.selectedModel);
    refreshBaseUrlEditability();
    setStatusMessage(provider.validationMessage, provider.validationStatus);
    refreshButtonStates();
}

void LLMSettingsDialog::populateModelCombo(const juce::String& selectedModel)
{
    modelCombo_.clear(juce::dontSendNotification);
    int itemId = 1;
    for (const auto& model : selectedDraftProvider().availableModels)
        modelCombo_.addItem(model, itemId++);

    if (selectedModel.isNotEmpty())
        modelCombo_.setText(selectedModel, juce::dontSendNotification);
}

void LLMSettingsDialog::refreshBaseUrlEditability()
{
    const auto editable = isBaseUrlEditableForProvider(selectedProviderId());
    baseUrlEditor_.setReadOnly(!editable);
    baseUrlEditor_.setColour(juce::TextEditor::backgroundColourId,
                             editable ? juce::Colours::black : juce::Colour(0xff202020));
}

void LLMSettingsDialog::refreshButtonStates()
{
    const auto& provider = selectedDraftProvider();
    const auto hasApiKey  = provider.apiKey.trim().isNotEmpty();
    const auto hasBaseUrl = draftSettings_.getBaseUrl(selectedProviderId()).trim().isNotEmpty();
    const auto hasModel   = provider.selectedModel.trim().isNotEmpty();

    fetchModelsButton_.setEnabled(hasApiKey && hasBaseUrl);
    testConnectionButton_.setEnabled(hasApiKey && hasBaseUrl && hasModel);
}

void LLMSettingsDialog::setStatusMessage(const juce::String& message, LLMValidationStatus status)
{
    const auto display = message.isNotEmpty() ? message : toDisplayString(status);
    statusLabel_.setText(display, juce::dontSendNotification);
}

void LLMSettingsDialog::fetchModels()
{
    saveControlsIntoSelectedProvider();
    const auto providerId = selectedProviderId();
    selectedDraftProvider().validationStatus = LLMValidationStatus::Testing;
    setStatusMessage("Fetching models...", LLMValidationStatus::Testing);
    refreshButtonStates();

    juce::Component::SafePointer<LLMSettingsDialog> safeThis(this);
    validator_.fetchModelsAsync(providerId, selectedDraftProvider(), [safeThis, providerId](LLMModelFetchResult result) mutable {
        if (safeThis == nullptr)
            return;

        auto& provider = safeThis->draftSettings_.getProvider(providerId);
        provider.validationStatus = result.status;
        provider.validationMessage = result.message;
        if (result.success)
        {
            provider.availableModels = result.models;
            if (provider.selectedModel.isEmpty() && !result.models.isEmpty())
                provider.selectedModel = result.models[0];
        }

        if (safeThis->selectedProviderId() == providerId)
            safeThis->loadProviderIntoControls(providerId);
    });
}

void LLMSettingsDialog::testConnection()
{
    saveControlsIntoSelectedProvider();
    const auto providerId = selectedProviderId();
    selectedDraftProvider().validationStatus = LLMValidationStatus::Testing;
    setStatusMessage("Testing connection...", LLMValidationStatus::Testing);
    refreshButtonStates();

    juce::Component::SafePointer<LLMSettingsDialog> safeThis(this);
    validator_.testConnectionAsync(providerId, selectedDraftProvider(), [safeThis, providerId](LLMValidationResult result) mutable {
        if (safeThis == nullptr)
            return;

        auto& provider = safeThis->draftSettings_.getProvider(providerId);
        provider.validationStatus = result.status;
        provider.validationMessage = result.message;
        provider.validationTimestampMs = juce::Time::currentTimeMillis();

        if (safeThis->selectedProviderId() == providerId)
            safeThis->loadProviderIntoControls(providerId);
    });
}

void LLMSettingsDialog::saveAndClose()
{
    saveControlsIntoSelectedProvider();
    // Always activate the selected provider when the user explicitly saves.
    // activateProviderIfValidated requires Active status, which is too strict: users
    // may skip testing, or the test may fail due to cold-start / network conditions.
    const auto pid = selectedProviderId();
    if (selectedDraftProvider().apiKey.trim().isNotEmpty())
        draftSettings_.activeProvider = pid;
    else
        draftSettings_.activateProviderIfValidated(pid); // fall back to guarded activation

    juce::String error;
    if (!store_.save(draftSettings_, error))
    {
        setStatusMessage(error, LLMValidationStatus::Failed);
        return;
    }

    if (onSaved_)
        onSaved_(draftSettings_);

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
}

void LLMSettingsDialog::cancelAndClose()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
}

void LLMSettingsDialog::resized()
{
    auto area = getLocalBounds().reduced(16);
    titleLabel_.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);

    auto row = area.removeFromTop(30);
    providerLabel_.setBounds(row.removeFromLeft(110));
    providerCombo_.setBounds(row);
    area.removeFromTop(8);

    row = area.removeFromTop(30);
    apiKeyLabel_.setBounds(row.removeFromLeft(110));
    apiKeyEditor_.setBounds(row);
    area.removeFromTop(8);

    row = area.removeFromTop(30);
    baseUrlLabel_.setBounds(row.removeFromLeft(110));
    baseUrlEditor_.setBounds(row);
    area.removeFromTop(8);

    row = area.removeFromTop(30);
    fetchModelsButton_.setBounds(row.removeFromRight(130));
    modelLabel_.setBounds(row.removeFromLeft(110));
    modelCombo_.setBounds(row);
    area.removeFromTop(8);

    testConnectionButton_.setBounds(area.removeFromTop(30).removeFromRight(150));
    area.removeFromTop(8);

    statusLabel_.setBounds(area.removeFromTop(60));

    auto buttons = area.removeFromBottom(34);
    cancelButton_.setBounds(buttons.removeFromRight(90).reduced(4, 2));
    saveButton_.setBounds(buttons.removeFromRight(90).reduced(4, 2));
}

} // namespace more_phi
