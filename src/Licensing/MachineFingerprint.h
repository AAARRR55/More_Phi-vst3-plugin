/*
 * More-Phi — Licensing/MachineFingerprint.h
 * Privacy-preserving local machine hash for node-locked certificates.
 */
#pragma once

#include <juce_core/juce_core.h>

#include "LicenseTypes.h"

namespace more_phi::licensing {

class MachineFingerprint
{
public:
    static juce::String computeMachineHash(juce::String productId = PRODUCT_ID);
};

} // namespace more_phi::licensing