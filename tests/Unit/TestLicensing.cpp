#include <catch2/catch_test_macros.hpp>

#include "Licensing/LicenseKey.h"
#include "Licensing/LicenseManager.h"
#include "Licensing/ActivationClient.h"
#include "Licensing/Ed25519Verifier.h"
#include "Licensing/SigningKeys.h"

#include <nlohmann/json.hpp>

#if __has_include(<ed25519.h>)
#  define MORE_PHI_TEST_HAVE_ED25519 1
#  include <ed25519.h> // vendored public-domain Ed25519
#else
#  define MORE_PHI_TEST_HAVE_ED25519 0
#endif

#include <array>
#include <cstring>

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

TEST_CASE("License key parser accepts purchasing backend MPHI keys", "[licensing]")
{
    const auto parsed = LicenseKey::parse("mphi-abc12-def34-56789-0fabc");
    REQUIRE(parsed.valid);
    REQUIRE(parsed.normalized == "MPHI-ABC12-DEF34-56789-0FABC");
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

TEST_CASE("HTTP activation client reports missing configuration without network", "[licensing]")
{
    HttpActivationClient client(LicenseApiConfig{});

    ActivationRequest request;
    request.licenseKey = makeValidLicenseKey();
    request.machineHash = "machine-test";
    request.pluginVersion = "test";

    const auto response = client.activate(request);
    REQUIRE_FALSE(response.success);
    REQUIRE(response.errorCode == "LICENSE_API_CONFIG_MISSING");
}

// ── Error-envelope parsing (store-backend returns { "error": { code, message } }) ─

TEST_CASE("parseActivationResponse extracts nested error envelope", "[licensing]")
{
    const juce::String body = R"({"error":{"code":"NOT_FOUND","message":"License not found"}})";
    const auto r = parseActivationResponse(404, body, true);

    REQUIRE_FALSE(r.success);
    REQUIRE(r.errorCode == "NOT_FOUND");
    // The friendly mapping localises NOT_FOUND — assert the user-facing copy,
    // not the raw server string, so a backend wording change can't silently
    // surface a confusing message in the activation overlay.
    REQUIRE(r.message.contains("not found"));
}

TEST_CASE("parseActivationResponse maps activation-limit code to friendly text", "[licensing]")
{
    const juce::String body = "{\"error\":{\"code\":\"MAX_ACTIVATIONS_REACHED\",\"message\":\"Activation limit reached (3 machines)\"}}";
    const auto r = parseActivationResponse(409, body, true);

    REQUIRE_FALSE(r.success);
    REQUIRE(r.errorCode == "MAX_ACTIVATIONS_REACHED");
    REQUIRE(r.message.contains("limit"));
}

TEST_CASE("parseActivationResponse falls back to flat message when no error envelope", "[licensing]")
{
    const juce::String body = R"({"error_code":"RATE_LIMITED","message":"Too many attempts"})";
    const auto r = parseActivationResponse(429, body, true);

    REQUIRE_FALSE(r.success);
    REQUIRE(r.errorCode == "RATE_LIMITED");
    REQUIRE(r.message.contains("attempts"));
}

TEST_CASE("parseActivationResponse accepts success body with certificate", "[licensing]")
{
    // Minimal valid success envelope: status + a dev certificate object.
    nlohmann::json payload;
    payload["schema"] = LICENSE_SCHEMA;
    payload["licenseId"] = "lic";
    payload["activationId"] = "act";
    payload["productId"] = PRODUCT_ID;
    payload["licenseType"] = "perpetual";
    payload["features"] = nlohmann::json::array({ "morph_pad" });
    payload["issuedAtUnix"] = 1'800'000'000;
    payload["validFromUnix"] = 1'800'000'000;
    payload["validUntilUnix"] = nullptr;
    payload["trialEndsAtUnix"] = nullptr;
    payload["subscriptionEndsAtUnix"] = nullptr;
    payload["leaseEndsAtUnix"] = nullptr;
    payload["nextOnlineCheckAtUnix"] = 1'800'000'000 + 3600;
    payload["offlineGraceEndsAtUnix"] = 1'800'000'000 + 7200;
    payload["machineHash"] = "machine-test";
    payload["nonce"] = "n";

    nlohmann::json body;
    body["status"] = "ACTIVE";
    body["activation_id"] = "act";
    body["license_id"] = "lic";
    body["activations_used"] = 1;
    body["max_activations"] = 3;
    body["certificate"]["payload"] = base64Url(payload.dump());
    body["certificate"]["signature"] = base64Url("dev-signature");
    body["certificate"]["keyId"] = "dev-key";

    const auto r = parseActivationResponse(200, juce::String(body.dump()), true);
    REQUIRE(r.success);
    REQUIRE(r.certificate.has_value());
    REQUIRE(r.certificate->keyId == "dev-key");
}

// ── Ed25519 verification (real signature math) ────────────────────────────────
// The production verifier (makeEd25519SignatureVerifier) looks keys up in the
// compiled-in SigningKeys table, so we install a test verifier that uses a
// freshly generated keypair — exercising the exact Ed25519 path the plugin runs
// in production, just with test-controlled key material.

#if MORE_PHI_TEST_HAVE_ED25519

namespace {

struct TestEd25519Key
{
    std::array<uint8_t, 32> publicKey {};
    std::array<uint8_t, 64> privateKey {}; // orlp/ed25519: 64-byte expanded secret
};

TestEd25519Key makeTestKey()
{
    // orlp/ed25519 derives the keypair from a 32-byte seed. Use a fixed seed so
    // the test is deterministic; either way the tamper/wrong-key cases below
    // prove the verify path, not the keypair randomness.
    TestEd25519Key k;
    std::array<uint8_t, 32> seed {};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i + 1);
    ed25519_create_keypair(k.publicKey.data(), k.privateKey.data(), seed.data());
    return k;
}

SignedCertificate makeCertificateSignedWith(const TestEd25519Key& key,
                                             const juce::String& machineHash,
                                             int64_t now,
                                             bool tamperPayload)
{
    nlohmann::json payload;
    payload["schema"] = LICENSE_SCHEMA;
    payload["licenseId"] = "lic_test";
    payload["activationId"] = "act_test";
    payload["productId"] = PRODUCT_ID;
    payload["licenseType"] = "perpetual";
    payload["features"] = nlohmann::json::array({ "morph_pad", "plugin_hosting", "mcp" });
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

    const std::string signedPayload = payload.dump();
    const std::string verifyPayload = tamperPayload ? (signedPayload + " ") : signedPayload;

    // Sign the ORIGINAL payload, so the tampered case proves the signature does
    // not match the mutated bytes (not that we signed the mutation).
    std::array<uint8_t, 64> signature {};
    ed25519_sign(signature.data(),
                 reinterpret_cast<const unsigned char*>(signedPayload.data()),
                 signedPayload.size(),
                 key.publicKey.data(),
                 key.privateKey.data());

    SignedCertificate cert;
    cert.payloadBase64Url = base64Url(verifyPayload);
    cert.signatureBase64Url = base64Url(std::string(reinterpret_cast<const char*>(signature.data()), signature.size()));
    cert.keyId = "test-key";
    return cert;
}

LicenseVerifier::SignatureVerifier makeTestVerifier(const TestEd25519Key& key)
{
    return [key](const std::string& /*keyId*/,
                 const std::vector<uint8_t>& payload,
                 const std::vector<uint8_t>& signature) -> bool
    {
        if (signature.size() != 64u)
            return false;
        return ed25519_verify(signature.data(),
                              payload.data(),
                              payload.size(),
                              key.publicKey.data()) != 0;
    };
}

} // namespace

TEST_CASE("Ed25519 verifier accepts a correctly signed certificate", "[licensing][crypto]")
{
    const auto key = makeTestKey();
    const juce::String machineHash = "machine-ed25519";
    const int64_t now = 1'800'000'000;
    auto cert = makeCertificateSignedWith(key, machineHash, now, /*tamperPayload=*/false);

    LicenseVerifier verifier;
    verifier.setSignatureVerifier(makeTestVerifier(key));

    const auto result = verifier.validateCertificate(cert, machineHash, now);
    REQUIRE(result.state == LicenseState::Licensed);
    REQUIRE(result.enablesPremiumFeatures);
}

TEST_CASE("Ed25519 verifier rejects a tampered payload", "[licensing][crypto]")
{
    const auto key = makeTestKey();
    const juce::String machineHash = "machine-ed25519";
    const int64_t now = 1'800'000'000;
    auto cert = makeCertificateSignedWith(key, machineHash, now, /*tamperPayload=*/true);

    LicenseVerifier verifier;
    verifier.setSignatureVerifier(makeTestVerifier(key));

    const auto result = verifier.validateCertificate(cert, machineHash, now);
    REQUIRE_FALSE(result.enablesPremiumFeatures);
    REQUIRE(result.state == LicenseState::Invalid);
}

TEST_CASE("Ed25519 verifier rejects signature from a different key", "[licensing][crypto]")
{
    const auto signingKey = makeTestKey();
    auto otherKey = makeTestKey(); // different seed -> different keypair
    // Make the keys actually differ: mutate the other key's bytes.
    otherKey.privateKey[0] ^= 0xFF;
    otherKey.publicKey[0]  ^= 0xFF;

    const juce::String machineHash = "machine-ed25519";
    const int64_t now = 1'800'000'000;
    auto cert = makeCertificateSignedWith(signingKey, machineHash, now, false);

    LicenseVerifier verifier;
    verifier.setSignatureVerifier(makeTestVerifier(otherKey)); // wrong pubkey

    const auto result = verifier.validateCertificate(cert, machineHash, now);
    REQUIRE_FALSE(result.enablesPremiumFeatures);
}

#else

TEST_CASE("Ed25519 verifier fails closed when crypto backend is unavailable", "[licensing][crypto]")
{
    // makeEd25519SignatureVerifier() returns a callable that always rejects in
    // a crypto-less build. A real signature path is impossible here, so we just
    // assert the fail-closed contract: no cert verifies.
    auto verifierFn = makeEd25519SignatureVerifier();
    std::vector<uint8_t> msg { 'a' };
    std::vector<uint8_t> sig (64, 0);
    REQUIRE_FALSE(verifierFn("prod-ed25519-2026-01", msg, sig));
}

#endif // MORE_PHI_TEST_HAVE_ED25519

TEST_CASE("Bundled production public key parses from hex", "[licensing][crypto]")
{
    // The compiled-in prod key must round-trip through the hex parser. This
    // guards against a malformed constant in SigningKeys.cpp.
    const auto* pub = publicKeyForKeyId("prod-ed25519-2026-01");
    REQUIRE(pub != nullptr);

    // Re-derive its hex and parse it back.
    std::string hex;
    static const char* digits = "0123456789abcdef";
    for (auto b : *pub)
    {
        hex += digits[(b >> 4) & 0x0f];
        hex += digits[b & 0x0f];
    }
    Ed25519PublicKey reparsed {};
    REQUIRE(parseEd25519PublicKeyHex(hex, reparsed));
    REQUIRE(reparsed == *pub);
}

TEST_CASE("Unknown keyId resolves to no public key", "[licensing][crypto]")
{
    REQUIRE(publicKeyForKeyId("does-not-exist") == nullptr);
}

// ── Verifier: lastValidatedPayload exposes the parsed activationId ───────────
// LicenseManager reads activationId from this (via publish_) so refresh and
// deactivate can address the right server-side activation without re-parsing
// the on-disk certificate. Verify the seam directly to avoid touching disk.

TEST_CASE("LicenseVerifier exposes last validated payload's activationId", "[licensing]")
{
    const juce::String machineHash = "machine-capture";
    const int64_t now = 1'800'000'000;

    nlohmann::json payload;
    payload["schema"] = LICENSE_SCHEMA;
    payload["licenseId"] = "lic_capture";
    payload["activationId"] = "act_capture_unique";
    payload["productId"] = PRODUCT_ID;
    payload["licenseType"] = "perpetual";
    payload["features"] = nlohmann::json::array({ "morph_pad" });
    payload["issuedAtUnix"] = now - 60;
    payload["validFromUnix"] = now - 60;
    payload["validUntilUnix"] = nullptr;
    payload["trialEndsAtUnix"] = nullptr;
    payload["subscriptionEndsAtUnix"] = nullptr;
    payload["leaseEndsAtUnix"] = nullptr;
    payload["nextOnlineCheckAtUnix"] = now + 3600;
    payload["offlineGraceEndsAtUnix"] = now + 7200;
    payload["machineHash"] = machineHash.toStdString();
    payload["nonce"] = "n";

    SignedCertificate cert;
    cert.payloadBase64Url = base64Url(payload.dump());
    cert.signatureBase64Url = base64Url("dev-signature");
    cert.keyId = "dev-key";

    LicenseVerifier verifier;
    verifier.addTrustedDevelopmentSignature("dev-key", "dev-signature");

    REQUIRE_FALSE(verifier.lastValidatedPayload().has_value()); // nothing validated yet

    const auto result = verifier.validateCertificate(cert, machineHash, now);
    REQUIRE(result.enablesPremiumFeatures);

    const auto& last = verifier.lastValidatedPayload();
    REQUIRE(last.has_value());
    REQUIRE(last->activationId == "act_capture_unique");
    REQUIRE(last->nextOnlineCheckAtUnix == now + 3600);
}
