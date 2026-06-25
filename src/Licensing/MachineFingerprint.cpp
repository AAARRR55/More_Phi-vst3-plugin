#include "MachineFingerprint.h"

#include "LicenseTypes.h"
// juce::SHA256 lives in the juce_cryptography module.
#include <juce_cryptography/juce_cryptography.h>
#include <juce_cryptography/juce_cryptography.h>   // AUDIT-FIX: juce::SHA256 lives in juce_cryptography, not juce_core

namespace more_phi::licensing {
namespace {

// AUDIT-FIX (security): returns the first hardware MAC address as a stable,
// hardware-bound identifier, or an empty string if none is available (e.g.
// headless CI). Enriching the fingerprint with a MAC raises the bar over the
// previous CPU/RAM-only inputs, which were trivially cloned from registry /
// system calls.
juce::String firstPhysicalMacAddress()
{
    juce::Array<juce::MACAddress> addresses;
    juce::MACAddress::findAllAddresses(addresses);
    for (const auto& addr : addresses)
    {
        if (! addr.isNull())
            return addr.toString();
    }
    return {};
}

} // namespace

// AUDIT-FIX (security): replaced the non-cryptographic juce::String::hashCode64()
// with a SHA-256 digest. hashCode64 is a 64-bit non-cryptographic hash of a
// short, predictable input string — brute-forceable to a preimage, letting an
// attacker forge a machineHash by cloning six trivially-readable fields.
// SHA-256 over an expanded input set (now including a hardware MAC address)
// makes preimage search computationally infeasible.
//
// NOTE: this changes every machineHash value, so existing dev activations must
// be re-activated. Acceptable pre-release (the signing key is still the dev
// key — see SigningKeys.cpp).
juce::String MachineFingerprint::computeMachineHash(juce::String productId)
{
    juce::String raw;
    raw << productId << "|"
        << juce::SystemStats::getOperatingSystemName() << "|"
        << juce::SystemStats::getDeviceDescription() << "|"
        << juce::SystemStats::getCpuVendor() << "|"
        << juce::SystemStats::getCpuModel() << "|"
        << juce::String(juce::SystemStats::getMemorySizeInMegabytes()) << "|"
        << firstPhysicalMacAddress();

    juce::SHA256 sha(raw.toUTF8());   // one-shot hash over the UTF-8 bytes
    return sha.toHexString();         // 64-digit lowercase hex digest
}

} // namespace more_phi::licensing
