#include "LicenseManager.h"

#include "Ed25519Verifier.h"
#include "Version.h"

#include <utility>

namespace more_phi::licensing {

LicenseManager::LicenseManager(LicenseRuntimeState& runtimeState)
    : LicenseManager(runtimeState, SecureLicenseStore(), HttpActivationClient::createFromEnvironment())
{
}

LicenseManager::LicenseManager(LicenseRuntimeState& runtimeState,
                               SecureLicenseStore store,
                               std::unique_ptr<IActivationClient> activationClient)
    : runtimeState_(runtimeState),
      store_(std::move(store)),
      activationClient_(std::move(activationClient)),
      machineHash_(MachineFingerprint::computeMachineHash())
{
    if (!activationClient_)
        activationClient_ = std::make_unique<StubActivationClient>();

    // Real Ed25519 verification against the compiled-in public keys. This is
    // the only signature path that accepts production certificates.
    verifier_.setSignatureVerifier(makeEd25519SignatureVerifier());

    // Debug-only development bypass: the local licensing server emits a fixed
    // "dev-signature" with keyId "dev-key" so a checkout of the backend can be
    // exercised without provisioning real key material. Compiled out of release
    // builds (NDEBUG) so a forged dev certificate can never unlock a shipped
    // plugin. The string form ("dev-signature") is what the decoded signature
    // bytes are compared against by the default verifier in LicenseVerifier.cpp.
#if ! defined(NDEBUG)
    verifier_.addTrustedDevelopmentSignature("dev-key", "dev-signature");
#endif
}

int64_t LicenseManager::nowUnixSeconds()
{
    return juce::Time::getCurrentTime().toMilliseconds() / 1000;
}

juce::String LicenseManager::createRequestNonce()
{
    juce::Uuid id;
    return id.toString();
}

void LicenseManager::publish_(const ValidationResult& result)
{
    runtimeState_.featureMask.store(result.featureMask, std::memory_order_release);
    runtimeState_.graceEndsUnixSeconds.store(result.graceEndsUnix, std::memory_order_release);
    runtimeState_.nextCheckUnixSeconds.store(result.nextOnlineCheckAtUnix, std::memory_order_release);
    runtimeState_.premiumFeaturesEnabled.store(result.enablesPremiumFeatures, std::memory_order_release);
    runtimeState_.state.store(result.state, std::memory_order_release);
    lastMessage_ = result.message;

    // Cache the activationId from the verifier's most-recent validated payload
    // so refresh/deactivate can address the right server-side activation without
    // re-parsing the on-disk certificate.
    if (result.enablesPremiumFeatures)
    {
        const auto& payload = verifier_.lastValidatedPayload();
        lastActivationId_ = payload ? juce::String(payload->activationId) : juce::String();
    }
}

ValidationResult LicenseManager::loadCachedCertificate()
{
    juce::String error;
    const auto cert = store_.loadCertificate(&error);
    if (!cert)
    {
        lastActivationId_.clear();
        ValidationResult result { LicenseState::ActivationRequired, false, 0u, 0, 0, error };
        publish_(result);
        return result;
    }

    auto result = verifier_.validateCertificate(*cert, machineHash_, nowUnixSeconds());
    publish_(result);
    return result;
}

ValidationResult LicenseManager::activateWithKey(const juce::String& licenseKey,
                                                 const juce::String& pluginVersion,
                                                 const juce::String& dawHint)
{
    const auto parsed = LicenseKey::parse(licenseKey);
    if (!parsed.valid)
    {
        ValidationResult result { LicenseState::Invalid, false, 0u, 0, 0, parsed.error };
        publish_(result);
        return result;
    }

    ActivationRequest request;
    request.licenseKey = parsed.normalized;
    request.pluginVersion = pluginVersion.isNotEmpty() ? pluginVersion : juce::String(VERSION_STRING);
    request.machineHash = machineHash_;
    request.os = juce::SystemStats::getOperatingSystemName();
    request.dawHint = dawHint;
    request.requestNonce = createRequestNonce();

    auto response = activationClient_->activate(request);
    if (!response.success || !response.certificate)
    {
        ValidationResult result {
            LicenseState::ActivationRequired,
            false,
            0u,
            0,
            0,
            response.message.isNotEmpty() ? response.message : "Activation failed."
        };
        publish_(result);
        return result;
    }

    auto result = verifier_.validateCertificate(*response.certificate, machineHash_, nowUnixSeconds());
    (void) storeVerifiedCertificate(*response.certificate, result);

    publish_(result);
    return result;
}

ValidationResult LicenseManager::refreshActivation(const juce::String& activationId)
{
    if (activationId.trim().isEmpty())
    {
        ValidationResult result { LicenseState::ActivationRequired, false, 0u, 0, 0, "Missing activation id for license refresh." };
        publish_(result);
        return result;
    }

    auto response = activationClient_->refresh(activationId, machineHash_);
    if (!response.success || !response.certificate)
    {
        ValidationResult result {
            LicenseState::ActivationRequired,
            false,
            0u,
            0,
            0,
            response.message.isNotEmpty() ? response.message : "License refresh failed."
        };
        publish_(result);
        return result;
    }

    auto result = verifier_.validateCertificate(*response.certificate, machineHash_, nowUnixSeconds());
    (void) storeVerifiedCertificate(*response.certificate, result);
    publish_(result);
    return result;
}

bool LicenseManager::deactivateActivation(const juce::String& activationId, juce::String* error)
{
    if (activationId.trim().isEmpty())
    {
        if (error != nullptr)
            *error = "Missing activation id for deactivation.";
        return false;
    }

    const auto response = activationClient_->deactivate(activationId, machineHash_);
    if (!response.success)
    {
        if (error != nullptr)
            *error = response.message.isNotEmpty() ? response.message : "License deactivation failed.";
        return false;
    }

    return clearActivation(error);
}

ValidationResult LicenseManager::importOfflineCertificate(const juce::String& signedCertificateJson)
{
    juce::String error;
    const auto cert = LicenseVerifier::parseSignedCertificateJson(signedCertificateJson, error);
    if (!cert)
    {
        ValidationResult result { LicenseState::Invalid, false, 0u, 0, 0, error };
        publish_(result);
        return result;
    }

    auto result = verifier_.validateCertificate(*cert, machineHash_, nowUnixSeconds());
    (void) storeVerifiedCertificate(*cert, result);

    publish_(result);
    return result;
}

bool LicenseManager::storeVerifiedCertificate(const SignedCertificate& certificate, ValidationResult& result)
{
    if (!result.enablesPremiumFeatures)
        return false;

    juce::String saveError;
    if (store_.saveCertificate(certificate, &saveError))
        return true;

    result.state = LicenseState::Invalid;
    result.enablesPremiumFeatures = false;
    result.featureMask = 0u;
    result.message = saveError;
    return false;
}

bool LicenseManager::clearActivation(juce::String* error)
{
    if (!store_.clearCertificate(error))
        return false;

    ValidationResult result {
        LicenseState::ActivationRequired,
        false,
        0u,
        0,
        0,
        "Activation removed from this computer."
    };
    lastActivationId_.clear();
    publish_(result);
    return true;
}

} // namespace more_phi::licensing