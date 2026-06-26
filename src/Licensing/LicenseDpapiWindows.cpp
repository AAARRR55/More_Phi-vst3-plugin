/*
 * More-Phi — Licensing/LicenseDpapiWindows.cpp
 *
 * Windows DPAPI (CryptProtectData / CryptUnprotectData) wrapper for the license
 * cert envelope. Isolated in its own TU so <windows.h> macros never collide with
 * JUCE headers. Compiled on Windows only (gated here + in CMakeLists).
 *
 * DPAPI binds the ciphertext to the current Windows user on the current machine
 * by default — exactly the scope a node-locked license wants. No key material
 * lives in our binary; Windows manages it.
 */
#if defined(_WIN32) && !defined(MORE_PHI_DISABLE_DPAPI)

#include <juce_core/juce_core.h>

// NOMINMAX + WIN32_LEAN_AND_MEAN are already project-wide; reaffirm locally
// since wincrypt headers are sensitive to min/max macros.
#ifndef NOMINMAX
#  define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif

#include <windows.h>
#include <wincrypt.h>

// These wrappers live at GLOBAL scope with C linkage so the cross-platform
// LicenseEnvelopeCrypto.cpp can declare them `extern "C"` and resolve them
// regardless of namespace. (The cert is signed + machine-hash-bound at the
// protocol layer; DPAPI only adds at-rest secrecy + user binding.)

extern "C" {

// Returns true on success. On failure, leaves outBase64 empty so the caller
// falls back to the portable BlowFish path.
bool dpapiProtect(const juce::String& plaintext, juce::String& outBase64)
{
    const auto utf8 = plaintext.toUTF8();
    const DWORD dataLen = static_cast<DWORD>(plaintext.getNumBytesAsUTF8());

    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(static_cast<const char*>(utf8)));
    in.cbData = dataLen;

    DATA_BLOB out{};
    // No entropy — we rely on DPAPI's user/machine binding alone. An entropy
    // blob would add a second secret to manage; the cert is already signed +
    // machine-hash-bound, so DPAPI's default scope is sufficient.
    if (! CryptProtectData(&in, L"MorePhi-License", nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    juce::MemoryBlock mb(out.pbData, static_cast<size_t>(out.cbData));
    outBase64 = mb.toBase64Encoding();
    LocalFree(out.pbData);
    return true;
}

// Returns true on success. Fails (returns false, outPlaintext untouched) if the
// blob was protected under a different user/machine — i.e. the cert was copied
// onto the wrong machine, which is exactly the case we want to refuse.
bool dpapiUnprotect(const juce::String& base64, juce::String& outPlaintext)
{
    juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding(base64) || mb.getSize() == 0)
        return false;

    DATA_BLOB in{};
    in.pbData = static_cast<BYTE*>(mb.getData());
    in.cbData = static_cast<DWORD>(mb.getSize());

    DATA_BLOB out{};
    LPWSTR description = nullptr;
    if (! CryptUnprotectData(&in, &description, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    if (description != nullptr)
        LocalFree(description);

    outPlaintext = juce::String::fromUTF8(reinterpret_cast<const char*>(out.pbData),
                                          static_cast<int>(out.cbData));
    // Secure-zero the plaintext buffer before freeing.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

} // extern "C"

#endif // _WIN32
