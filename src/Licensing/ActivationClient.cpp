#include "ActivationClient.h"

namespace more_phi::licensing {

ActivationResponse StubActivationClient::activate(const ActivationRequest&)
{
    return {
        false,
        "not_configured",
        "ACTIVATION_CLIENT_STUB",
        "Online activation client is not configured yet.",
        std::nullopt
    };
}

ActivationResponse StubActivationClient::refresh(const juce::String&, const juce::String&)
{
    return {
        false,
        "not_configured",
        "ACTIVATION_CLIENT_STUB",
        "Online license refresh client is not configured yet.",
        std::nullopt
    };
}

ActivationResponse StubActivationClient::deactivate(const juce::String&, const juce::String&)
{
    return {
        false,
        "not_configured",
        "ACTIVATION_CLIENT_STUB",
        "Online deactivation client is not configured yet.",
        std::nullopt
    };
}

} // namespace more_phi::licensing