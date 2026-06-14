/*
 * More-Phi — Licensing/ActivationClient.h
 * Mockable activation client interface. Network implementation can be added later.
 */
#pragma once

#include "LicenseTypes.h"

namespace more_phi::licensing {

struct ActivationRequest
{
    juce::String licenseKey;
    juce::String productId = PRODUCT_ID;
    juce::String pluginVersion;
    juce::String machineHash;
    juce::String os;
    juce::String dawHint;
    juce::String requestNonce;
};

struct ActivationResponse
{
    bool success = false;
    juce::String status;
    juce::String errorCode;
    juce::String message;
    std::optional<SignedCertificate> certificate;
};

class IActivationClient
{
public:
    virtual ~IActivationClient() = default;
    virtual ActivationResponse activate(const ActivationRequest& request) = 0;
    virtual ActivationResponse refresh(const juce::String& activationId,
                                       const juce::String& machineHash) = 0;
    virtual ActivationResponse deactivate(const juce::String& activationId,
                                          const juce::String& machineHash) = 0;
};

class StubActivationClient final : public IActivationClient
{
public:
    ActivationResponse activate(const ActivationRequest& request) override;
    ActivationResponse refresh(const juce::String& activationId,
                               const juce::String& machineHash) override;
    ActivationResponse deactivate(const juce::String& activationId,
                                  const juce::String& machineHash) override;
};

} // namespace more_phi::licensing