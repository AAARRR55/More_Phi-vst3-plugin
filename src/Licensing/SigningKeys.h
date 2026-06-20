/*
 * More-Phi — Licensing/SigningKeys.h
 * Compiled-in Ed25519 public keys, selected by certificate keyId.
 *
 * The license server signs each activation certificate with an Ed25519 private
 * key and stamps the matching keyId into the certificate. The plugin must know
 * the corresponding PUBLIC key to verify the signature offline (no endpoint
 * serves the public key — confirmed against store-backend/src/lib/crypto.ts).
 *
 * Key rotation: mint a new keypair, set the server's LICENSE_SIGNING_KEY_ID to
 * a new id (e.g. "prod-ed25519-2028-01"), add its public key here, and ship.
 * Old ids stay valid until their certs age out via nextOnlineCheckAtUnix.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace more_phi::licensing {

// A 32-byte raw Ed25519 public key.
using Ed25519PublicKey = std::array<uint8_t, 32>;

// Look up the public key for a given keyId. Returns nullptr if the id is not
// recognised (the caller treats that as a verification failure).
const Ed25519PublicKey* publicKeyForKeyId(std::string_view keyId) noexcept;

// Helpers used by tests/dev tooling: build the canonical 32-byte key from a hex
// string (64 hex chars, no separators).
bool parseEd25519PublicKeyHex(std::string_view hex64, Ed25519PublicKey& out) noexcept;

} // namespace more_phi::licensing
