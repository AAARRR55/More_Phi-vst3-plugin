/*
 * More-Phi — UI/LicenseActivationOverlay.h
 * Elegant overlay dialog for entering and validating the product license key.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Licensing/LicenseManager.h"

namespace more_phi {

class MorePhiProcessor;
class LicenseImportDialog;

class LicenseActivationOverlay : public juce::Component
{
public:
    explicit LicenseActivationOverlay(MorePhiProcessor& processor);
    ~LicenseActivationOverlay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Intercept mouse events to prevent clicking components behind the overlay
    void mouseDown(const juce::MouseEvent& event) override { (void)event; }
    void mouseUp(const juce::MouseEvent& event) override { (void)event; }
    void mouseDrag(const juce::MouseEvent& event) override { (void)event; }

    // R4: true once the user chose "Continue in Demo" this session. The editor
    // timer honors this so it doesn't re-spawn the overlay every tick. Audio is
    // never muted for lack of a license — only premium AI features are gated —
    // so demo mode is a fully usable plugin, not a brick. Resets on reopen.
    bool isDismissedForSession() const noexcept { return dismissedForSession_; }

private:
    void onActivateClicked();
    void onBuyClicked();
    void onOfflineClicked();
    void onRefreshClicked();
    void onDemoClicked();
    void handleActivationResult(const licensing::ValidationResult& result);
    void handleRefreshResult(const licensing::ValidationResult& result);

    // Re-renders the status/description copy for the current runtime state
    // (activation-required / grace / expired). Called from the editor timer and
    // after each activate/refresh.
    void refreshStateCopy();

    MorePhiProcessor& processor_;

    // UI Components
    juce::Label titleLabel_;
    juce::Label descLabel_;
    juce::TextEditor keyInput_;
    juce::TextButton activateBtn_{"Activate"};
    juce::TextButton buyBtn_{"Get License Key"};
    juce::TextButton refreshBtn_{"Refresh Now"};
    juce::TextButton offlineBtn_{"Activate Offline"};
    juce::TextButton demoBtn_{"Continue in Demo"};
    juce::Label statusLabel_;
    juce::HyperlinkButton offlineLink_;

    bool isActivating_ = false;
    bool dismissedForSession_ = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(LicenseActivationOverlay)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LicenseActivationOverlay)
};

} // namespace more_phi
