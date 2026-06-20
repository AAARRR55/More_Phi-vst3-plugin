/*
 * More-Phi — Licensing/LicenseVerifier.h
 * Signed certificate parsing and policy validation.
 */
#pragma once

#include <functional>
#include <map>

#include <nlohmann/json.hpp>

#include "LicenseTypes.h"

namespace more_phi::licensing {

class LicenseVerifier
{
public:
    using SignatureVerifier = std::function<bool(const std::string& keyId,
                                                 const std::vector<uint8_t>& payload,
                                                 const std::vector<uint8_t>& signature)>;

    LicenseVerifier();

    void setSignatureVerifier(SignatureVerifier verifier) { signatureVerifier_ = std::move(verifier); }
    void addTrustedDevelopmentSignature(const std::string& keyId, const std::string& signature) { trustedDevelopmentSignatures_[keyId] = signature; }

    std::optional<LicensePayload> parsePayload(const SignedCertificate& certificate, juce::String& error) const;
    ValidationResult validateCertificate(const SignedCertificate& certificate,
                                         const juce::String& expectedMachineHash,
                                         int64_t nowUnixSeconds) const;

    // The payload from the most recent successful validateCertificate() call on
    // this verifier. Stable until the next validateCertificate(). Used by the
    // manager to read the activationId / next-check time without re-parsing.
    const std::optional<LicensePayload>& lastValidatedPayload() const noexcept { return lastValidatedPayload_; }

    static std::optional<SignedCertificate> parseSignedCertificateJson(const juce::String& jsonText,
                                                                       juce::String& error);
    static juce::String toJson(const SignedCertificate& certificate);

private:
    bool verifySignature_(const SignedCertificate& certificate,
                          const std::vector<uint8_t>& payloadBytes,
                          juce::String& error) const;

    static std::vector<uint8_t> decodeBase64Url_(const std::string& encoded);
    static std::optional<int64_t> optionalUnix_(const nlohmann::json& json, const char* key);
    static int64_t requiredUnix_(const nlohmann::json& json, const char* key);

    SignatureVerifier signatureVerifier_;
    std::map<std::string, std::string> trustedDevelopmentSignatures_;
    mutable std::optional<LicensePayload> lastValidatedPayload_;
};

} // namespace more_phi::licensing