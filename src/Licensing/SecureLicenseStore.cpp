#include "SecureLicenseStore.h"

namespace more_phi::licensing {

SecureLicenseStore::SecureLicenseStore()
    : storageDirectory_(defaultStorageDirectory_())
{
}

SecureLicenseStore::SecureLicenseStore(juce::File storageDirectory)
    : storageDirectory_(std::move(storageDirectory))
{
}

juce::File SecureLicenseStore::defaultStorageDirectory_()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MorePhi")
        .getChildFile("Licensing");
}

std::optional<SignedCertificate> SecureLicenseStore::loadCertificate(juce::String* error) const
{
    const auto file = getCertificateFile();
    if (!file.existsAsFile())
    {
        if (error != nullptr)
            *error = "No activation certificate has been stored.";
        return std::nullopt;
    }

    const auto text = file.loadFileAsString();
    if (text.isEmpty())
    {
        if (error != nullptr)
            *error = "Stored activation certificate is empty.";
        return std::nullopt;
    }

    juce::String parseError;
    auto cert = LicenseVerifier::parseSignedCertificateJson(text, parseError);
    if (!cert && error != nullptr)
        *error = parseError;
    return cert;
}

bool SecureLicenseStore::saveCertificate(const SignedCertificate& certificate, juce::String* error) const
{
    if (!storageDirectory_.createDirectory())
    {
        if (error != nullptr)
            *error = "Could not create license storage directory.";
        return false;
    }

    const auto file = getCertificateFile();
    const auto json = LicenseVerifier::toJson(certificate);
    if (!file.replaceWithText(json))
    {
        if (error != nullptr)
            *error = "Could not write activation certificate.";
        return false;
    }

    return true;
}

bool SecureLicenseStore::clearCertificate(juce::String* error) const
{
    const auto file = getCertificateFile();
    if (!file.exists())
        return true;

    if (!file.deleteFile())
    {
        if (error != nullptr)
            *error = "Could not remove activation certificate.";
        return false;
    }

    return true;
}

} // namespace more_phi::licensing