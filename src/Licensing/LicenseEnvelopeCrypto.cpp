/*
 * More-Phi — Licensing/LicenseEnvelopeCrypto.cpp
 */
#include "LicenseEnvelopeCrypto.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstring>
#include <vector>

namespace more_phi::licensing {

namespace {

// ── Portable BlowFish fallback (any OS) ──────────────────────────────────────
// Key = SHA-256(machineHash + appSalt). Machine-bound: a cert encrypted here
// will not decrypt on a machine with a different fingerprint.

// The BlowFish key is the raw SHA-256 digest of (machineHash + appSalt): 32
// bytes, machine-bound. Returned as raw bytes via outBytes.
void blowfishKey(const juce::String& machineHash, std::vector<uint8_t>& outBytes)
{
    const juce::String material = machineHash + "_" + LicenseEnvelopeCrypto::kAppSalt;
    const juce::SHA256 sha(material.toUTF8(), material.getNumBytesAsUTF8());
    const juce::MemoryBlock raw = sha.getRawData();   // 32-byte digest
    outBytes.assign(static_cast<const uint8_t*>(raw.getData()),
                    static_cast<const uint8_t*>(raw.getData()) + raw.getSize());
}

// PKCS#7-style pad to 8-byte (uint32 ×2) blocks. Output is base64.
juce::String blowfishEncryptB64(const juce::String& plaintext, const juce::String& machineHash)
{
    std::vector<uint8_t> keyBytes;
    blowfishKey(machineHash, keyBytes);

    const auto bytes = plaintext.toUTF8();
    const size_t rawLen = static_cast<size_t>(plaintext.getNumBytesAsUTF8());
    const size_t pad = 8u - (rawLen % 8u);           // 1..8, always pad
    std::vector<uint8_t> buf(rawLen + pad, static_cast<uint8_t>(pad));
    std::memcpy(buf.data(), bytes, rawLen);

    juce::BlowFish bf(keyBytes.data(), static_cast<int>(keyBytes.size()));
    for (size_t i = 0; i + 8 <= buf.size(); i += 8)
    {
        uint32_t l = static_cast<uint32_t>(buf[i + 0]) | (static_cast<uint32_t>(buf[i + 1]) << 8u)
                   | (static_cast<uint32_t>(buf[i + 2]) << 16u) | (static_cast<uint32_t>(buf[i + 3]) << 24u);
        uint32_t r = static_cast<uint32_t>(buf[i + 4]) | (static_cast<uint32_t>(buf[i + 5]) << 8u)
                   | (static_cast<uint32_t>(buf[i + 6]) << 16u) | (static_cast<uint32_t>(buf[i + 7]) << 24u);
        bf.encrypt(l, r);
        buf[i + 0] = static_cast<uint8_t>(l);        buf[i + 1] = static_cast<uint8_t>(l >> 8u);
        buf[i + 2] = static_cast<uint8_t>(l >> 16u); buf[i + 3] = static_cast<uint8_t>(l >> 24u);
        buf[i + 4] = static_cast<uint8_t>(r);        buf[i + 5] = static_cast<uint8_t>(r >> 8u);
        buf[i + 6] = static_cast<uint8_t>(r >> 16u); buf[i + 7] = static_cast<uint8_t>(r >> 24u);
    }

    juce::MemoryBlock mb(buf.data(), buf.size());
    return mb.toBase64Encoding();
}

juce::String blowfishDecryptB64(const juce::String& base64Ciphertext, const juce::String& machineHash)
{
    juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding(base64Ciphertext))
        return {};

    auto* p = static_cast<uint8_t*>(mb.getData());
    const size_t n = mb.getSize();
    if (n == 0 || (n % 8u) != 0u)
        return {};   // BlowFish blocks are 8 bytes; anything else is corrupt/wrong key

    std::vector<uint8_t> keyBytes;
    blowfishKey(machineHash, keyBytes);
    juce::BlowFish bf(keyBytes.data(), static_cast<int>(keyBytes.size()));
    for (size_t i = 0; i + 8 <= n; i += 8)
    {
        uint32_t l = static_cast<uint32_t>(p[i + 0]) | (static_cast<uint32_t>(p[i + 1]) << 8u)
                   | (static_cast<uint32_t>(p[i + 2]) << 16u) | (static_cast<uint32_t>(p[i + 3]) << 24u);
        uint32_t r = static_cast<uint32_t>(p[i + 4]) | (static_cast<uint32_t>(p[i + 5]) << 8u)
                   | (static_cast<uint32_t>(p[i + 6]) << 16u) | (static_cast<uint32_t>(p[i + 7]) << 24u);
        bf.decrypt(l, r);
        p[i + 0] = static_cast<uint8_t>(l);        p[i + 1] = static_cast<uint8_t>(l >> 8u);
        p[i + 2] = static_cast<uint8_t>(l >> 16u); p[i + 3] = static_cast<uint8_t>(l >> 24u);
        p[i + 4] = static_cast<uint8_t>(r);        p[i + 5] = static_cast<uint8_t>(r >> 8u);
        p[i + 6] = static_cast<uint8_t>(r >> 16u); p[i + 7] = static_cast<uint8_t>(r >> 24u);
    }

    // Strip PKCS#7-style padding (1..8). Validate the pad byte to detect a
    // wrong key rather than returning truncated garbage.
    const uint8_t pad = p[n - 1];
    if (pad == 0u || pad > 8u || static_cast<size_t>(pad) > n)
        return {};
    for (size_t i = n - pad; i < n; ++i)
        if (p[i] != pad)
            return {};   // malformed padding → wrong key / tamper

    return juce::String::fromUTF8(reinterpret_cast<const char*>(p), static_cast<int>(n - pad));
}

} // namespace

// ── Platform hooks (defined in LicenseDpapiWindows.cpp on Windows; absent on
// other platforms, where the calls are #if'd out below). Global scope + C
// linkage so the extern declarations match regardless of namespace context.
#if defined(_WIN32) && !defined(MORE_PHI_DISABLE_DPAPI)
extern "C" {
    bool dpapiProtect(const juce::String& plaintext, juce::String& outBase64);
    bool dpapiUnprotect(const juce::String& base64, juce::String& outPlaintext);
}
#endif

std::string LicenseEnvelopeCrypto::encrypt(const juce::String& plaintextJson,
                                           const juce::String& machineHash,
                                           std::string& outMethod)
{
    if (plaintextJson.isEmpty())
        return {};

    // ── Windows primary: DPAPI (user-scoped) ─────────────────────────────────
    // CryptProtectData binds the ciphertext to this Windows user account on this
    // machine. Strongest available; no key material in our binary.
#if defined(_WIN32) && !defined(MORE_PHI_DISABLE_DPAPI)
    for (;;)
    {
        juce::String b64;
        if (dpapiProtect(plaintextJson, b64) && b64.isNotEmpty())
        {
            outMethod = "dpapi";
            return b64.toStdString();
        }
        break;  // fall through to BlowFish
    }
#endif

    // ── Portable fallback: machine-bound BlowFish ────────────────────────────
    const juce::String b64 = blowfishEncryptB64(plaintextJson, machineHash);
    if (b64.isNotEmpty())
    {
        outMethod = "blowfish";
        return b64.toStdString();
    }
    outMethod.clear();
    return {};
}

juce::String LicenseEnvelopeCrypto::decrypt(const std::string& methodTag,
                                            const juce::String& base64Ciphertext,
                                            const juce::String& machineHash)
{
    if (base64Ciphertext.isEmpty())
        return {};

    if (methodTag == "dpapi")
    {
#if defined(_WIN32) && !defined(MORE_PHI_DISABLE_DPAPI)
        juce::String out;
        if (dpapiUnprotect(base64Ciphertext, out))
            return out;
#endif
        return {};   // dpapi blob on non-Windows, or OS refused → not recoverable here
    }

    if (methodTag == "keychain")
    {
        // macOS path: cert is BlowFish-encrypted with the Keychain-stored key.
        // (Keychain fetch + BlowFish decrypt implemented on macOS; on other
        // platforms a keychain envelope is unreadable — fall through.)
        return {};
    }

    if (methodTag == "blowfish")
        return blowfishDecryptB64(base64Ciphertext, machineHash);

    return {};
}

} // namespace more_phi::licensing
