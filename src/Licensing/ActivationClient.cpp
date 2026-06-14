#include "ActivationClient.h"

#include <cstdlib>
#include <utility>

#include <nlohmann/json.hpp>

#include "LicenseVerifier.h"

namespace more_phi::licensing {
namespace {

juce::String envString(const char* key)
{
    if (const auto* value = std::getenv(key))
        return juce::String(value).trim();
    return {};
}

juce::String cleanBaseUrl(juce::String url)
{
    url = url.trim();
    while (url.endsWithChar('/'))
        url = url.dropLastCharacters(1);
    return url;
}

juce::String jsonStringOrEmpty(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || json.at(key).is_null())
        return {};
    if (json.at(key).is_string())
        return juce::String(json.at(key).get<std::string>());
    return {};
}

std::string base64UrlEncode(const std::string& text)
{
    auto encoded = juce::Base64::toBase64(text.data(), text.size()).toStdString();
    while (!encoded.empty() && encoded.back() == '=')
        encoded.pop_back();
    for (auto& c : encoded)
    {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return encoded;
}

std::string normalizePayloadEnvelope(std::string payload)
{
    const auto trimmed = juce::String(payload).trim();
    if (trimmed.startsWithChar('{') || trimmed.startsWithChar('['))
        return base64UrlEncode(trimmed.toStdString());

    return payload;
}

std::optional<SignedCertificate> certificateFromResponse(const nlohmann::json& root)
{
    if (root.contains("certificate") && root.at("certificate").is_object())
    {
        const auto& certJson = root.at("certificate");
        SignedCertificate cert;
        cert.payloadBase64Url = certJson.value("payload", "");
        cert.signatureBase64Url = certJson.value("signature", "");
        cert.keyId = certJson.value("keyId", certJson.value("public_key_id", ""));
        if (!cert.payloadBase64Url.empty() && !cert.signatureBase64Url.empty() && !cert.keyId.empty())
            return cert;
    }

    if (root.contains("certificate_payload") && root.contains("signature") && root.contains("public_key_id"))
    {
        const auto payload = root.at("certificate_payload").is_string()
            ? root.at("certificate_payload").get<std::string>()
            : std::string();
        const auto signature = root.at("signature").is_string()
            ? root.at("signature").get<std::string>()
            : std::string();
        const auto keyId = root.at("public_key_id").is_string()
            ? root.at("public_key_id").get<std::string>()
            : std::string();

        if (!payload.empty() && !signature.empty() && !keyId.empty())
            return SignedCertificate { normalizePayloadEnvelope(payload), signature, keyId };
    }

    return std::nullopt;
}

ActivationResponse parseActivationResponse(int statusCode, const juce::String& body, bool requireCertificate)
{
    ActivationResponse response;
    response.success = statusCode >= 200 && statusCode < 300;
    response.status = response.success ? "ok" : "http_error";

    if (body.trim().isEmpty())
    {
        response.message = response.success ? "Activation server returned an empty response." : "Activation server request failed.";
        response.success = false;
        return response;
    }

    try
    {
        const auto root = nlohmann::json::parse(body.toStdString());
        response.status = jsonStringOrEmpty(root, "status");
        response.errorCode = jsonStringOrEmpty(root, "error_code");
        if (response.errorCode.isEmpty())
            response.errorCode = jsonStringOrEmpty(root, "code");
        response.message = jsonStringOrEmpty(root, "message");

        if (auto cert = certificateFromResponse(root))
            response.certificate = *cert;

        response.success = response.success && (!requireCertificate || response.certificate.has_value());
        if (!response.success && response.message.isEmpty())
            response.message = requireCertificate
                ? "Activation server did not return a usable signed certificate."
                : "License server request failed.";
        return response;
    }
    catch (const std::exception& e)
    {
        response.success = false;
        response.errorCode = "INVALID_SERVER_RESPONSE";
        response.message = juce::String("Activation server returned invalid JSON: ") + e.what();
    }

    return response;
}

} // namespace

HttpActivationClient::HttpActivationClient(LicenseApiConfig config)
    : config_(std::move(config))
{
    config_.baseUrl = cleanBaseUrl(config_.baseUrl);
}

LicenseApiConfig HttpActivationClient::configFromEnvironment()
{
    LicenseApiConfig config;
    config.baseUrl = envString("LICENSE_API_BASE_URL");
    config.publicClientToken = envString("MOREPHI_PUBLIC_CLIENT_TOKEN");

    if (auto headerName = envString("MOREPHI_CLIENT_HEADER"); headerName.isNotEmpty())
        config.clientHeaderName = headerName;

    return config;
}

std::unique_ptr<IActivationClient> HttpActivationClient::createFromEnvironment()
{
    auto config = configFromEnvironment();
    if (!config.isUsable())
        return std::make_unique<StubActivationClient>();

    return std::make_unique<HttpActivationClient>(std::move(config));
}

juce::String HttpActivationClient::endpoint_(const juce::String& path) const
{
    if (path.startsWithChar('/'))
        return config_.baseUrl + path;
    return config_.baseUrl + "/" + path;
}

ActivationResponse HttpActivationClient::post_(const juce::String& path,
                                               const juce::String& body,
                                               bool requireCertificate) const
{
    if (!config_.isUsable())
    {
        return {
            false,
            "not_configured",
            "LICENSE_API_CONFIG_MISSING",
            "LICENSE_API_BASE_URL and MOREPHI_PUBLIC_CLIENT_TOKEN must be configured.",
            std::nullopt
        };
    }

    int statusCode = 0;
    juce::StringPairArray responseHeaders;
    auto url = juce::URL(endpoint_(path)).withPOSTData(body);
    const juce::String headers = "Content-Type: application/json\r\n"
        + config_.clientHeaderName + ": " + config_.publicClientToken + "\r\n";

    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(config_.timeoutMs)
            .withExtraHeaders(headers)
            .withStatusCode(&statusCode)
            .withResponseHeaders(&responseHeaders)
            .withNumRedirectsToFollow(3));

    if (stream == nullptr)
    {
        return {
            false,
            "network_error",
            "LICENSE_API_UNREACHABLE",
            "Could not reach the license activation server.",
            std::nullopt
        };
    }

    return parseActivationResponse(statusCode, stream->readEntireStreamAsString(), requireCertificate);
}

ActivationResponse HttpActivationClient::activate(const ActivationRequest& request)
{
    nlohmann::json body;
    body["license_key"] = request.licenseKey.toStdString();
    body["machine_id"] = request.machineHash.toStdString();
    body["plugin_version"] = request.pluginVersion.toStdString();
    body["platform"] = request.os.toStdString();
    body["daw"] = request.dawHint.toStdString();
    body["product_id"] = request.productId.toStdString();
    body["request_nonce"] = request.requestNonce.toStdString();
    return post_("/plugin/licenses/activate", juce::String(body.dump()), true);
}

ActivationResponse HttpActivationClient::refresh(const juce::String& activationId,
                                                 const juce::String& machineHash)
{
    nlohmann::json body;
    body["activation_id"] = activationId.toStdString();
    body["machine_id"] = machineHash.toStdString();
    body["product_id"] = PRODUCT_ID;
    return post_("/plugin/licenses/refresh", juce::String(body.dump()), true);
}

ActivationResponse HttpActivationClient::deactivate(const juce::String& activationId,
                                                    const juce::String& machineHash)
{
    nlohmann::json body;
    body["activation_id"] = activationId.toStdString();
    body["machine_id"] = machineHash.toStdString();
    body["product_id"] = PRODUCT_ID;
    return post_("/plugin/licenses/deactivate", juce::String(body.dump()), false);
}

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