/*
 * More-Phi — Licensing/Ed25519Verifier.h
 * Real Ed25519 signature verification for activation certificates.
 *
 * The server signs the UTF-8 bytes of the canonical certificate JSON with an
 * Ed25519 private key (store-backend/src/lib/crypto.ts: sign(null, payload, key))
 * and base64url-encodes the 64-byte signature. The plugin base64url-decodes the
 * certificate's `payload` and `signature` fields (done by LicenseVerifier) and
 * hands the raw bytes here.
 *
 * Verification is a pure function of (keyId → public key, message, signature):
 * no canonicalisation is needed on the plugin side because the server sends the
 * exact bytes it signed.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LicenseVerifier.h" // SignatureVerifier typedef

namespace more_phi::licensing {

// Build the production SignatureVerifier. Returns a callable suitable for
// LicenseVerifier::setSignatureVerifier(). When libsodium is available at
// compile time it performs real crypto_sign_verify_detached; when not, it
// rejects every signature (fail-closed) so a missing crypto lib can never
// grant access.
LicenseVerifier::SignatureVerifier makeEd25519SignatureVerifier();

} // namespace more_phi::licensing
