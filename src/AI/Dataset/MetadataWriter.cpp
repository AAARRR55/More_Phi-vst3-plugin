/*
 * MorphSnap — AI/Dataset/MetadataWriter.cpp
 * Implementation of comprehensive metadata management for dataset generation.
 */
#include "MetadataWriter.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <juce_cryptography/juce_cryptography.h>

namespace morphsnap {

// ── DatasetMetadata Implementation ──────────────────────────────────────────────

void DatasetMetadata::generateSampleId()
{
    auto now = juce::Time::getCurrentTime();
    auto random = juce::Random::getSystemRandom().nextInt(9999);
    sampleId = juce::String::formatted("sample_%lld_%04d",
                                       now.toMilliseconds(),
                                       random);
}

// ── MetadataWriter Implementation ───────────────────────────────────────────────

MetadataWriter::MetadataWriter()
{
    initializeSchema();
}

// ── Write Operations ────────────────────────────────────────────────────────────

bool MetadataWriter::writeMetadata(const juce::File& outputFile,
                                   const DatasetMetadata& metadata)
{
    juce::String error;
    if (!validateMetadata(metadata, error))
    {
        DBG("MetadataWriter: Validation failed: " + error);
        return false;
    }

    nlohmann::json json = metadataToJson(metadata);

    std::ofstream file(outputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return false;

    file << json.dump(4);
    return true;
}

bool MetadataWriter::writeManifest(const juce::File& outputFile,
                                   const std::vector<DatasetMetadata>& metadataList)
{
    nlohmann::json manifest = nlohmann::json::array();

    for (const auto& metadata : metadataList)
    {
        juce::String error;
        if (validateMetadata(metadata, error))
        {
            manifest.push_back(metadataToJson(metadata));
        }
    }

    // Add manifest metadata
    nlohmann::json output;
    output["version"] = "1.0.0";
    output["generated_at"] = juce::Time::getCurrentTime().toMilliseconds();
    output["total_samples"] = manifest.size();
    output["samples"] = manifest;

    std::ofstream file(outputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return false;

    file << output.dump(4);
    return true;
}

// ── Read Operations ──────────────────────────────────────────────────────────────

std::optional<DatasetMetadata> MetadataWriter::readMetadata(const juce::File& inputFile)
{
    if (!inputFile.existsAsFile())
        return std::nullopt;

    std::ifstream file(inputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return std::nullopt;

    try
    {
        nlohmann::json json;
        file >> json;

        juce::String error;
        if (!validateAgainstSchema(json, error))
        {
            DBG("MetadataWriter: Schema validation failed: " + error);
            return std::nullopt;
        }

        return jsonToMetadata(json);
    }
    catch (const nlohmann::json::exception& e)
    {
        DBG("MetadataWriter: JSON parse error: " + juce::String(e.what()));
        return std::nullopt;
    }
}

std::vector<DatasetMetadata> MetadataWriter::readManifest(const juce::File& inputFile)
{
    std::vector<DatasetMetadata> result;

    if (!inputFile.existsAsFile())
        return result;

    std::ifstream file(inputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return result;

    try
    {
        nlohmann::json json;
        file >> json;

        // Handle both array and object with "samples" key
        nlohmann::json samples;
        if (json.is_array())
        {
            samples = json;
        }
        else if (json.contains("samples") && json["samples"].is_array())
        {
            samples = json["samples"];
        }
        else
        {
            return result;
        }

        for (const auto& sampleJson : samples)
        {
            juce::String error;
            if (validateAgainstSchema(sampleJson, error))
            {
                result.push_back(jsonToMetadata(sampleJson));
            }
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        DBG("MetadataWriter: JSON parse error: " + juce::String(e.what()));
    }

    return result;
}

// ── Validation ──────────────────────────────────────────────────────────────────

bool MetadataWriter::validateMetadata(const DatasetMetadata& metadata,
                                      juce::String& outError)
{
    // Check required fields
    if (metadata.sampleId.isEmpty())
    {
        outError = "sampleId is required";
        return false;
    }

    if (metadata.timestamp <= 0)
    {
        outError = "timestamp must be positive";
        return false;
    }

    // Validate source provenance
    if (metadata.source.filePath.isEmpty())
    {
        outError = "source.filePath is required";
        return false;
    }

    if (metadata.source.sampleRate <= 0)
    {
        outError = "source.sampleRate must be positive";
        return false;
    }

    if (metadata.source.numChannels <= 0)
    {
        outError = "source.numChannels must be positive";
        return false;
    }

    // Validate chain details
    if (metadata.chain.sampleRate <= 0)
    {
        outError = "chain.sampleRate must be positive";
        return false;
    }

    if (metadata.chain.blockSize <= 0)
    {
        outError = "chain.blockSize must be positive";
        return false;
    }

    // Validate output characteristics
    if (metadata.output.durationSeconds < 0)
    {
        outError = "output.durationSeconds cannot be negative";
        return false;
    }

    // Validate ML targets
    if (!metadata.targets.styleClassification.isEmpty())
    {
        // Validate style classification is one of known values
        static const juce::StringArray validStyles = {
            "electronic", "rock", "pop", "jazz", "classical",
            "hiphop", "ambient", "cinematic", "experimental", "other"
        };

        if (!validStyles.contains(metadata.targets.styleClassification.toLowerCase()))
        {
            // Allow but warn
            DBG("MetadataWriter: Unknown style classification: " + metadata.targets.styleClassification);
        }
    }

    // Validate split value
    if (!metadata.split.isEmpty())
    {
        static const juce::StringArray validSplits = {"train", "val", "test"};
        if (!validSplits.contains(metadata.split.toLowerCase()))
        {
            outError = "split must be 'train', 'val', or 'test'";
            return false;
        }
    }

    // Validate processing intensity range
    if (metadata.targets.processingIntensity < 0.0f ||
        metadata.targets.processingIntensity > 1.0f)
    {
        outError = "processingIntensity must be between 0 and 1";
        return false;
    }

    return true;
}

bool MetadataWriter::validateAgainstSchema(const nlohmann::json& json,
                                           juce::String& outError)
{
    // Basic structural validation
    if (!json.is_object())
    {
        outError = "Root must be an object";
        return false;
    }

    // Required top-level fields
    static const std::vector<std::string> requiredFields = {
        "sampleId", "timestamp", "source", "chain", "output"
    };

    for (const auto& field : requiredFields)
    {
        if (!json.contains(field))
        {
            outError = "Missing required field: " + juce::String(field);
            return false;
        }
    }

    // Validate types
    if (!json["sampleId"].is_string())
    {
        outError = "sampleId must be a string";
        return false;
    }

    if (!json["timestamp"].is_number())
    {
        outError = "timestamp must be a number";
        return false;
    }

    if (!json["source"].is_object())
    {
        outError = "source must be an object";
        return false;
    }

    if (!json["chain"].is_object())
    {
        outError = "chain must be an object";
        return false;
    }

    if (!json["output"].is_object())
    {
        outError = "output must be an object";
        return false;
    }

    // Validate source fields
    const auto& source = json["source"];
    if (!source.contains("filePath") || !source["filePath"].is_string())
    {
        outError = "source.filePath must be a string";
        return false;
    }

    // Validate chain fields
    const auto& chain = json["chain"];
    if (!chain.contains("plugins") || !chain["plugins"].is_array())
    {
        outError = "chain.plugins must be an array";
        return false;
    }

    return true;
}

// ── Schema Management ───────────────────────────────────────────────────────────

nlohmann::json MetadataWriter::getSchema() const
{
    return schema_;
}

juce::File MetadataWriter::exportSchema(const juce::File& outputFile)
{
    std::ofstream file(outputFile.getFullPathName().toStdString());
    if (file.is_open())
    {
        file << schema_.dump(4);
        return outputFile;
    }
    return {};
}

// ── Parquet Export ──────────────────────────────────────────────────────────────

bool MetadataWriter::exportToParquet(const juce::File& outputFile,
                                     const std::vector<DatasetMetadata>& metadataList)
{
    // Flatten all metadata to CSV format for Parquet conversion
    std::ofstream file(outputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return false;

    // Write header
    file << "sample_id,timestamp,source_file,source_genre,source_lufs,"
         << "chain_type,sample_rate,block_size,"
         << "output_lufs,output_true_peak,output_dynamic_range,output_spectral_centroid,output_duration,"
         << "style_classification,processing_intensity,split\n";

    // Write data rows
    for (const auto& metadata : metadataList)
    {
        file << metadata.sampleId.toStdString() << ","
             << metadata.timestamp << ","
             << "\"" << metadata.source.filePath.toStdString() << "\","
             << "\"" << metadata.source.genre.toStdString() << "\","
             << metadata.source.originalLufs << ","
             << "\"" << metadata.chain.chainType.toStdString() << "\","
             << metadata.chain.sampleRate << ","
             << metadata.chain.blockSize << ","
             << metadata.output.lufs << ","
             << metadata.output.truePeakDb << ","
             << metadata.output.dynamicRangeDb << ","
             << metadata.output.spectralCentroidHz << ","
             << metadata.output.durationSeconds << ","
             << "\"" << metadata.targets.styleClassification.toStdString() << "\","
             << metadata.targets.processingIntensity << ","
             << "\"" << metadata.split.toStdString() << "\"\n";
    }

    return true;
}

bool MetadataWriter::exportFeatureTable(const juce::File& outputFile,
                                        const std::vector<DatasetMetadata>& metadataList)
{
    std::ofstream file(outputFile.getFullPathName().toStdString());
    if (!file.is_open())
        return false;

    if (metadataList.empty())
        return false;

    // Collect all feature keys from first sample
    std::vector<std::string> spectralKeys;
    std::vector<std::string> temporalKeys;
    std::vector<std::string> perceptualKeys;

    if (!metadataList[0].spectralFeatures.is_null())
    {
        for (auto it = metadataList[0].spectralFeatures.begin();
             it != metadataList[0].spectralFeatures.end(); ++it)
        {
            spectralKeys.push_back("spectral_" + it.key());
        }
    }

    if (!metadataList[0].temporalFeatures.is_null())
    {
        for (auto it = metadataList[0].temporalFeatures.begin();
             it != metadataList[0].temporalFeatures.end(); ++it)
        {
            temporalKeys.push_back("temporal_" + it.key());
        }
    }

    if (!metadataList[0].perceptualFeatures.is_null())
    {
        for (auto it = metadataList[0].perceptualFeatures.begin();
             it != metadataList[0].perceptualFeatures.end(); ++it)
        {
            perceptualKeys.push_back("perceptual_" + it.key());
        }
    }

    // Write header
    file << "sample_id";
    for (const auto& key : spectralKeys) file << "," << key;
    for (const auto& key : temporalKeys) file << "," << key;
    for (const auto& key : perceptualKeys) file << "," << key;
    file << ",processing_intensity,style_label,split\n";

    // Write data rows
    for (const auto& metadata : metadataList)
    {
        file << metadata.sampleId.toStdString();

        // Spectral features
        for (const auto& key : spectralKeys)
        {
            std::string featureKey = key.substr(9); // Remove "spectral_" prefix
            float value = 0.0f;
            if (metadata.spectralFeatures.contains(featureKey) &&
                metadata.spectralFeatures[featureKey].is_number())
            {
                value = metadata.spectralFeatures[featureKey].get<float>();
            }
            file << "," << value;
        }

        // Temporal features
        for (const auto& key : temporalKeys)
        {
            std::string featureKey = key.substr(8); // Remove "temporal_" prefix
            float value = 0.0f;
            if (metadata.temporalFeatures.contains(featureKey) &&
                metadata.temporalFeatures[featureKey].is_number())
            {
                value = metadata.temporalFeatures[featureKey].get<float>();
            }
            file << "," << value;
        }

        // Perceptual features
        for (const auto& key : perceptualKeys)
        {
            std::string featureKey = key.substr(11); // Remove "perceptual_" prefix
            float value = 0.0f;
            if (metadata.perceptualFeatures.contains(featureKey) &&
                metadata.perceptualFeatures[featureKey].is_number())
            {
                value = metadata.perceptualFeatures[featureKey].get<float>();
            }
            file << "," << value;
        }

        file << "," << metadata.targets.processingIntensity
             << ",\"" << metadata.targets.styleClassification.toStdString() << "\""
             << ",\"" << metadata.split.toStdString() << "\"\n";
    }

    return true;
}

// ── JSON Conversion ─────────────────────────────────────────────────────────────

nlohmann::json MetadataWriter::metadataToJson(const DatasetMetadata& metadata) const
{
    nlohmann::json json;

    json["sampleId"] = metadata.sampleId.toStdString();
    json["timestamp"] = metadata.timestamp;

    json["source"] = sourceProvenanceToJson(metadata.source);
    json["chain"] = processingChainDetailsToJson(metadata.chain);
    json["output"] = outputCharacteristicsToJson(metadata.output);

    if (!metadata.spectralFeatures.is_null())
        json["spectralFeatures"] = metadata.spectralFeatures;

    if (!metadata.temporalFeatures.is_null())
        json["temporalFeatures"] = metadata.temporalFeatures;

    if (!metadata.perceptualFeatures.is_null())
        json["perceptualFeatures"] = metadata.perceptualFeatures;

    json["targets"] = mlTargetsToJson(metadata.targets);

    if (metadata.split.isNotEmpty())
        json["split"] = metadata.split.toStdString();

    if (!metadata.tags.isEmpty())
    {
        json["tags"] = nlohmann::json::array();
        for (const auto& tag : metadata.tags)
            json["tags"].push_back(tag.toStdString());
    }

    return json;
}

DatasetMetadata MetadataWriter::jsonToMetadata(const nlohmann::json& json) const
{
    DatasetMetadata metadata;

    metadata.sampleId = json.value("sampleId", "");
    metadata.timestamp = json.value("timestamp", int64(0));

    if (json.contains("source"))
        metadata.source = jsonToSourceProvenance(json["source"]);

    if (json.contains("chain"))
        metadata.chain = jsonToProcessingChainDetails(json["chain"]);

    if (json.contains("output"))
        metadata.output = jsonToOutputCharacteristics(json["output"]);

    if (json.contains("spectralFeatures"))
        metadata.spectralFeatures = json["spectralFeatures"];

    if (json.contains("temporalFeatures"))
        metadata.temporalFeatures = json["temporalFeatures"];

    if (json.contains("perceptualFeatures"))
        metadata.perceptualFeatures = json["perceptualFeatures"];

    if (json.contains("targets"))
        metadata.targets = jsonToMLTargets(json["targets"]);

    metadata.split = json.value("split", "");

    if (json.contains("tags") && json["tags"].is_array())
    {
        for (const auto& tag : json["tags"])
        {
            if (tag.is_string())
                metadata.tags.add(juce::String(tag.get<std::string>()));
        }
    }

    return metadata;
}

// ── Utility Functions ──────────────────────────────────────────────────────────

juce::String MetadataWriter::computeFileHash(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    juce::FileInputStream stream(file);
    if (stream.failedToOpen())
        return {};

    juce::SHA256 hasher;

    // Read in chunks for large files
    constexpr int chunkSize = 65536;
    juce::HeapBlock<char> buffer(chunkSize);

    while (!stream.isExhausted())
    {
        auto bytesRead = stream.read(buffer, chunkSize);
        if (bytesRead > 0)
        {
            hasher.update(buffer, bytesRead);
        }
    }

    return hasher.toHexString();
}

juce::String MetadataWriter::computeFileHashMD5(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    juce::FileInputStream stream(file);
    if (stream.failedToOpen())
        return {};

    juce::MD5 hasher(&stream);
    return hasher.toHexString();
}

juce::String MetadataWriter::generateUniqueId()
{
    auto now = juce::Time::getCurrentTime();
    auto random = juce::Random::getSystemRandom().nextInt(999999);
    return juce::String::formatted("meta_%lld_%06d",
                                   now.toMilliseconds(),
                                   random);
}

float MetadataWriter::calculateProcessingIntensity(const std::vector<ParameterValue>& params)
{
    if (params.empty())
        return 0.0f;

    // Calculate intensity based on how far parameters deviate from neutral (0.5)
    float totalDeviation = 0.0f;
    int relevantParams = 0;

    for (const auto& param : params)
    {
        // Skip binary parameters for intensity calculation
        if (param.category == "binary")
            continue;

        // Weight by parameter category
        float weight = 1.0f;
        if (param.category == "decibel")
            weight = 1.5f;  // dB changes are more impactful
        else if (param.category == "frequency")
            weight = 0.8f;  // Frequency changes are less dramatic

        float deviation = std::abs(param.normalizedValue - 0.5f) * 2.0f; // 0-1 range
        totalDeviation += deviation * weight;
        relevantParams++;
    }

    if (relevantParams == 0)
        return 0.0f;

    // Normalize to 0-1 range
    return std::min(1.0f, totalDeviation / static_cast<float>(relevantParams));
}

// ── Private Helper Methods ─────────────────────────────────────────────────────

void MetadataWriter::initializeSchema()
{
    schema_ = {
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"$id", "https://morphsnap.ai/schemas/dataset-metadata.json"},
        {"title", "MorphSnap Dataset Metadata"},
        {"description", "Schema for synthetic audio dataset metadata"},
        {"type", "object"},
        {"required", {"sampleId", "timestamp", "source", "chain", "output"}},
        {"properties", {
            {"sampleId", {
                {"type", "string"},
                {"description", "Unique identifier for the sample"},
                {"pattern", "^sample_[0-9]+_[0-9]+$"}
            }},
            {"timestamp", {
                {"type", "integer"},
                {"description", "Unix timestamp in milliseconds"},
                {"minimum", 0}
            }},
            {"source", {
                {"type", "object"},
                {"required", {"filePath", "sampleRate", "numChannels"}},
                {"properties", {
                    {"filePath", {{"type", "string"}}},
                    {"genre", {{"type", "string"}}},
                    {"contentType", {{"type", "string"}}},
                    {"originalLufs", {{"type", "number"}}},
                    {"dynamicRangeDb", {{"type", "number"}}},
                    {"sampleRate", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                    {"numChannels", {{"type", "integer"}, {"minimum", 1}}},
                    {"numSamples", {{"type", "integer"}, {"minimum", 0}}},
                    {"fileHash", {{"type", "string"}}}
                }}
            }},
            {"chain", {
                {"type", "object"},
                {"required", {"plugins", "sampleRate", "blockSize"}},
                {"properties", {
                    {"chainType", {{"type", "string"}}},
                    {"plugins", {
                        {"type", "array"},
                        {"items", {
                            {"type", "object"},
                            {"required", {"pluginId", "pluginName"}},
                            {"properties", {
                                {"pluginId", {{"type", "string"}}},
                                {"pluginName", {{"type", "string"}}},
                                {"vendor", {{"type", "string"}}},
                                {"version", {{"type", "string"}}},
                                {"format", {{"type", "string"}, {"enum", {"VST3", "AU"}}}},
                                {"parameters", {
                                    {"type", "array"},
                                    {"items", {
                                        {"type", "object"},
                                        {"properties", {
                                            {"name", {{"type", "string"}}},
                                            {"index", {{"type", "integer"}}},
                                            {"normalizedValue", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                                            {"rawValue", {{"type", "number"}}},
                                            {"textValue", {{"type", "string"}}},
                                            {"category", {{"type", "string"}}}
                                        }}
                                    }}
                                }}
                            }}
                        }}
                    }},
                    {"sampleRate", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                    {"blockSize", {{"type", "integer"}, {"minimum", 1}}}
                }}
            }},
            {"output", {
                {"type", "object"},
                {"properties", {
                    {"lufs", {{"type", "number"}}},
                    {"truePeakDb", {{"type", "number"}}},
                    {"dynamicRangeDb", {{"type", "number"}}},
                    {"spectralCentroidHz", {{"type", "number"}, {"minimum", 0}}},
                    {"numSamples", {{"type", "integer"}, {"minimum", 0}}},
                    {"durationSeconds", {{"type", "number"}, {"minimum", 0}}}
                }}
            }},
            {"spectralFeatures", {{"type", "object"}}},
            {"temporalFeatures", {{"type", "object"}}},
            {"perceptualFeatures", {{"type", "object"}}},
            {"targets", {
                {"type", "object"},
                {"properties", {
                    {"parameterRegression", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                    {"styleClassification", {{"type", "string"}}},
                    {"processingIntensity", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"featureVector", {{"type", "array"}, {"items", {{"type", "number"}}}}}
                }}
            }},
            {"split", {
                {"type", "string"},
                {"enum", {"train", "val", "test", ""}}
            }},
            {"tags", {
                {"type", "array"},
                {"items", {{"type", "string"}}}
            }}
        }}
    };
}

nlohmann::json MetadataWriter::sourceProvenanceToJson(const SourceProvenance& source) const
{
    nlohmann::json json;

    json["filePath"] = source.filePath.toStdString();
    if (source.genre.isNotEmpty())
        json["genre"] = source.genre.toStdString();
    if (source.contentType.isNotEmpty())
        json["contentType"] = source.contentType.toStdString();
    json["originalLufs"] = source.originalLufs;
    json["dynamicRangeDb"] = source.dynamicRangeDb;
    json["sampleRate"] = source.sampleRate;
    json["numChannels"] = source.numChannels;
    json["numSamples"] = source.numSamples;
    if (source.fileHash.isNotEmpty())
        json["fileHash"] = source.fileHash.toStdString();

    return json;
}

SourceProvenance MetadataWriter::jsonToSourceProvenance(const nlohmann::json& json) const
{
    SourceProvenance source;

    source.filePath = json.value("filePath", "");
    source.genre = json.value("genre", "");
    source.contentType = json.value("contentType", "");
    source.originalLufs = json.value("originalLufs", 0.0f);
    source.dynamicRangeDb = json.value("dynamicRangeDb", 0.0f);
    source.sampleRate = json.value("sampleRate", 48000.0);
    source.numChannels = json.value("numChannels", 2);
    source.numSamples = json.value("numSamples", int64(0));
    source.fileHash = json.value("fileHash", "");

    return source;
}

nlohmann::json MetadataWriter::pluginDetailsToJson(const PluginDetails& plugin) const
{
    nlohmann::json json;

    json["pluginId"] = plugin.pluginId.toStdString();
    json["pluginName"] = plugin.pluginName.toStdString();
    if (plugin.vendor.isNotEmpty())
        json["vendor"] = plugin.vendor.toStdString();
    if (plugin.version.isNotEmpty())
        json["version"] = plugin.version.toStdString();
    if (plugin.format.isNotEmpty())
        json["format"] = plugin.format.toStdString();

    if (!plugin.parameters.empty())
    {
        json["parameters"] = nlohmann::json::array();
        for (const auto& param : plugin.parameters)
        {
            nlohmann::json paramJson;
            paramJson["name"] = param.name.toStdString();
            paramJson["index"] = param.index;
            paramJson["normalizedValue"] = param.normalizedValue;
            paramJson["rawValue"] = param.rawValue;
            if (param.textValue.isNotEmpty())
                paramJson["textValue"] = param.textValue.toStdString();
            if (param.category.isNotEmpty())
                paramJson["category"] = param.category.toStdString();

            json["parameters"].push_back(paramJson);
        }
    }

    return json;
}

PluginDetails MetadataWriter::jsonToPluginDetails(const nlohmann::json& json) const
{
    PluginDetails plugin;

    plugin.pluginId = json.value("pluginId", "");
    plugin.pluginName = json.value("pluginName", "");
    plugin.vendor = json.value("vendor", "");
    plugin.version = json.value("version", "");
    plugin.format = json.value("format", "");

    if (json.contains("parameters") && json["parameters"].is_array())
    {
        for (const auto& paramJson : json["parameters"])
        {
            ParameterValue param;
            param.name = paramJson.value("name", "");
            param.index = paramJson.value("index", 0);
            param.normalizedValue = paramJson.value("normalizedValue", 0.0f);
            param.rawValue = paramJson.value("rawValue", 0.0f);
            param.textValue = paramJson.value("textValue", "");
            param.category = paramJson.value("category", "");

            plugin.parameters.push_back(param);
        }
    }

    return plugin;
}

nlohmann::json MetadataWriter::processingChainDetailsToJson(const ProcessingChainDetails& chain) const
{
    nlohmann::json json;

    if (chain.chainType.isNotEmpty())
        json["chainType"] = chain.chainType.toStdString();

    json["plugins"] = nlohmann::json::array();
    for (const auto& plugin : chain.plugins)
    {
        json["plugins"].push_back(pluginDetailsToJson(plugin));
    }

    json["sampleRate"] = chain.sampleRate;
    json["blockSize"] = chain.blockSize;

    return json;
}

ProcessingChainDetails MetadataWriter::jsonToProcessingChainDetails(const nlohmann::json& json) const
{
    ProcessingChainDetails chain;

    chain.chainType = json.value("chainType", "");

    if (json.contains("plugins") && json["plugins"].is_array())
    {
        for (const auto& pluginJson : json["plugins"])
        {
            chain.plugins.push_back(jsonToPluginDetails(pluginJson));
        }
    }

    chain.sampleRate = json.value("sampleRate", 48000.0);
    chain.blockSize = json.value("blockSize", 512);

    return chain;
}

nlohmann::json MetadataWriter::outputCharacteristicsToJson(const OutputCharacteristics& output) const
{
    nlohmann::json json;

    json["lufs"] = output.lufs;
    json["truePeakDb"] = output.truePeakDb;
    json["dynamicRangeDb"] = output.dynamicRangeDb;
    json["spectralCentroidHz"] = output.spectralCentroidHz;
    json["numSamples"] = output.numSamples;
    json["durationSeconds"] = output.durationSeconds;

    return json;
}

OutputCharacteristics MetadataWriter::jsonToOutputCharacteristics(const nlohmann::json& json) const
{
    OutputCharacteristics output;

    output.lufs = json.value("lufs", 0.0f);
    output.truePeakDb = json.value("truePeakDb", 0.0f);
    output.dynamicRangeDb = json.value("dynamicRangeDb", 0.0f);
    output.spectralCentroidHz = json.value("spectralCentroidHz", 0.0f);
    output.numSamples = json.value("numSamples", int64(0));
    output.durationSeconds = json.value("durationSeconds", 0.0);

    return output;
}

nlohmann::json MetadataWriter::mlTargetsToJson(const MLTargets& targets) const
{
    nlohmann::json json;

    if (!targets.parameterRegression.empty())
        json["parameterRegression"] = targets.parameterRegression;

    if (targets.styleClassification.isNotEmpty())
        json["styleClassification"] = targets.styleClassification.toStdString();

    json["processingIntensity"] = targets.processingIntensity;

    if (!targets.featureVector.empty())
        json["featureVector"] = targets.featureVector;

    return json;
}

MLTargets MetadataWriter::jsonToMLTargets(const nlohmann::json& json) const
{
    MLTargets targets;

    if (json.contains("parameterRegression") && json["parameterRegression"].is_array())
    {
        for (const auto& val : json["parameterRegression"])
        {
            if (val.is_number())
                targets.parameterRegression.push_back(val.get<float>());
        }
    }

    targets.styleClassification = json.value("styleClassification", "");
    targets.processingIntensity = json.value("processingIntensity", 0.0f);

    if (json.contains("featureVector") && json["featureVector"].is_array())
    {
        for (const auto& val : json["featureVector"])
        {
            if (val.is_number())
                targets.featureVector.push_back(val.get<float>());
        }
    }

    return targets;
}

nlohmann::json MetadataWriter::flattenFeatures(const DatasetMetadata& metadata) const
{
    nlohmann::json flat;

    // Add sample ID
    flat["sample_id"] = metadata.sampleId.toStdString();

    // Flatten spectral features
    if (!metadata.spectralFeatures.is_null())
    {
        for (auto it = metadata.spectralFeatures.begin();
             it != metadata.spectralFeatures.end(); ++it)
        {
            std::string key = "spectral_" + it.key();
            if (it.value().is_number())
                flat[key] = it.value().get<float>();
        }
    }

    // Flatten temporal features
    if (!metadata.temporalFeatures.is_null())
    {
        for (auto it = metadata.temporalFeatures.begin();
             it != metadata.temporalFeatures.end(); ++it)
        {
            std::string key = "temporal_" + it.key();
            if (it.value().is_number())
                flat[key] = it.value().get<float>();
        }
    }

    // Flatten perceptual features
    if (!metadata.perceptualFeatures.is_null())
    {
        for (auto it = metadata.perceptualFeatures.begin();
             it != metadata.perceptualFeatures.end(); ++it)
        {
            std::string key = "perceptual_" + it.key();
            if (it.value().is_number())
                flat[key] = it.value().get<float>();
        }
    }

    // Add targets
    flat["processing_intensity"] = metadata.targets.processingIntensity;
    flat["style_label"] = metadata.targets.styleClassification.toStdString();
    flat["split"] = metadata.split.toStdString();

    return flat;
}

} // namespace morphsnap
