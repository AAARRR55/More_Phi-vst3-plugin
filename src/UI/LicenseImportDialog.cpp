/*
 * More-Phi — UI/LicenseImportDialog.cpp
 */
#include "LicenseImportDialog.h"
#include "../Plugin/PluginProcessor.h"
#include "MorePhiLookAndFeel.h"

namespace more_phi {

LicenseImportDialog::LicenseImportDialog(MorePhiProcessor& processor)
    : processor_(processor)
{
    titleLabel_.setText("Activate Offline", juce::dontSendNotification);
    titleLabel_.setJustificationType(juce::Justification::centred);
    titleLabel_.setFont(juce::Font(juce::FontOptions(20.0f, static_cast<int>(juce::Font::bold))));
    addAndMakeVisible(titleLabel_);

    descLabel_.setText(
        "Paste the signed certificate JSON you obtained from your account or "
        "support. It looks like: { \"payload\": \"...\", \"signature\": \"...\", \"keyId\": \"...\" }.",
        juce::dontSendNotification);
    descLabel_.setJustificationType(juce::Justification::centredLeft);
    descLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(descLabel_);

    certInput_.setMultiLine(true);
    certInput_.setReturnKeyStartsNewLine(true);
    certInput_.setTextToShowWhenEmpty(
        "{\"payload\":\"...\",\"signature\":\"...\",\"keyId\":\"prod-ed25519-2026-01\"}",
        juce::Colours::grey);
    certInput_.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, 0)));
    addAndMakeVisible(certInput_);

    statusLabel_.setJustificationType(juce::Justification::centred);
    statusLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(statusLabel_);

    importBtn_.onClick = [this] { onImportClicked(); };
    addAndMakeVisible(importBtn_);

    cancelBtn_.onClick = [this] { onCancelClicked(); };
    addAndMakeVisible(cancelBtn_);
}

LicenseImportDialog::~LicenseImportDialog() = default;

void LicenseImportDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff121826));
}

void LicenseImportDialog::resized()
{
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    auto area = getLocalBounds().reduced(20);

    titleLabel_.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);
    descLabel_.setBounds(area.removeFromTop(48));
    area.removeFromTop(8);
    certInput_.setBounds(area.removeFromTop(180));
    area.removeFromTop(8);
    statusLabel_.setBounds(area.removeFromTop(22));

    auto row = area.removeFromTop(32);
    const int halfW = (row.getWidth() - 12) / 2;
    cancelBtn_.setBounds(row.removeFromLeft(halfW));
    row.removeFromLeft(12);
    importBtn_.setBounds(row);

    certInput_.setColour(juce::TextEditor::backgroundColourId,
                         lnf ? lnf->backgroundDark : juce::Colour(0xff070709));
    certInput_.setColour(juce::TextEditor::outlineColourId,
                         lnf ? lnf->borderColour : juce::Colour(0xff323237));
}

void LicenseImportDialog::onImportClicked()
{
    const auto text = certInput_.getText().trim();
    if (text.isEmpty())
    {
        statusLabel_.setText("Paste a certificate first.", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
        return;
    }

    isImporting_ = true;
    importBtn_.setEnabled(false);
    cancelBtn_.setEnabled(false);
    certInput_.setEnabled(false);
    statusLabel_.setText("Verifying certificate...", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);

    juce::Component::SafePointer<LicenseImportDialog> safeThis(this);
    auto& p = processor_;
    juce::Thread::launch([safeThis, &p, text]()
    {
        const auto result = p.getLicenseManager().importOfflineCertificate(text);
        juce::MessageManager::callAsync([safeThis, result]()
        {
            if (safeThis != nullptr)
                safeThis->handleImportResult(result);
        });
    });
}

void LicenseImportDialog::onCancelClicked()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
}

void LicenseImportDialog::handleImportResult(const licensing::ValidationResult& result)
{
    isImporting_ = false;
    importBtn_.setEnabled(true);
    cancelBtn_.setEnabled(true);
    certInput_.setEnabled(true);

    if (result.enablesPremiumFeatures)
    {
        statusLabel_.setText("License imported successfully!", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff34d399));
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(1);
        return;
    }

    statusLabel_.setText(result.message.isNotEmpty() ? result.message : "Certificate could not be verified.",
                         juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
}

} // namespace more_phi
