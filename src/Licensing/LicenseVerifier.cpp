#include "LicenseVerifier.h"

#include <sstream>

namespace more_phi::licensing {
namespace {

constexpr int64_t CLOCK_SKEW_TOLERANCE_SECONDS = 10 * 60;

std::string standardBase64FromUrl(std::string encoded)
{
    for (auto& c : encoded)
    {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }

    while ((encoded.size() % 4u) != 0u)
        encoded.push_back('=');

    return encoded;
}

std::vector<std::string> stringVectorFromJson(const nlohmann::json& json, const char* key)
{
    std::vector<std::string> values;
    if (!json.contains(key) || !json.at(key).is_array())
        return values;

    for (const auto& value : json.at(key))
        if (value.is_string())
            values.push_back(value.get<std::string>());

    return values;
}

ValidationResult makeResult(LicenseState state, juce::String message, const LicensePayload& payload = {})
{
    const bool licensed = state == LicenseState::Licensed ||
                          state == LicenseState::TrialActive ||
                          state == LicenseState::GracePeriod;
    return {
        state,
        licensed,
        licensed ? payload.featureMask : 0u,
        payload.nextOnlineCheckAtUnix,
        payload.offlineGraceEndsAtUnix,
        std::move(message)
    };
}

} // namespace

LicenseVerifier::LicenseVerifier()
{
    signatureVerifier_ = [this](const std::string& keyId,
                                const std::vector<uint8_t>&,
                                const std::vector<uint8_t>& signature)
    {
        const auto it = trustedDevelopmentSignatures_.find(keyId);
        if (it == trustedDevelopmentSignatures_.end())
            return false;

        return std::string(signature.begin(), signature.end()) == it->second;
    };
}

std::vector<uint8_t> LicenseVerifier::decodeBase64Url_(const std::string& encoded)
{
    juce::MemoryOutputStream stream;
    if (!juce::Base64::convertFromBase64(stream, juce::String(standardBase64FromUrl(encoded))))
        return {};

    const auto* data = static_cast<const uint8_t*>(stream.getData());
    return { data, data + stream.getDataSize() };
}

std::optional<int64_t> LicenseVerifier::optionalUnix_(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || json.at(key).is_null())
        return std::nullopt;

    if (json.at(key).is_number_integer())
        return json.at(key).get<int64_t>();

    return std::nullopt;
}

int64_t LicenseVerifier::requiredUnix_(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || !json.at(key).is_number_integer())
        return 0;

    return json.at(key).get<int64_t>();
}

bool LicenseVerifier::verifySignature_(const SignedCertificate& certificate,
                                       const std::vector<uint8_t>& payloadBytes,
                                       juce::String& error) const
{
    if (certificate.keyId.empty())
    {
        error = "License certificate does not include a signing key id.";
        return false;
    }

    const auto signatureBytes = decodeBase64Url_(certificate.signatureBase64Url);
    if (signatureBytes.empty())
    {
        error = "License certificate signature is not valid base64.";
        return false;
    }

    // Primary path: real signature verification (Ed25519 in production builds).
    if (signatureVerifier_ && signatureVerifier_(certificate.keyId, payloadBytes, signatureBytes))
        return true;

    // Fallback: trusted development signatures. The map is only populated in
    // debug builds (LicenseManager registers "dev-key"/"dev-signature" under
    // #if !defined(NDEBUG)), so a forged dev certificate can never unlock a
    // release build. This lets a local checkout of the backend (which emits the
    // dev-signature stub instead of a real Ed25519 signature) be exercised
    // end-to-end without provisioning real key material.
    const auto it = trustedDevelopmentSignatures_.find(certificate.keyId);
    if (it != trustedDevelopmentSignatures_.end()
        && std::string(signatureBytes.begin(), signatureBytes.end()) == it->second)
        return true;

    error = "License certificate signature could not be verified.";
    return false;
}

std::optional<LicensePayload> LicenseVerifier::parsePayload(const SignedCertificate& certificate,
                                                            juce::String& error) const
{
    const auto payloadBytes = decodeBase64Url_(certificate.payloadBase64Url);
    if (payloadBytes.empty())
    {
        error = "License certificate payload is not valid base64.";
        return std::nullopt;
    }

    if (!verifySignature_(certificate, payloadBytes, error))
        return std::nullopt;

    try
    {
        const auto payloadJson = nlohmann::json::parse(std::string(payloadBytes.begin(), payloadBytes.end()));
        LicensePayload payload;
        payload.schema = payloadJson.value("schema", "");
        payload.licenseId = payloadJson.value("licenseId", "");
        payload.activationId = payloadJson.value("activationId", "");
        payload.productId = payloadJson.value("productId", "");
        payload.licenseType = licenseTypeFromString(payloadJson.value("licenseType", ""));
        payload.featureMask = featureMaskFromStrings(stringVectorFromJson(payloadJson, "features"));
        payload.issuedAtUnix = requiredUnix_(payloadJson, "issuedAtUnix");
        payload.validFromUnix = requiredUnix_(payloadJson, "validFromUnix");
        payload.validUntilUnix = optionalUnix_(payloadJson, "validUntilUnix");
        payload.trialEndsAtUnix = optionalUnix_(payloadJson, "trialEndsAtUnix");
        payload.subscriptionEndsAtUnix = optionalUnix_(payloadJson, "subscriptionEndsAtUnix");
        payload.leaseEndsAtUnix = optionalUnix_(payloadJson, "leaseEndsAtUnix");
        payload.nextOnlineCheckAtUnix = requiredUnix_(payloadJson, "nextOnlineCheckAtUnix");
        payload.offlineGraceEndsAtUnix = requiredUnix_(payloadJson, "offlineGraceEndsAtUnix");
        payload.machineHash = payloadJson.value("machineHash", "");
        payload.nonce = payloadJson.value("nonce", "");
        return payload;
    }
    catch (const std::exception& e)
    {
        error = juce::String("License certificate payload is not valid JSON: ") + e.what();
    }

    return std::nullopt;
}

ValidationResult LicenseVerifier::validateCertificate(const SignedCertificate& certificate,
                                                      const juce::String& expectedMachineHash,
                                                      int64_t nowUnixSeconds) const
{
    juce::String error;
    const auto maybePayload = parsePayload(certificate, error);
    if (!maybePayload)
    {
        lastValidatedPayload_.reset();
        return { LicenseState::Invalid, false, 0u, 0, 0, error };
    }

    const auto& payload = *maybePayload;
    lastValidatedPayload_ = payload;

    if (payload.schema != LICENSE_SCHEMA)
        return makeResult(LicenseState::Invalid, "License certificate schema is unsupported.", payload);

    if (payload.productId != PRODUCT_ID)
        return makeResult(LicenseState::Invalid, "License is for a different product.", payload);

    if (payload.validFromUnix > nowUnixSeconds + CLOCK_SKEW_TOLERANCE_SECONDS)
        return makeResult(LicenseState::Invalid, "System clock appears incorrect.", payload);

    if (expectedMachineHash.isNotEmpty() && payload.machineHash != expectedMachineHash.toStdString())
        return makeResult(LicenseState::ActivationRequired, "License is activated on another computer.", payload);

    if (payload.licenseType == LicenseType::Trial)
    {
        if (payload.trialEndsAtUnix && nowUnixSeconds <= *payload.trialEndsAtUnix)
            return makeResult(LicenseState::TrialActive, "Trial active.", payload);
        return makeResult(LicenseState::Expired, "Trial has expired.", payload);
    }

    if (payload.licenseType == LicenseType::Subscription)
    {
        if (payload.subscriptionEndsAtUnix && nowUnixSeconds <= *payload.subscriptionEndsAtUnix)
            return makeResult(LicenseState::Licensed, "Subscription active.", payload);
        if (nowUnixSeconds <= payload.offlineGraceEndsAtUnix)
            return makeResult(LicenseState::GracePeriod, "Subscription is in offline grace period.", payload);
        return makeResult(LicenseState::Expired, "Subscription expired.", payload);
    }

    if (payload.licenseType == LicenseType::Floating)
    {
        if (payload.leaseEndsAtUnix && nowUnixSeconds <= *payload.leaseEndsAtUnix)
            return makeResult(LicenseState::Licensed, "Floating lease active.", payload);
        return makeResult(LicenseState::FloatingLeaseUnavailable, "Floating license lease expired.", payload);
    }

    if (payload.licenseType == LicenseType::Perpetual || payload.licenseType == LicenseType::NodeLocked)
    {
        if (payload.validUntilUnix && nowUnixSeconds > *payload.validUntilUnix)
            return makeResult(LicenseState::Expired, "License expired.", payload);
        if (payload.nextOnlineCheckAtUnix <= 0 || nowUnixSeconds <= payload.nextOnlineCheckAtUnix)
            return makeResult(LicenseState::Licensed, "License active.", payload);
        if (nowUnixSeconds <= payload.offlineGraceEndsAtUnix)
            return makeResult(LicenseState::GracePeriod, "License is in offline validation grace period.", payload);
        return makeResult(LicenseState::ActivationRequired, "Online license validation is required.", payload);
    }

    return makeResult(LicenseState::Invalid, "Unknown license type.", payload);
}

std::optional<SignedCertificate> LicenseVerifier::parseSignedCertificateJson(const juce::String& jsonText,
                                                                             juce::String& error)
{
    try
    {
        const auto root = nlohmann::json::parse(jsonText.toStdString());
        SignedCertificate certificate;
        certificate.payloadBase64Url = root.value("payload", "");
        certificate.signatureBase64Url = root.value("signature", "");
        certificate.keyId = root.value("keyId", "");

        if (certificate.payloadBase64Url.empty() || certificate.signatureBase64Url.empty() || certificate.keyId.empty())
        {
            error = "Signed certificate JSON is missing payload, signature, or keyId.";
            return std::nullopt;
        }

        return certificate;
    }
    catch (const std::exception& e)
    {
        error = juce::String("Signed certificate JSON could not be parsed: ") + e.what();
        return std::nullopt;
    }
}

juce::String LicenseVerifier::toJson(const SignedCertificate& certificate)
{
    nlohmann::json root;
    root["payload"] = certificate.payloadBase64Url;
    root["signature"] = certificate.signatureBase64Url;
    root["keyId"] = certificate.keyId;
    return juce::String(root.dump());
}

} // namespace more_phi::licensing