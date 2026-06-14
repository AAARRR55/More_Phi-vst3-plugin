/*
 * More-Phi — Licensing/LicenseTypes.h
 * License model and real-time-safe runtime state.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace more_phi::licensing {

inline constexpr const char* PRODUCT_ID = "more-phi-vst3";
inline constexpr const char* LICENSE_SCHEMA = "morephi-license-cert-v1";
inline constexpr const char* LICENSE_KEY_PREFIX = "MPH1";

enum class LicenseType : uint8_t
{
    Unknown = 0,
    Trial,
    Perpetual,
    Subscription,
    NodeLocked,
    Floating
};

enum class LicenseState : uint8_t
{
    Unknown = 0,
    TrialActive,
    Licensed,
    GracePeriod,
    Expired,
    Invalid,
    ActivationRequired,
    FloatingLeaseUnavailable
};

enum class Feature : uint32_t
{
    MorphPad = 1u << 0u,
    PluginHosting = 1u << 1u,
    MCP = 1u << 2u,
    PremiumPresets = 1u << 3u,
    AudioDomainMorphing = 1u << 4u,
    FloatingSeats = 1u << 5u
};

struct SignedCertificate
{
    std::string payloadBase64Url;
    std::string signatureBase64Url;
    std::string keyId;
};

struct LicensePayload
{
    std::string schema;
    std::string licenseId;
    std::string activationId;
    std::string productId;
    LicenseType licenseType = LicenseType::Unknown;
    uint32_t featureMask = 0;
    int64_t issuedAtUnix = 0;
    int64_t validFromUnix = 0;
    std::optional<int64_t> validUntilUnix;
    std::optional<int64_t> trialEndsAtUnix;
    std::optional<int64_t> subscriptionEndsAtUnix;
    std::optional<int64_t> leaseEndsAtUnix;
    int64_t nextOnlineCheckAtUnix = 0;
    int64_t offlineGraceEndsAtUnix = 0;
    std::string machineHash;
    std::string nonce;
};

struct ValidationResult
{
    LicenseState state = LicenseState::Unknown;
    bool enablesPremiumFeatures = false;
    uint32_t featureMask = 0;
    int64_t nextOnlineCheckAtUnix = 0;
    int64_t graceEndsUnix = 0;
    juce::String message;
};

struct LicenseRuntimeState
{
    std::atomic<LicenseState> state { LicenseState::Unknown };
    std::atomic<bool> premiumFeaturesEnabled { false };
    std::atomic<uint32_t> featureMask { 0 };
    std::atomic<int64_t> graceEndsUnixSeconds { 0 };
    std::atomic<int64_t> nextCheckUnixSeconds { 0 };

    bool isFeatureEnabled(Feature feature) const noexcept
    {
        if (!premiumFeaturesEnabled.load(std::memory_order_relaxed))
            return false;

        const auto mask = featureMask.load(std::memory_order_relaxed);
        return (mask & static_cast<uint32_t>(feature)) != 0u;
    }
};

juce::String toString(LicenseState state);
juce::String toString(LicenseType type);
LicenseType licenseTypeFromString(const juce::String& text);
uint32_t featureMaskFromStrings(const std::vector<std::string>& features);

} // namespace more_phi::licensing