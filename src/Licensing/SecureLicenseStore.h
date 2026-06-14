/*
 * More-Phi — Licensing/SecureLicenseStore.h
 * Per-user license certificate persistence outside DAW project state.
 */
#pragma once

#include "LicenseVerifier.h"

namespace more_phi::licensing {

class SecureLicenseStore
{
public:
    SecureLicenseStore();
    explicit SecureLicenseStore(juce::File storageDirectory);

    std::optional<SignedCertificate> loadCertificate(juce::String* error = nullptr) const;
    bool saveCertificate(const SignedCertificate& certificate, juce::String* error = nullptr) const;
    bool clearCertificate(juce::String* error = nullptr) const;

    juce::File getCertificateFile() const { return storageDirectory_.getChildFile("activation-cert.json"); }
    juce::File getStorageDirectory() const { return storageDirectory_; }

private:
    static juce::File defaultStorageDirectory_();

    juce::File storageDirectory_;
};

} // namespace more_phi::licensing