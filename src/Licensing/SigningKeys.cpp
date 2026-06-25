/*
 * More-Phi — Licensing/SigningKeys.cpp
 */
#include "SigningKeys.h"

#include <map>
#include <string>

namespace more_phi::licensing {
namespace {

// ── Production Ed25519 public key(s) ─────────────────────────────────────────
// Each entry corresponds to a keyId the server may stamp into certificates.
// The public key below is derived (via `openssl`/Node crypto) from the private
// key configured on the licensing server for that keyId.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  RELEASE BUILDS: replace the placeholder below with the REAL production │
// │  Ed25519 public key (keyId "prod-ed25519-2026-01") before shipping.      │
// │  The placeholder is the DEV keypair's public half — fine for local      │
// │  end-to-end testing against a server whose .env uses the matching key.   │
// └─────────────────────────────────────────────────────────────────────────┘
//
// 32 raw bytes, big-endian (standard Ed25519 public key encoding).
// Hex (no separators): 8e1aa28c35a94bf3c6e555fadb5ba34c5b4880715665c84dfc379e358a52ac9f
constexpr Ed25519PublicKey kProdEd25519_2026_01 = {
    0x8e, 0x1a, 0xa2, 0x8c, 0x35, 0xa9, 0x4b, 0xf3,
    0xc6, 0xe5, 0x55, 0xfa, 0xdb, 0x5b, 0xa3, 0x4c,
    0x5b, 0x48, 0x80, 0x71, 0x56, 0x65, 0xc8, 0x4d,
    0xfc, 0x37, 0x9e, 0x35, 0x8a, 0x52, 0xac, 0x9f,
};

// ── AUDIT-FIX: ship-blocker guard ────────────────────────────────────────────
// The key above is the DEV keypair's public half. As long as it stays here, any
// certificate signed by the dev backend verifies as a production license. This
// constexpr predicate detects the placeholder so a static_assert can refuse to
// compile a real shipping build until the key is rotated.
constexpr bool isDevPlaceholderKey(const Ed25519PublicKey& k) noexcept
{
    constexpr Ed25519PublicKey kKnownDev = {
        0x8e, 0x1a, 0xa2, 0x8c, 0x35, 0xa9, 0x4b, 0xf3,
        0xc6, 0xe5, 0x55, 0xfa, 0xdb, 0x5b, 0xa3, 0x4c,
        0x5b, 0x48, 0x80, 0x71, 0x56, 0x65, 0xc8, 0x4d,
        0xfc, 0x37, 0x9e, 0x35, 0x8a, 0x52, 0xac, 0x9f,
    };
    for (std::size_t i = 0; i < kKnownDev.size(); ++i)
        if (k[i] != kKnownDev[i]) return false;
    return true;
}

// A genuine shipping plugin build (Release, NDEBUG defined, not a test build)
// MUST NOT carry the dev key. Tests (MORE_PHI_TEST_MODE) and Debug builds are
// exempt so end-to-end dev flows still work. Flip this to fail by replacing
// kProdEd25519_2026_01 with the real production public key.
#if defined(NDEBUG) && !defined(MORE_PHI_TEST_MODE)
static_assert(! isDevPlaceholderKey(kProdEd25519_2026_01),
    "RELEASE BUILD BLOCKED: SigningKeys.cpp still contains the DEV Ed25519 "
    "public key. Replace kProdEd25519_2026_01 with the real production key "
    "(keyId prod-ed25519-2026-01) before building a shipping Release.");
#endif

const std::map<std::string, const Ed25519PublicKey*>& knownKeys()
{
    // Constructed once on first use; pointers to the constexpr arrays above.
    static const std::map<std::string, const Ed25519PublicKey*> keys = {
        { "prod-ed25519-2026-01", &kProdEd25519_2026_01 },
    };
    return keys;
}

} // namespace

const Ed25519PublicKey* publicKeyForKeyId(std::string_view keyId) noexcept
{
    const auto& keys = knownKeys();
    // std::map lookup needs a std::string key.
    const auto it = keys.find(std::string(keyId));
    if (it == keys.end())
        return nullptr;
    return it->second;
}

bool parseEd25519PublicKeyHex(std::string_view hex64, Ed25519PublicKey& out) noexcept
{
    if (hex64.size() != 32 * 2)
        return false;

    auto nibble = [](char c, uint8_t& v) noexcept -> bool
    {
        if (c >= '0' && c <= '9') { v = static_cast<uint8_t>(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { v = static_cast<uint8_t>(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { v = static_cast<uint8_t>(c - 'A' + 10); return true; }
        return false;
    };

    for (std::size_t i = 0; i < 32; ++i)
    {
        uint8_t hi {}, lo {};
        if (! nibble(hex64[i * 2], hi) || ! nibble(hex64[i * 2 + 1], lo))
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace more_phi::licensing
