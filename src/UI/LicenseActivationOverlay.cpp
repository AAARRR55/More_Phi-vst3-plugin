/*
 * More-Phi — UI/LicenseActivationOverlay.cpp
 * Elegant overlay dialog for entering and validating the product license key.
 */
#include "LicenseActivationOverlay.h"
#include "../Plugin/PluginProcessor.h"
#include "MorePhiLookAndFeel.h"

namespace more_phi {

LicenseActivationOverlay::LicenseActivationOverlay(MorePhiProcessor& processor)
    : processor_(processor)
{
    // Configure Labels
    titleLabel_.setText("License Activation Required", juce::dontSendNotification);
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);

    descLabel_.setText("Please enter your More-Phi license key to unlock the plugin.\n"
                       "Obtain your license key from your account dashboard on the website and paste it here.", juce::dontSendNotification);
    descLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(descLabel_);

    // Configure Key Input
    keyInput_.setTextToShowWhenEmpty("MPHI-XXXXX-XXXXX-XXXXX-XXXXX", juce::Colours::grey);
    keyInput_.setJustification(juce::Justification::centred);
    keyInput_.setFont(juce::Font(16.0f, juce::Font::bold));
    addAndMakeVisible(keyInput_);

    // Status label
    statusLabel_.setText("", juce::dontSendNotification);
    statusLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel_);

    // Configure Buttons
    activateBtn_.onClick = [this] { onActivateClicked(); };
    addAndMakeVisible(activateBtn_);

    buyBtn_.onClick = [this] { onBuyClicked(); };
    addAndMakeVisible(buyBtn_);
}

LicenseActivationOverlay::~LicenseActivationOverlay() = default;

void LicenseActivationOverlay::paint(juce::Graphics& g)
{
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    juce::Colour bg = lnf ? lnf->backgroundDark : juce::Colour(0xff0d1b2a);
    juce::Colour surface = lnf ? lnf->surfaceColour : juce::Colour(0xff16213e);
    juce::Colour border = lnf ? lnf->borderColour : juce::Colour(0xff1e3a5f);

    // Draw dark semi-translucent background overlay over the entire editor
    g.fillAll(bg.withAlpha(0.85f));

    // Centered dialog container
    auto dialogArea = getLocalBounds().withSize(460, 300).withCentre(getLocalBounds().getCentre());
    
    // Draw outer glow/shadow
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.fillRoundedRectangle(dialogArea.toFloat().translated(0, 4), 12.0f);

    // Draw dialog card background
    g.setColour(surface);
    g.fillRoundedRectangle(dialogArea.toFloat(), 12.0f);

    // Draw card border
    g.setColour(border);
    g.drawRoundedRectangle(dialogArea.toFloat(), 12.0f, 1.5f);
}

void LicenseActivationOverlay::resized()
{
    auto dialogArea = getLocalBounds().withSize(460, 300).withCentre(getLocalBounds().getCentre());
    
    // Visual styling helper for custom colors
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    juce::Colour textCol = lnf ? lnf->textPrimary : juce::Colours::white;
    juce::Colour textDim = lnf ? lnf->textSecondary : juce::Colours::grey;
    juce::Colour coralAccent = lnf ? lnf->accentCoral : juce::Colour(0xffec415d);
    
    // Layout coordinates inside dialog card
    auto inner = dialogArea.reduced(24);
    
    titleLabel_.setBounds(inner.removeFromTop(36));
    titleLabel_.setFont(juce::Font(22.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, coralAccent);

    descLabel_.setBounds(inner.removeFromTop(48));
    descLabel_.setFont(juce::Font(13.0f));
    descLabel_.setColour(juce::Label::textColourId, textDim);

    inner.removeFromTop(12); // spacer

    keyInput_.setBounds(inner.removeFromTop(40).reduced(24, 0));
    keyInput_.setColour(juce::TextEditor::textColourId, textCol);
    keyInput_.setColour(juce::TextEditor::backgroundColourId, lnf ? lnf->backgroundDark : juce::Colour(0xff0d1b2a));
    keyInput_.setColour(juce::TextEditor::focusedOutlineColourId, coralAccent);
    keyInput_.setColour(juce::TextEditor::outlineColourId, lnf ? lnf->borderColour : juce::Colour(0xff1e3a5f));
    
    inner.removeFromTop(12); // spacer

    statusLabel_.setBounds(inner.removeFromTop(24));
    statusLabel_.setFont(juce::Font(12.0f));

    inner.removeFromTop(8); // spacer

    auto btnRow = inner.removeFromTop(36).reduced(24, 0);
    int btnWidth = (btnRow.getWidth() - 16) / 2;
    buyBtn_.setBounds(btnRow.removeFromLeft(btnWidth));
    activateBtn_.setBounds(btnRow.removeFromRight(btnWidth));
}

void LicenseActivationOverlay::onActivateClicked()
{
    juce::String key = keyInput_.getText().trim();
    if (key.isEmpty())
    {
        statusLabel_.setText("License key cannot be empty.", juce::dontSendNotification);
        auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
        statusLabel_.setColour(juce::Label::textColourId, lnf ? lnf->accentCoral : juce::Colours::red);
        return;
    }

    // Set UI to activating state
    isActivating_ = true;
    keyInput_.setEnabled(false);
    activateBtn_.setEnabled(false);
    buyBtn_.setEnabled(false);

    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    statusLabel_.setText("Connecting to activation server...", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, lnf ? lnf->textSecondary : juce::Colours::grey);

    // Launch background thread to handle activation (prevents UI blocking)
    juce::Component::SafePointer<LicenseActivationOverlay> safeThis(this);
    auto& p = processor_;
    juce::Thread::launch([safeThis, &p, key]()
    {
        auto result = p.getLicenseManager().activateWithKey(key, {});

        juce::MessageManager::callAsync([safeThis, result]()
        {
            if (safeThis != nullptr)
                safeThis->handleActivationResult(result);
        });
    });
}

void LicenseActivationOverlay::onBuyClicked()
{
    juce::URL("http://localhost:3000").launchInDefaultBrowser();
}

void LicenseActivationOverlay::handleActivationResult(const licensing::ValidationResult& result)
{
    isActivating_ = false;
    keyInput_.setEnabled(true);
    activateBtn_.setEnabled(true);
    buyBtn_.setEnabled(true);

    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    if (result.enablesPremiumFeatures)
    {
        statusLabel_.setText("License activated successfully!", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, lnf ? lnf->accentGreen : juce::Colours::green);
        
        // Hide overlay dynamically
        setVisible(false);
    }
    else
    {
        statusLabel_.setText(result.message.isNotEmpty() ? result.message : "Activation failed.", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, lnf ? lnf->accentCoral : juce::Colours::red);
    }
}

} // namespace more_phi
