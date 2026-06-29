/*
 * More-Phi — Licensing/LicenseEnvelopeCrypto.h
 *
 * At-rest encryption for the persisted license certificate (W4 / R2 hardening).
 *
 * Threat model: the previous SecureLicenseStore wrote the signed certificate as
 * PLAINTEXT JSON to %APPDATA%/MorePhi/Licensing/. The cert is Ed25519-signed and
 * machine-hash-bound (so it can't be FORGED or used on another machine), but it
 * WAS copyable as-is onto a second instance that happened to share the same
 * machine hash, and the payload (licenseId, features, expiry) was world-readable.
 *
 * This helper wraps the cert JSON in a versioned encrypted envelope:
 *   { "schema": 2, "method": "<dpapi|keychain|blowfish>", "enc": "<base64>" }
 *
 * Key hierarchy:
 *   - PRIMARY (Windows):  DPAPI CryptProtectData — key derived from the user's
 *                          Windows credentials, never leaves the OS. Strongest.
 *   - PRIMARY (macOS):     a key stored in the user Keychain under a fixed service
 *                          name; the cert envelope is BlowFish-encrypted with it.
 *   - FALLBACK (any OS):   BlowFish keyed by SHA-256(machineHash + appSalt).
 *                          Machine-bound (won't decrypt on a different machine)
 *                          and obfuscates the payload, though it is reversible by
 *                          a determined local attacker. Used when DPAPI/Keychain
 *                          are unavailable (headless CI, Linux, Wine).
 *
 * Legacy plaintext certs (schema 1 / raw JSON) are detected on load and
 * transparently re-saved in the encrypted envelope by SecureLicenseStore, so
 * existing activations migrate with no user action.
 */
#pragma once

#include <juce_core/juce_core.h>

#include <string>

namespace more_phi::licensing {

class LicenseEnvelopeCrypto
{
public:
    // Encrypt the UTF-8 cert JSON. Returns the method tag used ("dpapi",
    // "keychain", or "blowfish") via outMethod. Returns empty string on failure.
    static std::string encrypt(const juce::String& plaintextJson,
                               const juce::String& machineHash,
                               std::string& outMethod);

    // Decrypt a payload previously produced by encrypt(). methodTag must be the
    // tag returned at encryption time (read from the envelope). Returns empty
    // juce::String on failure (wrong machine, tampered payload, or OS refusal).
    static juce::String decrypt(const std::string& methodTag,
                                const juce::String& base64Ciphertext,
                                const juce::String& machineHash);

    // The compile-time application salt mixed into the fallback BlowFish key.
    // Not secret (in the binary), but raises the bar vs a verbatim machineHash
    // guess and ties the envelope to this product.
    static constexpr const char* kAppSalt = "morephi-licensing-envelope-v2";

    // Keychain service/account names (macOS primary path).
    static constexpr const char* kKeychainService = "com.morephi.licensing";
    static constexpr const char* kKeychainAccount = "license-envelope-key";
};

} // namespace more_phi::licensing
