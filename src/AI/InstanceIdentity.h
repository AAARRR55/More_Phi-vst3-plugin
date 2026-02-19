/*
 * MorphSnap - AI/InstanceIdentity.h
 * Per-instance identity for multi-instance MCP architecture.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <string>
#include <array>
#include <stdexcept>

#if JUCE_WINDOWS
    #define NOMINMAX
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

namespace morphsnap {

/**
 * Generates cryptographically secure random bytes.
 * Uses platform-specific CSPRNG:
 *   - Windows: BCryptGenRandom
 *   - macOS/iOS: SecRandomCopyBytes
 *   - Linux: getrandom() or /dev/urandom
 *
 * @param buffer Pointer to buffer to fill with random bytes
 * @param size Number of bytes to generate
 * @return true on success, false on failure
 */
inline bool generateSecureRandomBytes(void* buffer, size_t size)
{
    if (buffer == nullptr || size == 0)
        return false;

#if JUCE_WINDOWS
    // Use BCryptGenRandom (available on Windows Vista+)
    NTSTATUS status = BCryptGenRandom(
        nullptr,                    // Use default algorithm provider (RNG)
        static_cast<PUCHAR>(buffer),
        static_cast<ULONG>(size),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    return status == 0;  // STATUS_SUCCESS

#elif JUCE_MAC || JUCE_IOS
    // Use Security framework's SecRandomCopyBytes
    OSStatus status = SecRandomCopyBytes(kSecRandomDefault, size, buffer);
    return status == errSecSuccess;

#elif JUCE_LINUX
    // Try getrandom() first (Linux 3.17+)
    #ifdef SYS_getrandom
    ssize_t result = getrandom(buffer, size, 0);
    if (result == static_cast<ssize_t>(size))
        return true;
    #endif

    // Fallback to /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;

    ssize_t bytesRead = read(fd, buffer, size);
    close(fd);
    return bytesRead == static_cast<ssize_t>(size);

#else
    // Fallback for other platforms - NOT cryptographically secure
    // This should not be reached on supported platforms
    juce::Random rng(juce::Time::getHighResolutionTicks());
    auto* bytes = static_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(rng.nextInt(256));
    return true;
#endif
}

/**
 * Generates a random hex string using cryptographically secure RNG.
 *
 * @param numBytes Number of random bytes to generate (output will be 2*numBytes hex chars)
 * @return Hex string representation of the random bytes
 */
inline juce::String generateSecureRandomHexString(size_t numBytes)
{
    juce::HeapBlock<uint8_t> buffer(numBytes);
    if (!generateSecureRandomBytes(buffer.getData(), numBytes))
    {
        // On failure, throw an exception - we should not silently fall back to weak RNG
        throw std::runtime_error("Failed to generate cryptographically secure random bytes");
    }
    return juce::String::toHexString(buffer.getData(), static_cast<int>(numBytes));
}

/**
 * Generates a short code using cryptographically secure RNG.
 *
 * @param length Desired length of the code (default 8)
 * @return Short alphanumeric code
 */
inline juce::String generateSecureShortCode(int length = 8)
{
    static const char* hexChars = "0123456789abcdef";
    const int numBytes = (length + 1) / 2;  // Each byte = 2 hex chars

    juce::HeapBlock<uint8_t> buffer(numBytes);
    if (!generateSecureRandomBytes(buffer.getData(), numBytes))
    {
        throw std::runtime_error("Failed to generate cryptographically secure random bytes");
    }

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

        // Generate instanceId using cryptographically secure RNG (128-bit / 16 bytes)
        id.instanceId = generateSecureRandomHexString(16);

        // Generate bearerToken using cryptographically secure RNG (128-bit / 16 bytes)
        id.bearerToken = generateSecureRandomHexString(16);

        // Generate morphCode using cryptographically secure RNG (8 hex chars)
        id.morphCode = generateSecureShortCode(8);

        return id;
    }

    juce::String toJSON() const
    {
        return "{\"instanceId\":\"" + instanceId + "\""
             + ",\"morphCode\":\"" + morphCode + "\""
             + ",\"port\":" + juce::String(port)
             + ",\"bearerToken\":\"" + bearerToken + "\""
             + ",\"createdAt\":" + juce::String(createdAt) + "}";
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
     * Overwrites the bearer token buffer with volatile writes to hinder
     * compiler elimination, then resets all identity fields.
     *
     * Note: juce::String uses a reference-counted internal buffer, so this
     * is a best-effort measure — full security would require heap-allocated
     * char arrays with guaranteed zeroing semantics.
     */
    void zeroize()
    {
        // Overwrite bearer token bytes before the String refcount is released
        const int tokenLen = bearerToken.length();
        if (tokenLen > 0)
        {
            // getCharPointer() gives a raw pointer to the internal buffer.
            // Casting via volatile prevents the compiler from eliding the writes.
            auto* rawPtr = bearerToken.getCharPointer().getAddress();
            volatile char* vp = reinterpret_cast<volatile char*>(rawPtr);
            for (int i = 0; i < tokenLen; ++i)
                vp[i] = '\0';
        }
        bearerToken = {};
        instanceId  = {};
        morphCode   = {};
        port        = 0;
        createdAt   = 0;
    }
};

} // namespace morphsnap