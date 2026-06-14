#include "MachineFingerprint.h"

#include "LicenseTypes.h"

namespace more_phi::licensing {

juce::String MachineFingerprint::computeMachineHash(juce::String productId)
{
    juce::String raw;
    raw << productId << "|"
        << juce::SystemStats::getOperatingSystemName() << "|"
        << juce::SystemStats::getDeviceDescription() << "|"
        << juce::SystemStats::getCpuVendor() << "|"
        << juce::SystemStats::getCpuModel() << "|"
        << juce::String(juce::SystemStats::getMemorySizeInMegabytes());

    return juce::String::toHexString(raw.hashCode64());
}

} // namespace more_phi::licensing