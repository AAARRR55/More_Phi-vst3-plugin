/*
 * More-Phi — Licensing/ActivationClient.h
 * Mockable activation client interface and configurable HTTP implementation.
 */
#pragma once

#include <memory>

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

struct LicenseApiConfig
{
    juce::String baseUrl;
    juce::String publicClientToken;
    juce::String clientHeaderName = "X-MorePhi-Client";
    int timeoutMs = 8000;

    bool isUsable() const noexcept
    {
        return baseUrl.trim().isNotEmpty() && publicClientToken.trim().isNotEmpty();
    }
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

// Parses a raw activation-server response body into an ActivationResponse.
// Exposed (rather than file-static inside ActivationClient.cpp) so the
// error-envelope mapping and certificate extraction are unit-testable without
// going to the network.
ActivationResponse parseActivationResponse(int statusCode,
                                           const juce::String& body,
                                           bool requireCertificate);

class HttpActivationClient final : public IActivationClient
{
public:
    explicit HttpActivationClient(LicenseApiConfig config);

    ActivationResponse activate(const ActivationRequest& request) override;
    ActivationResponse refresh(const juce::String& activationId,
                               const juce::String& machineHash) override;
    ActivationResponse deactivate(const juce::String& activationId,
                                  const juce::String& machineHash) override;

    static std::unique_ptr<IActivationClient> createFromEnvironment();
    static LicenseApiConfig configFromEnvironment();

private:
    ActivationResponse post_(const juce::String& path, const juce::String& body, bool requireCertificate) const;
    juce::String endpoint_(const juce::String& path) const;

    LicenseApiConfig config_;
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