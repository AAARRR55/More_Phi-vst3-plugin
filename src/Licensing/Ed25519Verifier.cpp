/*
 * More-Phi — Licensing/Ed25519Verifier.cpp
 */
#include "Ed25519Verifier.h"

#include "SigningKeys.h"

#if __has_include(<ed25519.h>)
#  define MORE_PHI_HAVE_ED25519 1
#  include <ed25519.h> // vendored public-domain Ed25519 (verify path)
#else
#  define MORE_PHI_HAVE_ED25519 0
#endif

namespace more_phi::licensing {

namespace {

#if MORE_PHI_HAVE_ED25519

// The vendored ed25519.h has no global init requirement (unlike libsodium), so
// verification is a pure function. ed25519_verify returns 1 on a valid
// signature, 0 otherwise.
bool verifyWithEd25519(const Ed25519PublicKey& publicKey,
                       const std::vector<uint8_t>& message,
                       const std::vector<uint8_t>& signature) noexcept
{
    // Ed25519 signatures are exactly 64 bytes.
    if (signature.size() != 64u)
        return false;

    return ed25519_verify(signature.data(),
                          message.data(),
                          message.size(),
                          publicKey.data()) != 0;
}

#endif // MORE_PHI_HAVE_ED25519

} // namespace

LicenseVerifier::SignatureVerifier makeEd25519SignatureVerifier()
{
    return [](const std::string& keyId,
              const std::vector<uint8_t>& payload,
              const std::vector<uint8_t>& signature) -> bool
    {
        const auto* publicKey = publicKeyForKeyId(keyId);
        if (publicKey == nullptr)
            return false; // Unknown key id — never accept.

#if MORE_PHI_HAVE_ED25519
        return verifyWithEd25519(*publicKey, payload, signature);
#else
        // No crypto backend compiled in. Fail closed: the dev-signature bypass
        // (wired separately in LicenseManager, debug-only) is the only path
        // that can succeed in this configuration, which is correct for a
        // stripped-down build but must never grant a production cert.
        (void) payload;
        (void) signature;
        return false;
#endif
    };
}

} // namespace more_phi::licensing
