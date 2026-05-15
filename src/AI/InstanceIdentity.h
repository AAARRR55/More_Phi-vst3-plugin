/*
 * More-Phi - AI/InstanceIdentity.h
 * Per-instance identity for multi-instance MCP architecture.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <string>
#include <array>
#include <stdexcept>

#if JUCE_WINDOWS
    // Must include windows.h BEFORE bcrypt.h - bcrypt.h requires NTSTATUS, ULONG, etc.
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#elif JUCE_MAC || JUCE_IOS
    #include <Security/SecRandom.h>
#elif JUCE_LINUX
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/random.h>
#endif

namespace more_phi {

/**
 * Generates cryptographically secure random bytes.
 * Uses platform-specific CSPRNG:
 *   - Windows: BCryptGenRandom
 *   - macOS/iOS: SecRandomCopyBytes
 *   - Linux: getrandom() or /dev/urandom
 */
inline bool generateSecureRandomBytes(void* buffer, size_t size)
{
    if (buffer == nullptr || size == 0)
        return false;

#if JUCE_WINDOWS
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        static_cast<PUCHAR>(buffer),
        static_cast<ULONG>(size),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    return status == 0;

#elif JUCE_MAC || JUCE_IOS
    OSStatus status = SecRandomCopyBytes(kSecRandomDefault, size, buffer);
    return status == errSecSuccess;

#elif JUCE_LINUX
    #ifdef SYS_getrandom
    ssize_t result = getrandom(buffer, size, 0);
    if (result == static_cast<ssize_t>(size))
        return true;
    #endif

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;

    ssize_t bytesRead = read(fd, buffer, size);
    close(fd);
    return bytesRead == static_cast<ssize_t>(size);

#else
    juce::Random rng(juce::Time::getHighResolutionTicks());
    auto* bytes = static_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(rng.nextInt(256));
    return true;
#endif
}

inline juce::String generateSecureRandomHexString(size_t numBytes)
{
    juce::HeapBlock<uint8_t> buffer(numBytes);
    if (!generateSecureRandomBytes(buffer.getData(), numBytes))
        throw std::runtime_error("Failed to generate secure random bytes");
    return juce::String::toHexString(buffer.getData(), static_cast<int>(numBytes));
}

inline juce::String generateSecureShortCode(int length = 8)
{
    static const char* hexChars = "0123456789abcdef";
    const int numBytes = (length + 1) / 2;
    juce::HeapBlock<uint8_t> buffer(numBytes);
    if (!generateSecureRandomBytes(buffer.getData(), numBytes))
        throw std::runtime_error("Failed to generate secure random bytes");

    juce::String result;
    for (int i = 0; i < numBytes && result.length() < length; ++i)
    {
        uint8_t byte = buffer[i];
        result += hexChars[(byte >> 4) & 0x0F];
        if (result.length() < length)
            result += hexChars[byte & 0x0F];
    }
    return result;
}

struct InstanceIdentity
{
    juce::String instanceId;
    juce::String morphCode;
    int          port = 0;
    juce::String bearerToken;
    juce::int64  createdAt = 0;

    static InstanceIdentity generate(int assignedPort)
    {
        InstanceIdentity id;
        id.port = assignedPort;
        id.createdAt = juce::Time::currentTimeMillis();
        id.instanceId = generateSecureRandomHexString(16);
        id.bearerToken = generateSecureRandomHexString(16);
        id.morphCode = generateSecureShortCode(8);
        return id;
    }

    juce::String toJSON() const
    {
        return toRedactedJSON();
    }

    juce::String toRedactedJSON() const
    {
        return "{\"instanceId\":\"" + instanceId + "\""
             + ",\"morphCode\":\"" + morphCode + "\""
             + ",\"port\":" + juce::String(port)
             + ",\"createdAt\":" + juce::String(createdAt) + "}";
    }

    /**
     * Best-effort token zeroization before clearing.
     */
    void zeroize()
    {
        const int tokenLen = bearerToken.length();
        if (tokenLen > 0)
        {
            auto* rawPtr = bearerToken.getCharPointer().getAddress();
#if JUCE_WINDOWS
            SecureZeroMemory(rawPtr, static_cast<size_t>(tokenLen));
#else
            explicit_bzero(rawPtr, static_cast<size_t>(tokenLen));
#endif
        }
        bearerToken = {};
        instanceId  = {};
        morphCode   = {};
        port        = 0;
        createdAt   = 0;
    }
};

} // namespace more_phi
