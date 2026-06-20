/*
 * More-Phi — Licensing/LicenseManager.h
 * Non-audio-thread license orchestration and runtime state publication.
 */
#pragma once

#include <memory>

#include "ActivationClient.h"
#include "LicenseKey.h"
#include "LicenseVerifier.h"
#include "MachineFingerprint.h"
#include "SecureLicenseStore.h"

namespace more_phi::licensing {

class LicenseManager
{
public:
    explicit LicenseManager(LicenseRuntimeState& runtimeState);
    LicenseManager(LicenseRuntimeState& runtimeState,
                   SecureLicenseStore store,
                   std::unique_ptr<IActivationClient> activationClient);

    ValidationResult loadCachedCertificate();
    ValidationResult activateWithKey(const juce::String& licenseKey,
                                     const juce::String& pluginVersion,
                                     const juce::String& dawHint = {});
    ValidationResult refreshActivation(const juce::String& activationId);
    bool deactivateActivation(const juce::String& activationId, juce::String* error = nullptr);
    ValidationResult importOfflineCertificate(const juce::String& signedCertificateJson);
    bool clearActivation(juce::String* error = nullptr);

    bool storeVerifiedCertificate(const SignedCertificate& certificate, ValidationResult& result);

    LicenseRuntimeState& getRuntimeState() noexcept { return runtimeState_; }
    const LicenseRuntimeState& getRuntimeState() const noexcept { return runtimeState_; }
    LicenseVerifier& getVerifier() noexcept { return verifier_; }
    const LicenseVerifier& getVerifier() const noexcept { return verifier_; }
    juce::String getMachineHash() const { return machineHash_; }
    juce::String getLastMessage() const { return lastMessage_; }

    // The activationId from the most recently validated certificate. Used by the
    // refresh/deactivate flows and surfaced to the UI. Empty if no certificate
    // has been validated in this session or the store is empty.
    juce::String lastActivationId() const { return lastActivationId_; }

    static int64_t nowUnixSeconds();
    static juce::String createRequestNonce();

private:
    void publish_(const ValidationResult& result);

    LicenseRuntimeState& runtimeState_;
    SecureLicenseStore store_;
    LicenseVerifier verifier_;
    std::unique_ptr<IActivationClient> activationClient_;
    juce::String machineHash_;
    juce::String lastMessage_;
    juce::String lastActivationId_;
};

} // namespace more_phi::licensing