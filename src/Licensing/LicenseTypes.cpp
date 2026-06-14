#include "LicenseTypes.h"

namespace more_phi::licensing {

juce::String toString(LicenseState state)
{
    switch (state)
    {
        case LicenseState::TrialActive: return "trial_active";
        case LicenseState::Licensed: return "licensed";
        case LicenseState::GracePeriod: return "grace_period";
        case LicenseState::Expired: return "expired";
        case LicenseState::Invalid: return "invalid";
        case LicenseState::ActivationRequired: return "activation_required";
        case LicenseState::FloatingLeaseUnavailable: return "floating_lease_unavailable";
        case LicenseState::Unknown: break;
    }
    return "unknown";
}

juce::String toString(LicenseType type)
{
    switch (type)
    {
        case LicenseType::Trial: return "trial";
        case LicenseType::Perpetual: return "perpetual";
        case LicenseType::Subscription: return "subscription";
        case LicenseType::NodeLocked: return "node_locked";
        case LicenseType::Floating: return "floating";
        case LicenseType::Unknown: break;
    }
    return "unknown";
}

LicenseType licenseTypeFromString(const juce::String& text)
{
    const auto normalized = text.trim().toLowerCase();
    if (normalized == "trial") return LicenseType::Trial;
    if (normalized == "perpetual") return LicenseType::Perpetual;
    if (normalized == "subscription") return LicenseType::Subscription;
    if (normalized == "node_locked" || normalized == "node-locked") return LicenseType::NodeLocked;
    if (normalized == "floating") return LicenseType::Floating;
    return LicenseType::Unknown;
}

uint32_t featureMaskFromStrings(const std::vector<std::string>& features)
{
    uint32_t mask = 0;
    for (const auto& feature : features)
    {
        const auto name = juce::String(feature).trim().toLowerCase();
        if (name == "morph_pad") mask |= static_cast<uint32_t>(Feature::MorphPad);
        else if (name == "plugin_hosting") mask |= static_cast<uint32_t>(Feature::PluginHosting);
        else if (name == "mcp") mask |= static_cast<uint32_t>(Feature::MCP);
        else if (name == "premium_presets") mask |= static_cast<uint32_t>(Feature::PremiumPresets);
        else if (name == "audio_domain_morphing") mask |= static_cast<uint32_t>(Feature::AudioDomainMorphing);
        else if (name == "floating_seats") mask |= static_cast<uint32_t>(Feature::FloatingSeats);
    }
    return mask;
}

} // namespace more_phi::licensing