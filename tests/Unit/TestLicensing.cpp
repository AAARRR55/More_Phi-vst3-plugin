#include <catch2/catch_test_macros.hpp>

#include "Licensing/LicenseKey.h"
#include "Licensing/LicenseManager.h"

#include <nlohmann/json.hpp>

using namespace more_phi::licensing;

namespace {

std::string base64Url(const std::string& text)
{
    auto encoded = juce::Base64::toBase64(text.data(), text.size()).toStdString();
    while (!encoded.empty() && encoded.back() == '=')
        encoded.pop_back();
    for (auto& c : encoded)
    {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return encoded;
}

juce::String makeValidLicenseKey()
{
    const juce::String body = "MPH1-9K4M-Q7XN-2R8D-HW6T-3PZA";
    juce::String key = body;
    key << "-" << juce::String::charToString(LicenseKey::checksumChar(LicenseKey::checksumIndex(body)));
    return key;
}

SignedCertificate makeCertificate(const juce::String& machineHash, int64_t now)
{
    nlohmann::json payload;
    payload["schema"] = LICENSE_SCHEMA;
    payload["licenseId"] = "lic_test";
    payload["activationId"] = "act_test";
    payload["productId"] = PRODUCT_ID;
    payload["licenseType"] = "perpetual";
    payload["features"] = { "morph_pad", "plugin_hosting", "mcp" };
    payload["issuedAtUnix"] = now - 60;
    payload["validFromUnix"] = now - 60;
    payload["validUntilUnix"] = nullptr;
    payload["trialEndsAtUnix"] = nullptr;
    payload["subscriptionEndsAtUnix"] = nullptr;
    payload["leaseEndsAtUnix"] = nullptr;
    payload["nextOnlineCheckAtUnix"] = now + 3600;
    payload["offlineGraceEndsAtUnix"] = now + 7200;
    payload["machineHash"] = machineHash.toStdString();
    payload["nonce"] = "nonce_test";

    SignedCertificate cert;
    cert.payloadBase64Url = base64Url(payload.dump());
    cert.signatureBase64Url = base64Url("dev-signature");
    cert.keyId = "dev-key";
    return cert;
}

} // namespace

TEST_CASE("License key parser normalizes and verifies checksum", "[licensing]")
{
    const auto key = makeValidLicenseKey();
    const auto parsed = LicenseKey::parse(key.toLowerCase().replace("-", " "));

    REQUIRE(parsed.valid);
    REQUIRE(parsed.normalized.startsWith("MPH1-"));
}

TEST_CASE("License key parser rejects checksum mismatch", "[licensing]")
{
    auto key = makeValidLicenseKey();
    const auto replacement = key.endsWithChar('0') ? "1" : "0";
    key = key.substring(0, key.length() - 1) + replacement;

    const auto parsed = LicenseKey::parse(key);
    REQUIRE_FALSE(parsed.valid);
}

TEST_CASE("License verifier validates trusted development certificate", "[licensing]")
{
    const juce::String machineHash = "machine-test";
    const int64_t now = 1800000000;
    auto cert = makeCertificate(machineHash, now);

    LicenseVerifier verifier;
    verifier.addTrustedDevelopmentSignature("dev-key", "dev-signature");

    const auto result = verifier.validateCertificate(cert, machineHash, now);
    REQUIRE(result.state == LicenseState::Licensed);
    REQUIRE(result.enablesPremiumFeatures);
    REQUIRE((result.featureMask & static_cast<uint32_t>(Feature::MCP)) != 0u);
}

TEST_CASE("License verifier rejects machine mismatch", "[licensing]")
{
    const int64_t now = 1800000000;
    auto cert = makeCertificate("machine-a", now);

    LicenseVerifier verifier;
    verifier.addTrustedDevelopmentSignature("dev-key", "dev-signature");

    const auto result = verifier.validateCertificate(cert, "machine-b", now);
    REQUIRE(result.state == LicenseState::ActivationRequired);
    REQUIRE_FALSE(result.enablesPremiumFeatures);
}
