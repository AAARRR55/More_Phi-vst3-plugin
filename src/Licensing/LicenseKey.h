/*
 * More-Phi — Licensing/LicenseKey.h
 * Human-readable license key normalization and checksum validation.
 */
#pragma once

#include <cstdint>

#include <juce_core/juce_core.h>

namespace more_phi::licensing {

struct ParsedLicenseKey
{
    bool valid = false;
    juce::String normalized;
    juce::String error;
};

class LicenseKey
{
public:
    static juce::String normalize(juce::String input);
    static uint8_t checksumIndex(juce::StringRef normalizedWithoutChecksum);
    static juce::juce_wchar checksumChar(uint8_t checksumIndex) noexcept;
    static ParsedLicenseKey parse(juce::String input);
    static bool isValid(juce::String input) { return parse(input).valid; }

private:
    static int alphabetIndex(juce::juce_wchar c) noexcept;
};

} // namespace more_phi::licensing