#include "LLMConnectionValidator.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <winhttp.h>
#endif

namespace more_phi {

namespace {

constexpr auto testPrompt = "Reply with exactly: OK";

juce::String trimTrailingSlash(juce::String url)
{
    return url.trim().trimCharactersAtEnd("/");
}

bool isValidHttpsUrl(const juce::String& value)
{
    const auto trimmed = value.trim();
    return trimmed.startsWithIgnoreCase("https://") && trimmed.length() > 8
           && !trimmed.containsChar(' ');
}

bool isLoopbackHost(const juce::String& host)
{
    const auto normalized = host.trim().toLowerCase();
    return normalized == "localhost"
        || normalized == "127.0.0.1"
        || normalized == "::1"
        || normalized == "[::1]";
}

bool isValidOpenAICompatibleUrl(const juce::String& value)
{
    const auto trimmed = value.trim();
    if (trimmed.containsChar(' '))
        return false;

    if (isValidHttpsUrl(trimmed))
        return true;

    if (!trimmed.startsWithIgnoreCase("http://") || trimmed.length() <= 7)
        return false;

    const auto withoutScheme = trimmed.substring(7);
    const auto slashIndex = withoutScheme.indexOfChar('/');
    const auto authority = slashIndex >= 0 ? withoutScheme.substring(0, slashIndex)
                                           : withoutScheme;
    const auto host = authority.startsWithChar('[')
        ? authority.upToFirstOccurrenceOf("]", true, false)
        : authority.upToFirstOccurrenceOf(":", false, false);

    return isLoopbackHost(host);
}

juce::String baseUrlFor(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto& definition = getLLMProviderDefinition(providerId);
    if (definition.customBaseUrlAllowed)
        return trimTrailingSlash(settings.customBaseUrl);
    return trimTrailingSlash(definition.fixedBaseUrl);
}

int timeoutFor(LLMProviderId providerId, LLMValidationOperation operation) noexcept
{
    if (providerId == LLMProviderId::NVIDIA)
        return operation == LLMValidationOperation::TestPrompt ? 120000 : 30000;

    return operation == LLMValidationOperation::TestPrompt ? 30000 : 20000;
}

LLMValidationResult inputError(const juce::String& message)
{
    return {false, LLMValidationStatus::Failed, message};
}

LLMModelFetchResult modelError(const juce::String& message)
{
    return {false, LLMValidationStatus::Failed, message, {}};
}

juce::String authHeadersFor(LLMProviderId providerId, const juce::String& apiKey)
{
    if (providerId == LLMProviderId::Anthropic)
        return "x-api-key: " + apiKey + "\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n";

    return "Authorization: Bearer " + apiKey + "\r\ncontent-type: application/json\r\n";
}

void addModelId(juce::StringArray& models, const nlohmann::json& item)
{
    if (item.is_string())
    {
        models.add(juce::String(item.get<std::string>()));
        return;
    }

    if (!item.is_object())
        return;

    static constexpr const char* keys[] { "id", "name", "model" };
    for (const auto* key : keys)
    {
        if (item.contains(key) && item[key].is_string())
        {
            models.add(juce::String(item[key].get<std::string>()));
            return;
        }
    }
}

void collectModels(juce::StringArray& models, const nlohmann::json& value)
{
    if (!value.is_array())
        return;

    for (const auto& item : value)
        addModelId(models, item);
}

juce::String extractTextContent(const nlohmann::json& content)
{
    if (content.is_string())
        return juce::String(content.get<std::string>());

    if (!content.is_array())
        return {};

    juce::String text;
    for (const auto& block : content)
    {
        if (block.is_string())
        {
            text += juce::String(block.get<std::string>());
        }
        else if (block.is_object() && block.contains("text") && block["text"].is_string())
        {
            text += juce::String(block["text"].get<std::string>());
        }
    }

    return text;
}

juce::String extractOpenAIText(const nlohmann::json& root)
{
    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty())
        return {};

    const auto& first = root["choices"].front();
    if (!first.contains("message") || !first["message"].is_object())
    {
        if (first.contains("text"))
            return extractTextContent(first["text"]);

        return {};
    }

    const auto& message = first["message"];
    if (!message.contains("content"))
        return {};

    return extractTextContent(message["content"]);
}

juce::String extractAnthropicText(const nlohmann::json& root)
{
    if (!root.contains("content") || !root["content"].is_array())
        return {};

    juce::String text;
    for (const auto& block : root["content"])
    {
        if (block.is_object() && block.contains("text") && block["text"].is_string())
            text += juce::String(block["text"].get<std::string>());
    }
    return text;
}

#if JUCE_WINDOWS
const char* winHttpFriendlyText(DWORD code)
{
    // Translate the most common WinHTTP transport errors so users see actionable
    // guidance instead of a bare numeric code. Reference: winhttp.h ERROR_WINHTTP_*.
    switch (code)
    {
        case 12002: return "request timed out";
        case 12007: return "host name could not be resolved (check DNS / internet connection)";
        case 12029: return "could not connect to the server (check the base URL and your firewall)";
        case 12030: return "connection was aborted by the server";
        case 12175: return "TLS/SSL handshake failed (check certificate / proxy)";
        default:    return nullptr;
    }
}

LLMHttpResponse winHttpFailure(const char* stage, DWORD errorCode = GetLastError())
{
    juce::String message = juce::String("WinHTTP ") + stage + " failed with error "
                         + juce::String(static_cast<int>(errorCode));
    if (const char* friendly = winHttpFriendlyText(errorCode))
        message += juce::String(" (") + friendly + ")";
    message += ".";
    return {-static_cast<int>(errorCode), message, {}};
}

struct WinHttpHandle
{
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle()
    {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    operator HINTERNET() const noexcept { return handle; }
};

std::wstring toWideString(const juce::String& text)
{
    return std::wstring(text.toWideCharPointer());
}

LLMHttpResponse executeWithWinHttp(const LLMHttpRequest& request)
{
    const auto urlWide = toWideString(request.url);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(urlWide.c_str(), static_cast<DWORD>(urlWide.size()), 0, &parts))
        return winHttpFailure("URL parse");

    const bool isHttps = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const bool isHttp = parts.nScheme == INTERNET_SCHEME_HTTP;
    if (!isHttps && !isHttp)
        return {-1, "WinHTTP request failed: only http:// and https:// URLs are supported.", {}};

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";

    const DWORD accessType =
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#endif

    WinHttpHandle session(WinHttpOpen(L"MorePhi/3.3.0",
                                      accessType,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0));
    if (session.handle == nullptr)
        return winHttpFailure("session open");

    WinHttpSetTimeouts(session,
                       (std::min)(request.timeoutMs, 10000),  // DNS resolve
                       (std::min)(request.timeoutMs, 15000),  // connect
                       (std::min)(request.timeoutMs, 30000),  // send
                       request.timeoutMs);                     // receive (full timeout for LLM generation)

    WinHttpHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (connection.handle == nullptr)
        return winHttpFailure("connect");

    const auto method = request.method == LLMHttpMethod::Post ? L"POST" : L"GET";
    WinHttpHandle httpRequest(WinHttpOpenRequest(connection,
                                                 method,
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 isHttps ? WINHTTP_FLAG_SECURE : 0));
    if (httpRequest.handle == nullptr)
        return winHttpFailure("request open");

    const auto headers = toWideString(request.extraHeaders);
    if (!headers.empty())
    {
        if (!WinHttpAddRequestHeaders(httpRequest,
                                      headers.c_str(),
                                      static_cast<DWORD>(headers.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        {
            return winHttpFailure("add headers");
        }
    }

    const auto body = request.body.toRawUTF8();
    const DWORD bodyBytes = request.method == LLMHttpMethod::Post
                                ? static_cast<DWORD>(std::strlen(body))
                                : 0;

    if (!WinHttpSendRequest(httpRequest,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            bodyBytes > 0 ? const_cast<char*>(body) : WINHTTP_NO_REQUEST_DATA,
                            bodyBytes,
                            bodyBytes,
                            0))
    {
        return winHttpFailure("send request");
    }

    if (!WinHttpReceiveResponse(httpRequest, nullptr))
        return winHttpFailure("receive response");

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(httpRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusCodeSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode == 0)
        return winHttpFailure("query status");

    juce::MemoryOutputStream responseBody;
    for (;;)
    {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(httpRequest, &bytesAvailable) || bytesAvailable == 0)
            break;

        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(httpRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0)
            break;

        responseBody.write(buffer.data(), bytesRead);
    }

    return {static_cast<int>(statusCode), responseBody.toString(), {}};
}
#endif

} // namespace

LLMHttpResponse JuceLLMHttpClient::execute(const LLMHttpRequest& request)
{
#if JUCE_WINDOWS
    return executeWithWinHttp(request);
#else
    int statusCode = 0;
    juce::StringPairArray responseHeaders;
    auto url = juce::URL(request.url);

    if (request.method == LLMHttpMethod::Post)
        url = url.withPOSTData(request.body);

    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(request.timeoutMs)
            .withExtraHeaders(request.extraHeaders)
            .withStatusCode(&statusCode)
            .withResponseHeaders(&responseHeaders)
            .withNumRedirectsToFollow(3));
    if (stream == nullptr)
        return {statusCode, {}, {}};

    return {statusCode, stream->readEntireStreamAsString(), responseHeaders.getDescription()};
#endif
}

LLMConnectionValidator::LLMConnectionValidator()
    : httpClient_(std::make_shared<JuceLLMHttpClient>())
{
}

LLMConnectionValidator::LLMConnectionValidator(std::shared_ptr<ILLMHttpClient> httpClient)
    : httpClient_(std::move(httpClient))
{
}

LLMValidationResult LLMConnectionValidator::validateInputsForTest(LLMProviderId providerId,
                                                                  const LLMProviderSettings& settings,
                                                                  LLMValidationOperation operation)
{
    if (settings.apiKey.trim().isEmpty())
        return inputError("Missing API key.");

    const auto baseUrl = baseUrlFor(providerId, settings);
    if (providerId == LLMProviderId::OpenAICompatible)
    {
        if (!isValidOpenAICompatibleUrl(baseUrl))
            return inputError("Invalid OpenAI Compatible Base URL. Use https://, or http://localhost for a local server.");
    }
    else if (!isValidHttpsUrl(baseUrl))
    {
        return inputError("Invalid provider Base URL. Use an https:// URL.");
    }

    if (operation == LLMValidationOperation::TestPrompt && settings.selectedModel.trim().isEmpty())
        return inputError("Missing model selection.");

    return {true, LLMValidationStatus::Untested, {}};
}

LLMHttpRequest LLMConnectionValidator::buildFetchModelsRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto baseUrl = baseUrlFor(providerId, settings);
    const auto path = providerId == LLMProviderId::Anthropic ? "/v1/models" : "/models";
    return {LLMHttpMethod::Get,
            baseUrl + path,
            authHeadersFor(providerId, settings.apiKey.trim()),
            {},
            timeoutFor(providerId, LLMValidationOperation::FetchModels)};
}

LLMHttpRequest LLMConnectionValidator::buildTestPromptRequestForTest(LLMProviderId providerId, const LLMProviderSettings& settings)
{
    const auto baseUrl = baseUrlFor(providerId, settings);
    const auto model = settings.selectedModel.trim();

    if (providerId == LLMProviderId::Anthropic)
    {
        nlohmann::json body;
        body["model"] = model.toStdString();
        body["max_tokens"] = 16;
        body["messages"] = {{{"role", "user"}, {"content", testPrompt}}};
        return {LLMHttpMethod::Post,
                baseUrl + "/v1/messages",
                authHeadersFor(providerId, settings.apiKey.trim()),
                juce::String(body.dump()),
                timeoutFor(providerId, LLMValidationOperation::TestPrompt)};
    }

    nlohmann::json body;
    body["model"] = model.toStdString();
    body["max_tokens"] = 16;
    body["messages"] = {{{"role", "user"}, {"content", testPrompt}}};
    return {LLMHttpMethod::Post,
            baseUrl + "/chat/completions",
            authHeadersFor(providerId, settings.apiKey.trim()),
            juce::String(body.dump()),
            timeoutFor(providerId, LLMValidationOperation::TestPrompt)};
}

LLMModelFetchResult LLMConnectionValidator::parseModelListForTest(LLMProviderId /*providerId*/, int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return modelError("Authentication failed while fetching models.");
    if (statusCode <= 0)
        return modelError(body.isNotEmpty() ? body : "Network timeout while fetching models.");
    if (statusCode < 200 || statusCode >= 300)
        return modelError("Model list request failed with HTTP " + juce::String(statusCode) + ".");

    try
    {
        const auto root = nlohmann::json::parse(body.toStdString());
        juce::StringArray models;
        if (root.contains("data"))
            collectModels(models, root["data"]);
        if (models.isEmpty() && root.contains("models"))
            collectModels(models, root["models"]);
        if (models.isEmpty() && root.is_array())
            collectModels(models, root);

        models.removeEmptyStrings();
        models.removeDuplicates(false);
        if (models.isEmpty())
            return modelError("Model list parse failure: no model IDs returned.");

        return {true, LLMValidationStatus::Untested, "Fetched " + juce::String(models.size()) + " models.", models};
    }
    catch (const std::exception& e)
    {
        return modelError("Model list parse failure: " + juce::String(e.what()));
    }
}

LLMValidationResult LLMConnectionValidator::parseTestPromptForTest(LLMProviderId providerId, int statusCode, const juce::String& body)
{
    if (statusCode == 401 || statusCode == 403)
        return inputError("Authentication failed while testing the provider.");
    if (statusCode <= 0)
        return inputError(body.isNotEmpty() ? body : "Network timeout while testing the provider.");
    if (statusCode < 200 || statusCode >= 300)
        return inputError("Test prompt failed with HTTP " + juce::String(statusCode) + ".");

    try
    {
        const auto root = nlohmann::json::parse(body.toStdString());
        const auto text = providerId == LLMProviderId::Anthropic ? extractAnthropicText(root) : extractOpenAIText(root);
        if (!text.trim().contains("OK"))
            return inputError("Test prompt failure: provider did not reply with OK.");

        return {true, LLMValidationStatus::Active, "Connection test succeeded."};
    }
    catch (const std::exception& e)
    {
        return inputError("Unsupported provider response format: " + juce::String(e.what()));
    }
}

void LLMConnectionValidator::fetchModelsAsync(LLMProviderId providerId, LLMProviderSettings settings, ModelCallback callback) const
{
    const auto input = validateInputsForTest(providerId, settings, LLMValidationOperation::FetchModels);
    if (!input.success)
    {
        juce::MessageManager::callAsync([callback = std::move(callback), input]() mutable {
            callback({false, input.status, input.message, {}});
        });
        return;
    }

    auto client = httpClient_;
    std::thread([client, providerId, settings = std::move(settings), callback = std::move(callback)]() mutable {
        const auto request = buildFetchModelsRequestForTest(providerId, settings);
        const auto response = client->execute(request);
        auto result = parseModelListForTest(providerId, response.statusCode, response.body);
        juce::MessageManager::callAsync([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    }).detach();
}

void LLMConnectionValidator::testConnectionAsync(LLMProviderId providerId, LLMProviderSettings settings, TestCallback callback) const
{
    const auto input = validateInputsForTest(providerId, settings, LLMValidationOperation::TestPrompt);
    if (!input.success)
    {
        juce::MessageManager::callAsync([callback = std::move(callback), input]() mutable {
            callback(input);
        });
        return;
    }

    auto client = httpClient_;
    std::thread([client, providerId, settings = std::move(settings), callback = std::move(callback)]() mutable {
        if (providerId == LLMProviderId::NVIDIA)
        {
            const auto request = buildFetchModelsRequestForTest(providerId, settings);
            const auto response = client->execute(request);
            auto models = parseModelListForTest(providerId, response.statusCode, response.body);
            LLMValidationResult result;
            if (!models.success)
            {
                result = {false, models.status, "NVIDIA model endpoint check failed: " + models.message};
            }
            else if (!models.models.contains(settings.selectedModel.trim()))
            {
                result = {false,
                          LLMValidationStatus::Failed,
                          "NVIDIA endpoint is reachable, but the selected model was not returned by /models."};
            }
            else
            {
                result = {true,
                          LLMValidationStatus::Active,
                          "NVIDIA endpoint reachable and selected model is available."};
            }

            juce::MessageManager::callAsync([callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
            return;
        }

        const auto request = buildTestPromptRequestForTest(providerId, settings);
        const auto response = client->execute(request);
        auto result = parseTestPromptForTest(providerId, response.statusCode, response.body);
        juce::MessageManager::callAsync([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    }).detach();
}

} // namespace more_phi
