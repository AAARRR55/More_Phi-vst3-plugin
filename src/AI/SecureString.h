/*
 * More-Phi — AI/SecureString.h
 *
 * RAII string container that zeroes its contents on destruction.
 * Uses platform-specific secure memory wipe (SecureZeroMemory on Windows,
 * volatile-sink memset on other platforms) to prevent API key material from
 * persisting in heap memory after use.
 *
 * ponytail: wraps a single juce::String internally for compatibility with the
 * rest of the codebase. If key-copy tracing shows it's still insufficient,
 * replace with a HeapBlock-based implementation that avoids CoW sharing.
 */
#pragma once

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace more_phi {

/**
 * A string wrapper that:
 *  - Implicitly converts to/from juce::String for API compatibility
 *  - Calls SecureZeroMemory on the internal buffer when cleared or destroyed
 *
 * NOTE: Due to juce::String's copy-on-write semantics, copies of this string
 * may share the same backing buffer. Always call .clear() / zeroize() at known
 * exit points rather than relying solely on the destructor.
 */
class SecureString
{
public:
    SecureString() = default;

    SecureString(const juce::String& s) : data_(s) {}

    SecureString(const SecureString& other) : data_(other.data_) {}

    SecureString(SecureString&& other) noexcept
        : data_(std::move(other.data_))
    {
        other.data_ = {};
    }

    SecureString& operator=(const SecureString& other)
    {
        if (this != &other)
        {
            zeroize();
            data_ = other.data_;
        }
        return *this;
    }

    SecureString& operator=(SecureString&& other) noexcept
    {
        if (this != &other)
        {
            zeroize();
            data_ = std::move(other.data_);
            other.data_ = {};
        }
        return *this;
    }

    ~SecureString() { zeroize(); }

    /** Convert to juce::String (read-only access for API compatibility). */
    const juce::String& toString() const noexcept { return data_; }
    operator const juce::String&() const noexcept { return data_; }

    /** Implicit conversion from juce::String for assignment. */
    SecureString& operator=(const juce::String& s)
    {
        zeroize();
        data_ = s;
        return *this;
    }

    /** Clear and zeroize the buffer. */
    void clear()
    {
        zeroize();
        data_ = {};
    }

    /** Check if empty. */
    bool isEmpty() const noexcept { return data_.isEmpty(); }

    /** Length of string. */
    int length() const noexcept { return data_.length(); }

    /** Character pointer for use with HTTP headers etc. */
    juce::String::CharPointerType getCharPointer() const noexcept
    {
        return data_.getCharPointer();
    }

private:
    void zeroize()
    {
        const int len = data_.length();
        if (len > 0)
        {
            auto* rawPtr = const_cast<juce::CharPointerType::CharType*>(
                data_.getCharPointer().getAddress());
#if JUCE_WINDOWS
            SecureZeroMemory(rawPtr, static_cast<size_t>(len * sizeof(juce::CharPointerType::CharType)));
#else
            std::memset(rawPtr, 0, static_cast<size_t>(len * sizeof(juce::CharPointerType::CharType)));
            // Volatile sink prevents compiler from eliding the wipe as a dead store.
            volatile const void* sink = rawPtr;
            (void)sink;
#endif
        }
    }

    juce::String data_;
};

/** Free function for zeroizing a plain juce::String at known exit points. */
inline void zeroizeString(juce::String& s)
{
    const int len = s.length();
    if (len > 0)
    {
        auto* rawPtr = const_cast<juce::CharPointerType::CharType*>(
            s.getCharPointer().getAddress());
#if JUCE_WINDOWS
        SecureZeroMemory(rawPtr, static_cast<size_t>(len * sizeof(juce::CharPointerType::CharType)));
#else
        std::memset(rawPtr, 0, static_cast<size_t>(len * sizeof(juce::CharPointerType::CharType)));
        volatile const void* sink = rawPtr;
        (void)sink;
#endif
    }
    s = {};
}

} // namespace more_phi
