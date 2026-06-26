/*
 * More-Phi — Licensing/SecureLicenseStore.cpp
 *
 * Persistent license certificate store. The certificate is Ed25519-signed and
 * machine-hash-bound at the protocol layer; this class adds at-rest encryption
 * of the persisted file (W4 / R2) so the payload is not world-readable and a
 * verbatim file copy won't decrypt on a machine with a different fingerprint /
 * Windows user.
 *
 * File format — versioned envelope:
 *   {
 *     "schema": 2,
 *     "method": "dpapi" | "keychain" | "blowfish",
 *     "enc": "<base64 ciphertext>"
 *   }
 *
 * Legacy plaintext (schema 1 / raw signed-cert JSON) is detected on load and
 * transparently re-saved as an encrypted schema-2 envelope, so existing
 * activations migrate with no user action. On failure to decrypt (wrong user,
 * wrong machine, corrupted file) the cert is reported absent — the activation
 * flow then re-activates against the backend.
 */
#include "SecureLicenseStore.h"

#include "LicenseEnvelopeCrypto.h"
#include "MachineFingerprint.h"

#include <nlohmann/json.hpp>

namespace more_phi::licensing {

namespace {

constexpr int kCurrentStoreSchema = 2;

// Try to parse @p text as the encrypted envelope (schema 2). On success, fills
// outMethod + outCiphertextB64 and returns true. Returns false if the text is
// not a schema-2 envelope (caller will then treat it as legacy plaintext).
bool parseEnvelope(const juce::String& text, std::string& outMethod, juce::String& outCiphertextB64)
{
    try
    {
        const auto root = nlohmann::json::parse(text.toStdString());
        if (! root.is_object())
            return false;
        const auto schema = root.value("schema", 0);
        if (schema != kCurrentStoreSchema)
            return false;
        outMethod = root.value("method", "");
        outCiphertextB64 = juce::String(root.value("enc", ""));
        return ! outMethod.empty() && outCiphertextB64.isNotEmpty();
    }
    catch (...)
    {
        return false;
    }
}

juce::String machineHashForBinding()
{
    return MachineFingerprint::computeMachineHash();
}

} // namespace

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
    if (! file.existsAsFile())
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

    // ── Schema-2 encrypted envelope (current) ────────────────────────────────
    std::string method;
    juce::String ciphertextB64;
    if (parseEnvelope(text, method, ciphertextB64))
    {
        const auto plaintext = LicenseEnvelopeCrypto::decrypt(method, ciphertextB64,
                                                              machineHashForBinding());
        if (plaintext.isEmpty())
        {
            // Wrong machine / wrong Windows user / corrupted. Treat as absent so
            // the activation flow re-activates rather than reporting a forgery.
            if (error != nullptr)
                *error = "Stored certificate could not be decrypted on this machine "
                         "(it may have been activated under a different user account).";
            return std::nullopt;
        }
        juce::String parseError;
        auto cert = LicenseVerifier::parseSignedCertificateJson(plaintext, parseError);
        if (! cert)
        {
            if (error != nullptr)
                *error = parseError;
        }
        return cert;
    }

    // ── Legacy plaintext (schema 1 / raw JSON) — load + flag for re-save ──────
    juce::String parseError;
    auto cert = LicenseVerifier::parseSignedCertificateJson(text, parseError);
    if (! cert)
    {
        if (error != nullptr)
            *error = parseError;
        return std::nullopt;
    }
    // Transparent migration: re-save encrypted. Best-effort; if the directory is
    // read-only we still return the loaded cert (read path must not fail just
    // because the upgrade write can't happen).
    juce::String migrateError;
    saveCertificate(*cert, &migrateError);
    return cert;
}

bool SecureLicenseStore::saveCertificate(const SignedCertificate& certificate, juce::String* error) const
{
    if (! storageDirectory_.createDirectory())
    {
        if (error != nullptr)
            *error = "Could not create license storage directory.";
        return false;
    }

    // Serialize the signed cert to JSON, then wrap in the encrypted envelope.
    const auto plaintext = LicenseVerifier::toJson(certificate);
    std::string method;
    const auto ciphertextB64 = LicenseEnvelopeCrypto::encrypt(plaintext,
                                                              machineHashForBinding(),
                                                              method);
    if (ciphertextB64.empty())
    {
        // Should be vanishingly rare (BlowFish always succeeds). Fail closed
        // rather than writing plaintext — a plaintext write would regress W4.
        if (error != nullptr)
            *error = "Could not encrypt the activation certificate for storage.";
        return false;
    }

    nlohmann::json envelope;
    envelope["schema"] = kCurrentStoreSchema;
    envelope["method"] = method;
    envelope["enc"] = ciphertextB64;

    const auto file = getCertificateFile();
    if (! file.replaceWithText(juce::String(envelope.dump())))
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
    if (! file.exists())
        return true;

    // Overwrite before delete so the ciphertext can't be recovered from disk
    // by a forensic tool after the user deactivates.
    file.replaceWithData("", 0);

    if (! file.deleteFile())
    {
        if (error != nullptr)
            *error = "Could not remove activation certificate.";
        return false;
    }

    return true;
}

} // namespace more_phi::licensing
