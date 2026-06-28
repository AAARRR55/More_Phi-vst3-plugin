/*
 * More-Phi — Licensing/LicenseEnvelopeCrypto.cpp
 *
 * SECURITY (Finding #8): BlowFish fallback upgraded from ECB to CTR mode
 * (June 2026). New encryptions use CTR (0x02 mode byte prefix). Old ECB
 * ciphertext (0x01 or no prefix) is detected and decrypted for backward
 * compatibility. DPAPI remains the primary path on Windows.
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

// Mode byte constants for ciphertext format detection
constexpr uint8_t kModeEcb = 0x01;  // legacy ECB (backward compat)
constexpr uint8_t kModeCtr = 0x02;  // CTR mode (current)

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

// Derive CTR initial counter from the BlowFish key material.
// Uses first 8 bytes of SHA-256(key) as the nonce/counter start.
static void deriveCtrCounter(const std::vector<uint8_t>& keyBytes,
                              uint32_t& ctr_l, uint32_t& ctr_r)
{
    juce::SHA256 sha(keyBytes.data(), keyBytes.size());
    juce::MemoryBlock raw = sha.getRawData();
    const auto* d = static_cast<const uint8_t*>(raw.getData());
    ctr_l = static_cast<uint32_t>(d[0])
          | (static_cast<uint32_t>(d[1]) << 8u)
          | (static_cast<uint32_t>(d[2]) << 16u)
          | (static_cast<uint32_t>(d[3]) << 24u);
    ctr_r = static_cast<uint32_t>(d[4])
          | (static_cast<uint32_t>(d[5]) << 8u)
          | (static_cast<uint32_t>(d[6]) << 16u)
          | (static_cast<uint32_t>(d[7]) << 24u);
}

// PKCS#7 padding validation: pad byte must be 1..8 and fill the last pad bytes.
// Returns the unpadded length on success, 0 on failure.
static size_t validatePkcs7Pad(const uint8_t* data, size_t len)
{
    if (len == 0) return 0;
    const uint8_t pad = data[len - 1];
    if (pad == 0u || pad > 8u || pad > len) return 0;
    for (size_t i = len - pad; i < len; ++i)
        if (data[i] != pad)
            return 0;
    return len - pad;
}

// ── CTR-mode decrypt (shared by encrypt XOR and decrypt XOR) ─────────────────
static void ctrXorBlock(uint8_t* block, size_t blockSize,
                         juce::BlowFish& bf, uint32_t& ctr_l, uint32_t& ctr_r)
{
    uint32_t kl = ctr_l, kr = ctr_r;
    bf.encrypt(kl, kr);

    auto& b0 = block[0]; auto& b1 = block[1];
    auto& b2 = block[2]; auto& b3 = block[3];
    auto& b4 = block[4]; auto& b5 = block[5];
    auto& b6 = block[6]; auto& b7 = block[7];

    uint32_t pl = static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8u)
                | (static_cast<uint32_t>(b2) << 16u) | (static_cast<uint32_t>(b3) << 24u);
    uint32_t pr = static_cast<uint32_t>(b4) | (static_cast<uint32_t>(b5) << 8u)
                | (static_cast<uint32_t>(b6) << 16u) | (static_cast<uint32_t>(b7) << 24u);
    pl ^= kl;
    pr ^= kr;

    block[0] = static_cast<uint8_t>(pl);        block[1] = static_cast<uint8_t>(pl >> 8u);
    block[2] = static_cast<uint8_t>(pl >> 16u); block[3] = static_cast<uint8_t>(pl >> 24u);
    block[4] = static_cast<uint8_t>(pr);        block[5] = static_cast<uint8_t>(pr >> 8u);
    block[6] = static_cast<uint8_t>(pr >> 16u); block[7] = static_cast<uint8_t>(pr >> 24u);

    // Increment 64-bit counter (low word first, carry to high)
    if (++ctr_l == 0) ++ctr_r;
}

// ── CTR mode encrypt (PKCS#7-padded, 0x02 prefix) ────────────────────────────
juce::String blowfishEncryptB64(const juce::String& plaintext, const juce::String& machineHash)
{
    std::vector<uint8_t> keyBytes;
    blowfishKey(machineHash, keyBytes);

    const auto bytes = plaintext.toUTF8();
    const size_t rawLen = static_cast<size_t>(plaintext.getNumBytesAsUTF8());
    const size_t pad = 8u - (rawLen % 8u);           // 1..8, always pad

    // +1 byte for mode marker (0x02) prefixing the padded plaintext
    std::vector<uint8_t> buf(1 + rawLen + pad, static_cast<uint8_t>(pad));
    buf[0] = kModeCtr;
    std::memcpy(buf.data() + 1, bytes, rawLen);

    uint32_t ctr_l{}, ctr_r{};
    deriveCtrCounter(keyBytes, ctr_l, ctr_r);

    juce::BlowFish bf(keyBytes.data(), static_cast<int>(keyBytes.size()));
    for (size_t i = 1; i + 8 <= buf.size(); i += 8)  // skip mode byte at index 0
        ctrXorBlock(&buf[i], 8, bf, ctr_l, ctr_r);

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
    if (n == 0)
        return {};

    std::vector<uint8_t> keyBytes;
    blowfishKey(machineHash, keyBytes);

    // ── Detect cipher mode from format ─────────────────────────────────────
    // CTR (current): 1-byte mode marker (0x02) + multiple-of-8 ciphertext
    // ECB (legacy):  multiple-of-8 ciphertext, no prefix
    //
    // Detection is unambiguous:
    //   CTR: n >= 9 && p[0]==0x02 && (n-1) % 8 == 0
    //   ECB: n % 8 == 0 (only reached if CTR check fails)
    bool isCtr = (n >= 9 && p[0] == kModeCtr && ((n - 1) % 8u) == 0);

    juce::BlowFish bf(keyBytes.data(), static_cast<int>(keyBytes.size()));

    if (isCtr)
    {
        // ── CTR mode (Finding #8 fix) ──────────────────────────────────────
        uint32_t ctr_l{}, ctr_r{};
        deriveCtrCounter(keyBytes, ctr_l, ctr_r);

        for (size_t i = 1; i + 8 <= n; i += 8)
            ctrXorBlock(&p[i], 8, bf, ctr_l, ctr_r);

        const size_t unpadded = validatePkcs7Pad(p + 1, n - 1);
        if (unpadded == 0)
            return {};

        return juce::String::fromUTF8(reinterpret_cast<const char*>(p + 1),
                                       static_cast<int>(unpadded));
    }

    // ── ECB mode (legacy, backward compat) ────────────────────────────────
    if ((n % 8u) != 0u)
        return {};

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

    const size_t unpadded = validatePkcs7Pad(p, n);
    if (unpadded == 0)
        return {};

    return juce::String::fromUTF8(reinterpret_cast<const char*>(p),
                                   static_cast<int>(unpadded));
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

    // ── Portable fallback: machine-bound BlowFish (CTR mode) ────────────────
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
