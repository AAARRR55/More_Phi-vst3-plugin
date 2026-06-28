#include "LLMSettingsStore.h"

#include "Licensing/LicenseEnvelopeCrypto.h"
#include "Licensing/MachineFingerprint.h"

#include <string>

namespace more_phi {

namespace {

// Encrypted envelope schema version for LLM settings (distinct from license certs)
constexpr int kLLMEnvelopeSchema = 1;

// Machine-fingerprint product namespace (separate from licensing's product ID)
constexpr const char* kLLMFingerprintProduct = "more-phi-llm-v1";

juce::String readErrorMessage()
{
    return "Unable to read LLM settings. Your settings have been reset to defaults.";
}

juce::String writeErrorMessage()
{
    return "Unable to save LLM settings. Please check that MorePhi can write to your application data folder.";
}

const juce::var& getObjectProperty(const juce::DynamicObject& object, const juce::Identifier& propertyName)
{
    return object.getProperty(propertyName);
}

void loadStringProperty(const juce::DynamicObject& object,
                        const juce::Identifier& propertyName,
                        juce::String& destination)
{
    const auto& value = getObjectProperty(object, propertyName);
    if (value.isString())
        destination = value.toString();
}

void loadInt64Property(const juce::DynamicObject& object,
                       const juce::Identifier& propertyName,
                       juce::int64& destination)
{
    const auto& value = getObjectProperty(object, propertyName);
    if (value.isInt() || value.isInt64())
        destination = static_cast<juce::int64>(value);
}

void loadStringArrayProperty(const juce::DynamicObject& object,
                             const juce::Identifier& propertyName,
                             juce::StringArray& destination)
{
    const auto& value = getObjectProperty(object, propertyName);
    if (! value.isArray())
        return;

    juce::StringArray loaded;
    if (const auto* array = value.getArray())
    {
        for (const auto& item : *array)
        {
            if (item.isString())
                loaded.add(item.toString());
        }
    }

    destination = loaded;
}

void loadValidationStatusProperty(const juce::DynamicObject& object, LLMProviderSettings& settings)
{
    const auto& value = getObjectProperty(object, "validationStatus");
    if (! value.isString())
        return;

    if (const auto status = llmValidationStatusFromStorageKey(value.toString()))
        settings.validationStatus = *status;
}

void loadProviderSettings(const juce::DynamicObject& object, LLMProviderSettings& settings)
{
    loadStringProperty(object, "apiKey", settings.apiKey);
    loadStringProperty(object, "customBaseUrl", settings.customBaseUrl);
    loadStringProperty(object, "selectedModel", settings.selectedModel);
    loadStringArrayProperty(object, "availableModels", settings.availableModels);
    loadValidationStatusProperty(object, settings);
    loadInt64Property(object, "validationTimestampMs", settings.validationTimestampMs);
    loadStringProperty(object, "validationMessage", settings.validationMessage);
}

juce::var stringArrayToVar(const juce::StringArray& strings)
{
    juce::Array<juce::var> result;
    result.ensureStorageAllocated(strings.size());

    for (const auto& string : strings)
        result.add(string);

    return result;
}

juce::DynamicObject::Ptr providerSettingsToObject(const LLMProviderDefinition& definition,
                                                  const LLMProviderSettings& settings)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("apiKey", settings.apiKey);

    if (definition.id == LLMProviderId::OpenAICompatible)
        object->setProperty("customBaseUrl", settings.customBaseUrl);

    object->setProperty("selectedModel", settings.selectedModel);
    object->setProperty("availableModels", stringArrayToVar(settings.availableModels));
    object->setProperty("validationStatus", toStorageKey(settings.validationStatus));
    object->setProperty("validationTimestampMs", settings.validationTimestampMs);
    object->setProperty("validationMessage", settings.validationMessage);
    return object;
}

// ── Machine-bound fingerprint for LLM settings encryption ───────────────────
// SECURITY (Finding #2): Binds encrypted llm_settings.json to a specific machine
// so cross-machine copying is ineffective.
juce::String llmMachineFingerprint()
{
    // Reuse the licensing module's machine fingerprint with a distinct product
    // namespace so it's decoupled from license cert machine hashes.
    return licensing::MachineFingerprint::computeMachineHash(kLLMFingerprintProduct);
}

// ── Encrypted envelope helpers ──────────────────────────────────────────────
// Format: { "schema": 1, "method": "<dpapi|blowfish>", "enc": "<base64>" }

juce::String buildEnvelope(const juce::String& plaintextJson)
{
    std::string method;
    const auto machineHash = llmMachineFingerprint();
    const auto encrypted = licensing::LicenseEnvelopeCrypto::encrypt(plaintextJson, machineHash, method);

    if (encrypted.empty() || method.empty())
        return {};

    juce::DynamicObject::Ptr envelope = new juce::DynamicObject();
    envelope->setProperty("schema", kLLMEnvelopeSchema);
    envelope->setProperty("method", juce::String(method));
    envelope->setProperty("enc", juce::String(encrypted));
    return juce::JSON::toString(juce::var(envelope.get()), false);
}

juce::String extractFromEnvelope(const juce::String& envelopeJson)
{
    juce::var parsed;
    if (juce::JSON::parse(envelopeJson, parsed).failed() || ! parsed.isObject())
        return {};

    const auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return {};

    const auto method = obj->getProperty("method").toString();
    const auto enc = obj->getProperty("enc").toString();
    if (method.isEmpty() || enc.isEmpty())
        return {};

    const auto machineHash = llmMachineFingerprint();
    return licensing::LicenseEnvelopeCrypto::decrypt(method.toStdString(), enc, machineHash);
}

} // namespace

LLMSettingsStore::LLMSettingsStore()
    : configFile_(defaultConfigFile())
{
}

LLMSettingsStore::LLMSettingsStore(juce::File configFile)
    : configFile_(std::move(configFile))
{
}

juce::File LLMSettingsStore::defaultConfigFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MorePhi")
        .getChildFile("llm_settings.json");
}

bool LLMSettingsStore::load(LLMSettings& settings, juce::String& error) const
{
    settings = LLMSettings::createDefault();
    error.clear();

    if (! configFile_.existsAsFile())
        return true;

    auto contents = configFile_.loadFileAsString();
    if (contents.trim().isEmpty())
        return true;

    // ── Step 1: detect encrypted envelope ──────────────────────────────────
    // If the file starts with '{', try envelope detection via structural
    // inspection without full parse: look for "method" and "enc" keys.
    // ponytail: simple heuristic — check for envelope fields after parse.
    juce::var parsed;
    juce::DynamicObject* root = nullptr;

    // Try envelope decryption first
    const auto decrypted = extractFromEnvelope(contents);
    if (decrypted.isNotEmpty())
    {
        // Was an encrypted envelope — parse the decrypted content
        if (juce::JSON::parse(decrypted, parsed).failed() || ! parsed.isObject())
        {
            error = readErrorMessage();
            return false;
        }
        root = parsed.getDynamicObject();
    }
    else
    {
        // Not an envelope or decryption failed — try legacy plaintext parse
        // (handles migration from unencrypted to encrypted storage)
        if (juce::JSON::parse(contents, parsed).failed() || ! parsed.isObject())
        {
            error = readErrorMessage();
            return false;
        }
        root = parsed.getDynamicObject();
    }

    if (root == nullptr)
    {
        error = readErrorMessage();
        return false;
    }

    // ── Step 2: parse provider settings ────────────────────────────────────
    const auto& activeProvider = getObjectProperty(*root, "activeProvider");
    if (activeProvider.isString())
    {
        if (const auto providerId = llmProviderIdFromStorageKey(activeProvider.toString()))
            settings.activeProvider = *providerId;
    }

    const auto& providers = getObjectProperty(*root, "providers");
    if (const auto* providerRoot = providers.getDynamicObject())
    {
        for (const auto& definition : getLLMProviderDefinitions())
        {
            const auto& providerValue = getObjectProperty(*providerRoot, definition.storageKey);
            if (const auto* providerObject = providerValue.getDynamicObject())
                loadProviderSettings(*providerObject, settings.getProvider(definition.id));
        }
    }

    return true;
}

bool LLMSettingsStore::save(const LLMSettings& settings, juce::String& error) const
{
    error.clear();

    const auto directoryResult = configFile_.getParentDirectory().createDirectory();
    if (directoryResult.failed())
    {
        error = writeErrorMessage();
        return false;
    }

    // ── Step 1: build settings JSON ────────────────────────────────────────
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", 1);
    root->setProperty("activeProvider", settings.activeProvider.has_value() ? toStorageKey(*settings.activeProvider) : juce::String());

    juce::DynamicObject::Ptr providers = new juce::DynamicObject();
    for (const auto& definition : getLLMProviderDefinitions())
    {
        providers->setProperty(definition.storageKey,
                               juce::var(providerSettingsToObject(definition, settings.getProvider(definition.id)).get()));
    }

    root->setProperty("providers", juce::var(providers.get()));

    const auto plaintextJson = juce::JSON::toString(juce::var(root.get()), false);

    // ── Step 2: encrypt and write envelope ─────────────────────────────────
    // SECURITY (Finding #2): Uses same DPAPI/BlowFish infrastructure as the
    // licensing system. On Windows, DPAPI binds ciphertext to the current user
    // account. On other platforms, machine-bound BlowFish prevents cross-machine
    // extraction. Legacy plaintext files are transparently migrated on next save.
    const auto envelopeJson = buildEnvelope(plaintextJson);
    if (envelopeJson.isEmpty())
    {
        // Encryption failure — fall back to plaintext as last resort.
        // This should only happen in exotic environments (no DPAPI, no BlowFish).
        if (! configFile_.replaceWithText(plaintextJson))
        {
            error = writeErrorMessage();
            return false;
        }
        return true;
    }

    if (! configFile_.replaceWithText(envelopeJson))
    {
        error = writeErrorMessage();
        return false;
    }

    return true;
}

} // namespace more_phi
