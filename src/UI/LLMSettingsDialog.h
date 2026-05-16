#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AI/LLMConnectionValidator.h"
#include "AI/LLMSettingsStore.h"

namespace more_phi {

class LLMSettingsDialog final : public juce::Component
{
public:
    using SavedCallback = std::function<void(const LLMSettings&)>;

    LLMSettingsDialog(LLMSettings initialSettings,
                      LLMSettingsStore& store,
                      LLMConnectionValidator& validator,
                      SavedCallback onSaved);

    void resized() override;

    static juce::StringArray providerNamesForMenu();
    static bool isBaseUrlEditableForProvider(LLMProviderId providerId);

private:
    LLMProviderId selectedProviderId() const;
    LLMProviderSettings& selectedDraftProvider();
    const LLMProviderSettings& selectedDraftProvider() const;

    void configureControls();
    void loadProviderIntoControls(LLMProviderId providerId);
    void saveControlsIntoSelectedProvider();
    void populateProviderCombo();
    void populateModelCombo(const juce::String& selectedModel);
    void refreshBaseUrlEditability();
    void refreshButtonStates();
    void setStatusMessage(const juce::String& message, LLMValidationStatus status);

    void fetchModels();
    void testConnection();
    void saveAndClose();
    void cancelAndClose();

    LLMSettings draftSettings_;
    LLMSettingsStore& store_;
    LLMConnectionValidator& validator_;
    SavedCallback onSaved_;

    juce::Label titleLabel_;
    juce::Label providerLabel_;
    juce::ComboBox providerCombo_;
    juce::Label apiKeyLabel_;
    juce::TextEditor apiKeyEditor_;
    juce::Label baseUrlLabel_;
    juce::TextEditor baseUrlEditor_;
    juce::TextButton fetchModelsButton_{"Fetch Models"};
    juce::Label modelLabel_;
    juce::ComboBox modelCombo_;
    juce::TextButton testConnectionButton_{"Test Connection"};
    juce::TextButton saveButton_{"Save"};
    juce::TextButton cancelButton_{"Cancel"};
    juce::Label statusLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMSettingsDialog)
};

} // namespace more_phi
