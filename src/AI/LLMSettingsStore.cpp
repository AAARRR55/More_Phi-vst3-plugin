#include "LLMSettingsStore.h"

namespace more_phi {

namespace {

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

}

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

    const auto contents = configFile_.loadFileAsString();
    if (contents.trim().isEmpty())
        return true;

    juce::var parsed;
    if (juce::JSON::parse(contents, parsed).failed() || ! parsed.isObject())
    {
        error = readErrorMessage();
        return false;
    }

    const auto* root = parsed.getDynamicObject();
    if (root == nullptr)
    {
        error = readErrorMessage();
        return false;
    }

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

    if (! configFile_.replaceWithText(juce::JSON::toString(juce::var(root.get()), true)))
    {
        error = writeErrorMessage();
        return false;
    }

    return true;
}

}
