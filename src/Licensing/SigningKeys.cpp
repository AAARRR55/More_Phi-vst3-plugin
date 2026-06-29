/*
 * More-Phi — Licensing/SigningKeys.cpp
 *
 * Phase 1 (production readiness): the production Ed25519 public key is injected
 * at BUILD time via CMake, never committed. Two build modes:
 *
 *  1. PRODUCTION: -DMORE_PHI_PROD_ED25519_KEY_HEX=<64 hex> is passed to CMake.
 *     CMake generates <build>/generated/licensing/ProdSigningKey.h defining
 *     more_phi::licensing::kProdEd25519_2026_01 (the REAL production key). The
 *     __has_include check below picks that header up and the Release build is
 *     unblocked (isDevPlaceholderKey() returns false for the injected key).
 *
 *  2. DEV/TEST (default): no key injected. The dev keypair's public half is used
 *     so local end-to-end activation tests work against the dev backend. A real
 *     Release build (NDEBUG, tests OFF) is then CORRECTLY BLOCKED by the
 *     static_assert below — the ship-blocker is intentional and stays in place.
 *
 * Key rotation procedure: mint a new keypair on the licensing server, then pass
 * the new public half (64 hex chars) via MORE_PHI_PROD_ED25519_KEY_HEX at build
 * time. The dev key literal below is retained ONLY for mode 2; it never ships.
 */
#include "SigningKeys.h"

#include <map>
#include <string>

#if __has_include("ProdSigningKey.h") // generated header — present only when a prod key is injected
#  include "ProdSigningKey.h"
#  define MORE_PHI_HAS_PROD_KEY 1
#else
#  define MORE_PHI_HAS_PROD_KEY 0
#endif

namespace more_phi::licensing {
namespace {

#if ! MORE_PHI_HAS_PROD_KEY
// ── DEV PLACEHOLDER public key (mode 2 only) ────────────────────────────────
// 32 raw bytes, big-endian. Hex (no separators):
//   8e1aa28c35a94bf3c6e555fadb5ba34c5b4880715665c84dfc379e358a52ac9f
// This is the DEV keypair's public half — fine for local end-to-end testing
// against a dev backend. It MUST be replaced by the injected production key
// before any shipping Release build (enforced by the static_assert below).
constexpr Ed25519PublicKey kProdEd25519_2026_01 = {
    0x8e, 0x1a, 0xa2, 0x8c, 0x35, 0xa9, 0x4b, 0xf3,
    0xc6, 0xe5, 0x55, 0xfa, 0xdb, 0x5b, 0xa3, 0x4c,
    0x5b, 0x48, 0x80, 0x71, 0x56, 0x65, 0xc8, 0x4d,
    0xfc, 0x37, 0x9e, 0x35, 0x8a, 0x52, 0xac, 0x9f,
};
#endif

// ── AUDIT-FIX: ship-blocker guard ────────────────────────────────────────────
// The DEV placeholder public key. As long as it is compiled into a binary, any
// certificate signed by the dev backend verifies as a production license. This
// constexpr predicate detects the placeholder so a static_assert can refuse to
// compile a real shipping build until the production key is injected (mode 1).
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
// exempt so end-to-end dev flows still work. Flip this to pass by injecting the
// production key via MORE_PHI_PROD_ED25519_KEY_HEX at build time.
#if defined(NDEBUG) && !defined(MORE_PHI_TEST_MODE)
static_assert(! isDevPlaceholderKey(kProdEd25519_2026_01),
    "RELEASE BUILD BLOCKED: SigningKeys.cpp still contains the DEV Ed25519 "
    "public key. Inject the production key at build time: "
    "cmake -DMORE_PHI_PROD_ED25519_KEY_HEX=<64 hex chars> "
    "(keyId prod-ed25519-2026-01) before building a shipping Release.");
#endif

// Compile-time cross-check that, when a production key IS injected, it differs
// from the dev placeholder. Catches a misconfigured CI that injects the dev hex.
#if MORE_PHI_HAS_PROD_KEY
static_assert(! isDevPlaceholderKey(kProdEd25519_2026_01),
    "RELEASE BUILD BLOCKED: the injected MORE_PHI_PROD_ED25519_KEY_HEX equals "
    "the DEV placeholder key. Inject the REAL production key, not the dev one.");
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
