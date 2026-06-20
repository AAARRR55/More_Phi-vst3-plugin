/*
 * More-Phi — UI/LicenseImportDialog.h
 * Modal dialog for activating a license on an offline (air-gapped) machine.
 *
 * Flow: the user obtains a signed-certificate JSON blob elsewhere (e.g. from
 * another machine's dashboard, or support) and pastes it here. The blob is the
 * `{ "payload", "signature", "keyId" }` object the server would otherwise return
 * from /activate. LicenseManager::importOfflineCertificate verifies + stores it.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Licensing/LicenseManager.h"

namespace more_phi {

class MorePhiProcessor;

class LicenseImportDialog : public juce::Component
{
public:
    explicit LicenseImportDialog(MorePhiProcessor& processor);
    ~LicenseImportDialog() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void onImportClicked();
    void onCancelClicked();
    void handleImportResult(const licensing::ValidationResult& result);

    MorePhiProcessor& processor_;

    juce::Label titleLabel_;
    juce::Label descLabel_;
    juce::TextEditor certInput_;
    juce::Label statusLabel_;
    juce::TextButton importBtn_ {"Import Certificate"};
    juce::TextButton cancelBtn_ {"Cancel"};

    bool isImporting_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LicenseImportDialog)
};

} // namespace more_phi
