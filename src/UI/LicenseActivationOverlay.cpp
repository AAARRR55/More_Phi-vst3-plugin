/*
 * More-Phi — UI/LicenseActivationOverlay.cpp
 * Elegant overlay dialog for entering and validating the product license key.
 */
#include "LicenseActivationOverlay.h"
#include "LicenseImportDialog.h"
#include "../Plugin/PluginProcessor.h"
#include "../Version.h"
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
    keyInput_.setTextToShowWhenEmpty("MPH1-XXXX-XXXX-XXXX-XXXX-XXXX-C", juce::Colours::grey);
    keyInput_.setFont(juce::Font(juce::FontOptions(16.0f, static_cast<int>(juce::Font::bold))));
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

    // Refresh (shown when licensed-but-grace/expired by refreshStateCopy)
    refreshBtn_.onClick = [this] { onRefreshClicked(); };
    addChildComponent(refreshBtn_);

    // Offline activation entry — for air-gapped machines that obtained a
    // signed certificate elsewhere.
    offlineBtn_.onClick = [this] { onOfflineClicked(); };
    addAndMakeVisible(offlineBtn_);

    refreshStateCopy();
}

LicenseActivationOverlay::~LicenseActivationOverlay() = default;

void LicenseActivationOverlay::paint(juce::Graphics& g)
{
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    juce::Colour bg = lnf ? lnf->backgroundDark : juce::Colour(0xff070709);
    juce::Colour surface = lnf ? lnf->surfaceColour : juce::Colour(0xff0d0d10);
    juce::Colour border = lnf ? lnf->borderColour : juce::Colour(0xff323237);

    // Draw dark semi-translucent background overlay over the entire editor
    g.fillAll(bg.withAlpha(0.85f));

    // Centered dialog container
    auto dialogArea = getLocalBounds().withSize(460, 340).withCentre(getLocalBounds().getCentre());

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
    auto dialogArea = getLocalBounds().withSize(460, 340).withCentre(getLocalBounds().getCentre());

    // Visual styling helper for custom colors
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    juce::Colour textCol = lnf ? lnf->textPrimary : juce::Colours::white;
    juce::Colour textDim = lnf ? lnf->textSecondary : juce::Colours::grey;
    juce::Colour coralAccent = lnf ? lnf->accentCoral : juce::Colour(0xffe5c057);

    // Layout coordinates inside dialog card
    auto inner = dialogArea.reduced(24);

    titleLabel_.setBounds(inner.removeFromTop(36));
    titleLabel_.setFont(juce::Font(juce::FontOptions(22.0f, static_cast<int>(juce::Font::bold))));
    titleLabel_.setColour(juce::Label::textColourId, coralAccent);

    descLabel_.setBounds(inner.removeFromTop(48));
    descLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    descLabel_.setColour(juce::Label::textColourId, textDim);

    inner.removeFromTop(12); // spacer

    keyInput_.setBounds(inner.removeFromTop(40).reduced(24, 0));
    keyInput_.setColour(juce::TextEditor::textColourId, textCol);
    keyInput_.setColour(juce::TextEditor::backgroundColourId, lnf ? lnf->backgroundDark : juce::Colour(0xff070709));
    keyInput_.setColour(juce::TextEditor::focusedOutlineColourId, coralAccent);
    keyInput_.setColour(juce::TextEditor::outlineColourId, lnf ? lnf->borderColour : juce::Colour(0xff323237));

    inner.removeFromTop(12); // spacer

    statusLabel_.setBounds(inner.removeFromTop(24));
    statusLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));

    inner.removeFromTop(8); // spacer

    auto btnRow = inner.removeFromTop(36).reduced(24, 0);
    int btnWidth = (btnRow.getWidth() - 16) / 2;
    buyBtn_.setBounds(btnRow.removeFromLeft(btnWidth));
    activateBtn_.setBounds(btnRow.removeFromRight(btnWidth));

    inner.removeFromTop(10); // spacer

    // Refresh button (only visible in grace/expired states — toggled in refreshStateCopy)
    refreshBtn_.setBounds(inner.removeFromTop(30).reduced(24, 0));

    inner.removeFromTop(6);

    // Offline link (small, bottom-centred)
    offlineBtn_.setBounds(inner.removeFromTop(24).reduced(120, 0));
    offlineBtn_.setColour(juce::TextButton::textColourOffId, textDim);
    offlineBtn_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
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
    juce::URL(juce::String(more_phi::STORE_URL)).launchInDefaultBrowser();
}

void LicenseActivationOverlay::onOfflineClicked()
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Activate Offline";
    options.dialogBackgroundColour = juce::Colour(0xff121826);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.content.setOwned(new LicenseImportDialog(processor_));
    options.launchAsync();
}

void LicenseActivationOverlay::onRefreshClicked()
{
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    statusLabel_.setText("Refreshing license...", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, lnf ? lnf->textSecondary : juce::Colours::grey);
    refreshBtn_.setEnabled(false);

    juce::Component::SafePointer<LicenseActivationOverlay> safeThis(this);
    auto& p = processor_;
    const auto activationId = p.getLicenseManager().lastActivationId();

    juce::Thread::launch([safeThis, &p, activationId]()
    {
        const auto result = p.getLicenseManager().refreshActivation(activationId);
        juce::MessageManager::callAsync([safeThis, result]()
        {
            if (safeThis != nullptr)
                safeThis->handleRefreshResult(result);
        });
    });
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

void LicenseActivationOverlay::handleRefreshResult(const licensing::ValidationResult& result)
{
    refreshBtn_.setEnabled(true);
    refreshStateCopy();
    (void) result;
}

void LicenseActivationOverlay::refreshStateCopy()
{
    auto* lnf = dynamic_cast<MorePhiLookAndFeel*>(&getLookAndFeel());
    const auto state = processor_.getLicenseRuntimeState().state.load(std::memory_order_relaxed);
    using LS = licensing::LicenseState;

    switch (state)
    {
        case LS::GracePeriod:
            titleLabel_.setText("Online Validation Required", juce::dontSendNotification);
            statusLabel_.setText("Your license needs an online check. Click Refresh Now to renew it.",
                                 juce::dontSendNotification);
            statusLabel_.setColour(juce::Label::textColourId,
                                   lnf ? lnf->accentAmber : juce::Colours::orange);
            refreshBtn_.setVisible(true);
            keyInput_.setVisible(false);
            activateBtn_.setVisible(false);
            break;

        case LS::Expired:
            titleLabel_.setText("License Expired", juce::dontSendNotification);
            statusLabel_.setText("This license has expired. Enter a new key or contact support.",
                                 juce::dontSendNotification);
            statusLabel_.setColour(juce::Label::textColourId,
                                   lnf ? lnf->accentCoral : juce::Colours::red);
            refreshBtn_.setVisible(false);
            keyInput_.setVisible(true);
            activateBtn_.setVisible(true);
            break;

        case LS::ActivationRequired:
            titleLabel_.setText("License Activation Required", juce::dontSendNotification);
            statusLabel_.setText("", juce::dontSendNotification);
            refreshBtn_.setVisible(false);
            keyInput_.setVisible(true);
            activateBtn_.setVisible(true);
            break;

        default:
            // Licensed / unknown — the editor hides this overlay entirely in
            // those states, but if shown, present the default activation copy.
            titleLabel_.setText("License Activation Required", juce::dontSendNotification);
            statusLabel_.setText("", juce::dontSendNotification);
            refreshBtn_.setVisible(false);
            keyInput_.setVisible(true);
            activateBtn_.setVisible(true);
            break;
    }
}

} // namespace more_phi
