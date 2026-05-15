/*
 * More-Phi — AI/Dataset/DatasetOrganizer.cpp
 * Implementation of dataset organization, splitting, and verification.
 */
#include "DatasetOrganizer.h"
#include <juce_cryptography/juce_cryptography.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <set>

namespace more_phi {

// ── Constants ─────────────────────────────────────────────────────────────────

static const juce::StringArray SPLITS = {"train", "val", "test"};
static const juce::StringArray FEATURE_TYPES = {"spectral", "temporal", "perceptual"};
static const juce::StringArray TARGET_TYPES = {"regression", "classification"};

// ── Constructor ───────────────────────────────────────────────────────────────

DatasetOrganizer::DatasetOrganizer(const juce::File& rootDirectory)
    : rootDirectory_(rootDirectory)
    , random_(juce::Random::getSystemRandom().nextInt())
{
}

// ── Directory Structure Management ────────────────────────────────────────────

bool DatasetOrganizer::initializeStructure()
{
    if (!createDirectoryIfNeeded(rootDirectory_))
        return false;

    // Create audio directories for each split
    for (const auto& split : SPLITS)
    {
        if (!createDirectoryIfNeeded(getAudioDirectory(split)))
            return false;
    }

    // Create metadata directory
    if (!createDirectoryIfNeeded(getMetadataDirectory()))
        return false;

    // Create feature directories
    for (const auto& featureType : FEATURE_TYPES)
    {
        if (!createDirectoryIfNeeded(getFeaturesDirectory(featureType)))
            return false;
    }

    // Create target directories
    for (const auto& targetType : TARGET_TYPES)
    {
        if (!createDirectoryIfNeeded(getTargetsDirectory(targetType)))
            return false;
    }

    // Initialize empty global manifest if it doesn't exist
    auto manifestFile = getMetadataDirectory().getChildFile("manifests.json");
    if (!manifestFile.exists())
    {
        nlohmann::json manifest;
        manifest["samples"] = nlohmann::json::object();
        manifest["splitAssignments"] = nlohmann::json::object();
        manifest["hashIndex"] = nlohmann::json::object();
        manifest["createdAt"] = juce::Time::getCurrentTime().toMilliseconds();
        manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();

        return saveGlobalManifest(manifest);
    }

    return true;
}

bool DatasetOrganizer::structureExists() const
{
    if (!rootDirectory_.exists() || !rootDirectory_.isDirectory())
        return false;

    // Check essential directories
    for (const auto& split : SPLITS)
    {
        auto audioDir = rootDirectory_.getChildFile("audio").getChildFile(split);
        if (!audioDir.exists() || !audioDir.isDirectory())
            return false;
    }

    auto metadataDir = rootDirectory_.getChildFile("metadata");
    if (!metadataDir.exists() || !metadataDir.isDirectory())
        return false;

    return true;
}

juce::File DatasetOrganizer::getAudioDirectory(const juce::String& split, const juce::String& genre)
{
    auto dir = rootDirectory_.getChildFile("audio").getChildFile(split);
    if (genre.isNotEmpty())
        dir = dir.getChildFile(genre);
    return dir;
}

juce::File DatasetOrganizer::getMetadataDirectory()
{
    return rootDirectory_.getChildFile("metadata");
}

juce::File DatasetOrganizer::getFeaturesDirectory(const juce::String& featureType)
{
    auto dir = rootDirectory_.getChildFile("features");
    if (featureType.isNotEmpty())
        dir = dir.getChildFile(featureType);
    return dir;
}

juce::File DatasetOrganizer::getTargetsDirectory(const juce::String& targetType)
{
    auto dir = rootDirectory_.getChildFile("targets");
    if (targetType.isNotEmpty())
        dir = dir.getChildFile(targetType);
    return dir;
}

// ── Sample Management ─────────────────────────────────────────────────────────

bool DatasetOrganizer::addSample(const juce::String& sampleId,
                                  const juce::File& audioFile,
                                  const nlohmann::json& metadata,
                                  const juce::String& split)
{
    if (!audioFile.existsAsFile())
    {
        logError("Audio file does not exist: " + audioFile.getFullPathName());
        return false;
    }

    if (!validateMetadata(metadata))
    {
        logError("Invalid metadata for sample: " + sampleId);
        return false;
    }

    // Determine target split
    juce::String targetSplit = split;
    if (targetSplit.isEmpty())
    {
        targetSplit = "train"; // Default to train
    }

    if (SPLITS.contains(targetSplit))
    {
        // Get genre from metadata for subdirectory organization
        juce::String genre = getGenreFromMetadata(metadata);
        if (genre.isEmpty())
            genre = "unknown";

        auto targetDir = getAudioDirectory(targetSplit, genre);
        if (!createDirectoryIfNeeded(targetDir))
            return false;

        // Copy audio file
        auto destFile = targetDir.getChildFile(sampleId + "." + audioFile.getFileExtension());
        if (!copyAudioFile(audioFile, destFile))
            return false;

        // Save individual metadata file
        auto metadataDir = getMetadataDirectory();
        auto sampleMetadataFile = metadataDir.getChildFile(sampleId + ".json");
        std::ofstream o(sampleMetadataFile.getFullPathName().toStdString());
        o << metadata.dump(4);

        // Update global manifest
        auto manifest = loadGlobalManifest();
        manifest["samples"][sampleId.toStdString()] = {
            {"audioPath", destFile.getRelativePathFrom(rootDirectory_).toStdString()},
            {"metadataPath", sampleMetadataFile.getRelativePathFrom(rootDirectory_).toStdString()},
            {"genre", genre.toStdString()},
            {"intensity", getIntensityFromMetadata(metadata)},
            {"addedAt", juce::Time::getCurrentTime().toMilliseconds()}
        };
        manifest["splitAssignments"][sampleId.toStdString()] = targetSplit.toStdString();

        // Add hash to index for deduplication
        auto hash = computeFileHash(destFile);
        if (hash.isNotEmpty())
        {
            manifest["hashIndex"][hash.toStdString()] = sampleId.toStdString();
        }

        manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();

        return saveGlobalManifest(manifest);
    }
    else
    {
        logError("Invalid split: " + targetSplit);
        return false;
    }
}

bool DatasetOrganizer::moveSample(const juce::String& sampleId, const juce::String& targetSplit)
{
    if (!SPLITS.contains(targetSplit))
    {
        logError("Invalid target split: " + targetSplit);
        return false;
    }

    auto manifest = loadGlobalManifest();

    if (!manifest["samples"].contains(sampleId.toStdString()))
    {
        logError("Sample not found: " + sampleId);
        return false;
    }

    juce::String currentSplit = manifest["splitAssignments"][sampleId.toStdString()].get<std::string>().c_str();
    if (currentSplit == targetSplit)
        return true; // Already in target split

    // Get current sample info
    auto& sampleInfo = manifest["samples"][sampleId.toStdString()];
    juce::String genre = sampleInfo["genre"].get<std::string>().c_str();

    // Move audio file
    auto currentAudioFile = rootDirectory_.getChildFile(sampleInfo["audioPath"].get<std::string>());
    auto targetDir = getAudioDirectory(targetSplit, genre);
    if (!createDirectoryIfNeeded(targetDir))
        return false;

    auto targetFile = targetDir.getChildFile(currentAudioFile.getFileName());
    if (currentAudioFile.existsAsFile())
    {
        if (!currentAudioFile.moveFileTo(targetFile))
        {
            logError("Failed to move audio file for sample: " + sampleId);
            return false;
        }
    }

    // Update manifest
    sampleInfo["audioPath"] = targetFile.getRelativePathFrom(rootDirectory_).toStdString();
    manifest["splitAssignments"][sampleId.toStdString()] = targetSplit.toStdString();
    manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();

    return saveGlobalManifest(manifest);
}

bool DatasetOrganizer::removeSample(const juce::String& sampleId)
{
    auto manifest = loadGlobalManifest();

    if (!manifest["samples"].contains(sampleId.toStdString()))
    {
        logError("Sample not found: " + sampleId);
        return false;
    }

    // Get sample info before removal
    auto& sampleInfo = manifest["samples"][sampleId.toStdString()];
    juce::String audioPath = sampleInfo["audioPath"].get<std::string>().c_str();
    juce::String metadataPath = sampleInfo["metadataPath"].get<std::string>().c_str();

    // Delete audio file
    auto audioFile = rootDirectory_.getChildFile(audioPath);
    if (audioFile.existsAsFile())
        audioFile.deleteFile();

    // Delete metadata file
    auto metadataFile = rootDirectory_.getChildFile(metadataPath);
    if (metadataFile.existsAsFile())
        metadataFile.deleteFile();

    // Remove from hash index
    auto hash = computeFileHash(audioFile);
    if (hash.isNotEmpty() && manifest["hashIndex"].contains(hash.toStdString()))
    {
        manifest["hashIndex"].erase(hash.toStdString());
    }

    // Remove from manifest
    manifest["samples"].erase(sampleId.toStdString());
    manifest["splitAssignments"].erase(sampleId.toStdString());
    manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();

    return saveGlobalManifest(manifest);
}

// ── Splitting ─────────────────────────────────────────────────────────────────

void DatasetOrganizer::performSplit(const SplitConfig& config)
{
    auto manifest = loadGlobalManifest();

    // Collect all unassigned samples
    std::vector<juce::String> unassignedSamples;
    for (auto& [id, info] : manifest["samples"].items())
    {
        if (!manifest["splitAssignments"].contains(id) ||
            manifest["splitAssignments"][id].get<std::string>().empty())
        {
            unassignedSamples.push_back(id);
        }
    }

    if (unassignedSamples.empty())
        return;

    // Stratification: group by genre and intensity
    std::map<juce::String, std::vector<juce::String>> stratifiedGroups;

    if (config.stratifyByGenre && config.stratifyByIntensity)
    {
        for (const auto& sampleId : unassignedSamples)
        {
            auto& info = manifest["samples"][sampleId.toStdString()];
            juce::String genre = info["genre"].get<std::string>().c_str();
            float intensity = info["intensity"].get<float>();
            juce::String intensityBucket = intensity < 0.33f ? "low" : (intensity < 0.67f ? "medium" : "high");
            juce::String key = genre + "_" + intensityBucket;
            stratifiedGroups[key].push_back(sampleId);
        }
    }
    else if (config.stratifyByGenre)
    {
        for (const auto& sampleId : unassignedSamples)
        {
            auto& info = manifest["samples"][sampleId.toStdString()];
            juce::String genre = info["genre"].get<std::string>().c_str();
            stratifiedGroups[genre].push_back(sampleId);
        }
    }
    else
    {
        stratifiedGroups["all"] = unassignedSamples;
    }

    // Initialize RNG with seed
    std::mt19937 rng(config.randomSeed);

    // Split each stratum
    for (auto& [stratum, samples] : stratifiedGroups)
    {
        // Shuffle samples
        std::shuffle(samples.begin(), samples.end(), rng);

        int total = static_cast<int>(samples.size());
        int trainEnd = static_cast<int>(total * config.trainRatio);
        int valEnd = trainEnd + static_cast<int>(total * config.valRatio);

        for (int i = 0; i < total; ++i)
        {
            juce::String split;
            if (i < trainEnd)
                split = "train";
            else if (i < valEnd)
                split = "val";
            else
                split = "test";

            manifest["splitAssignments"][samples[i].toStdString()] = split.toStdString();

            // Move file to appropriate directory
            moveSample(samples[i], split);
        }
    }

    // Reload manifest to pick up any updates made by moveSample() calls
    // (moveSample updates audioPath and saves its own manifest copy)
    manifest = loadGlobalManifest();
    manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();
    saveGlobalManifest(manifest);
    updateManifests();
}

void DatasetOrganizer::assignSplit(const juce::String& sampleId, const juce::String& split)
{
    moveSample(sampleId, split);
}

juce::String DatasetOrganizer::getSplitForSample(const juce::String& sampleId)
{
    auto manifest = loadGlobalManifest();

    if (manifest["splitAssignments"].contains(sampleId.toStdString()))
    {
        return manifest["splitAssignments"][sampleId.toStdString()].get<std::string>().c_str();
    }

    return "";
}

// ── Manifest Management ───────────────────────────────────────────────────────

bool DatasetOrganizer::updateManifests()
{
    auto manifest = loadGlobalManifest();

    // Create per-split manifests
    for (const auto& split : SPLITS)
    {
        nlohmann::json splitManifest;
        splitManifest["split"] = split.toStdString();
        splitManifest["samples"] = nlohmann::json::array();
        splitManifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();

        for (auto& [id, assignedSplit] : manifest["splitAssignments"].items())
        {
            if (assignedSplit.get<std::string>() == split.toStdString())
            {
                nlohmann::json sampleEntry;
                sampleEntry["id"] = id;
                sampleEntry["audioPath"] = manifest["samples"][id]["audioPath"];
                sampleEntry["metadataPath"] = manifest["samples"][id]["metadataPath"];
                sampleEntry["genre"] = manifest["samples"][id]["genre"];
                sampleEntry["intensity"] = manifest["samples"][id]["intensity"];
                splitManifest["samples"].push_back(sampleEntry);
            }
        }

        auto splitManifestFile = getMetadataDirectory().getChildFile(split + "_manifest.json");
        std::ofstream o(splitManifestFile.getFullPathName().toStdString());
        o << splitManifest.dump(4);
    }

    return true;
}

nlohmann::json DatasetOrganizer::loadManifest(const juce::String& split)
{
    auto manifestFile = getMetadataDirectory().getChildFile(split + "_manifest.json");

    if (!manifestFile.existsAsFile())
        return nlohmann::json::object();

    try
    {
        std::ifstream i(manifestFile.getFullPathName().toStdString());
        nlohmann::json j;
        i >> j;
        return j;
    }
    catch (const std::exception& e)
    {
        logError("Failed to load manifest: " + juce::String(e.what()));
        return nlohmann::json::object();
    }
}

bool DatasetOrganizer::saveManifest(const juce::String& split, const nlohmann::json& manifest)
{
    auto manifestFile = getMetadataDirectory().getChildFile(split + "_manifest.json");

    try
    {
        std::ofstream o(manifestFile.getFullPathName().toStdString());
        o << manifest.dump(4);
        return true;
    }
    catch (const std::exception& e)
    {
        logError("Failed to save manifest: " + juce::String(e.what()));
        return false;
    }
}

// ── Integrity Verification ─────────────────────────────────────────────────────

IntegrityReport DatasetOrganizer::verifyIntegrity()
{
    IntegrityReport report;
    report.passed = true;

    auto manifest = loadGlobalManifest();

    // Check each sample
    std::set<juce::String> referencedFiles;

    for (auto& [id, info] : manifest["samples"].items())
    {
        juce::String audioPath = info["audioPath"].get<std::string>().c_str();
        juce::String metadataPath = info["metadataPath"].get<std::string>().c_str();

        auto audioFile = rootDirectory_.getChildFile(audioPath);
        auto metadataFile = rootDirectory_.getChildFile(metadataPath);

        referencedFiles.insert(audioFile.getFullPathName().toStdString());
        referencedFiles.insert(metadataFile.getFullPathName().toStdString());

        // Check audio file exists
        if (!audioFile.existsAsFile())
        {
            report.missingFiles.add(audioFile.getFullPathName());
            report.passed = false;
            continue;
        }

        // Check audio file is valid (non-zero size)
        if (audioFile.getSize() == 0)
        {
            report.corruptedFiles.add(audioFile.getFullPathName());
            report.passed = false;
        }

        // Check metadata file exists
        if (!metadataFile.existsAsFile())
        {
            report.missingFiles.add(metadataFile.getFullPathName());
            report.passed = false;
            continue;
        }

        // Validate metadata content
        try
        {
            std::ifstream i(metadataFile.getFullPathName().toStdString());
            nlohmann::json meta;
            i >> meta;

            if (!validateMetadata(meta))
            {
                report.invalidMetadata.add(metadataFile.getFullPathName());
                report.passed = false;
            }
        }
        catch (const std::exception&)
        {
            report.invalidMetadata.add(metadataFile.getFullPathName());
            report.passed = false;
        }
    }

    // Check for orphaned files (files not in manifest)
    for (const auto& split : SPLITS)
    {
        auto audioDir = getAudioDirectory(split);
        if (audioDir.exists())
        {
            juce::Array<juce::File> audioFiles;
            audioDir.findChildFiles(audioFiles, juce::File::findFiles, true, "*.wav;*.aiff;*.flac");

            for (const auto& file : audioFiles)
            {
                if (referencedFiles.find(file.getFullPathName().toStdString()) == referencedFiles.end())
                {
                    report.orphanedFiles.add(file.getFullPathName());
                    report.passed = false;
                }
            }
        }
    }

    // Generate summary
    juce::StringArray issues;
    if (!report.missingFiles.isEmpty())
        issues.add(juce::String(report.missingFiles.size()) + " missing files");
    if (!report.corruptedFiles.isEmpty())
        issues.add(juce::String(report.corruptedFiles.size()) + " corrupted files");
    if (!report.invalidMetadata.isEmpty())
        issues.add(juce::String(report.invalidMetadata.size()) + " invalid metadata files");
    if (!report.orphanedFiles.isEmpty())
        issues.add(juce::String(report.orphanedFiles.size()) + " orphaned files");

    if (issues.isEmpty())
    {
        report.summary = "Dataset integrity check passed. All files present and valid.";
    }
    else
    {
        report.summary = "Issues found: " + issues.joinIntoString(", ");
    }

    return report;
}

bool DatasetOrganizer::repairIntegrity(const IntegrityReport& report)
{
    bool allRepaired = true;

    // Remove orphaned files
    for (const auto& orphan : report.orphanedFiles)
    {
        juce::File file(orphan);
        if (file.existsAsFile())
        {
            if (!file.deleteFile())
            {
                logError("Failed to delete orphaned file: " + orphan);
                allRepaired = false;
            }
        }
    }

    // Remove entries for missing files from manifest
    auto manifest = loadGlobalManifest();
    std::vector<std::string> toRemove;

    for (auto& [id, info] : manifest["samples"].items())
    {
        juce::String audioPath = info["audioPath"].get<std::string>().c_str();
        auto audioFile = rootDirectory_.getChildFile(audioPath);

        if (report.missingFiles.contains(audioFile.getFullPathName()))
        {
            toRemove.push_back(id);
        }
    }

    for (const auto& id : toRemove)
    {
        manifest["samples"].erase(id);
        manifest["splitAssignments"].erase(id);
    }

    if (!toRemove.empty())
    {
        manifest["updatedAt"] = juce::Time::getCurrentTime().toMilliseconds();
        saveGlobalManifest(manifest);
    }

    return allRepaired;
}

// ── Deduplication ──────────────────────────────────────────────────────────────

std::vector<juce::String> DatasetOrganizer::findDuplicates()
{
    std::vector<juce::String> duplicates;
    auto manifest = loadGlobalManifest();

    std::map<juce::String, std::vector<juce::String>> hashToSamples;

    for (auto& [id, info] : manifest["samples"].items())
    {
        juce::String audioPath = info["audioPath"].get<std::string>().c_str();
        auto audioFile = rootDirectory_.getChildFile(audioPath);

        if (audioFile.existsAsFile())
        {
            auto hash = computeFileHash(audioFile);
            if (hash.isNotEmpty())
            {
                hashToSamples[hash.toStdString()].push_back(id);
            }
        }
    }

    for (auto& [hash, samples] : hashToSamples)
    {
        if (samples.size() > 1)
        {
            // Keep the first one, mark the rest as duplicates
            for (size_t i = 1; i < samples.size(); ++i)
            {
                duplicates.push_back(samples[i]);
            }
        }
    }

    return duplicates;
}

bool DatasetOrganizer::removeDuplicates()
{
    auto duplicates = findDuplicates();

    for (const auto& sampleId : duplicates)
    {
        removeSample(sampleId);
    }

    return true;
}

// ── Statistics ─────────────────────────────────────────────────────────────────

DatasetStats DatasetOrganizer::computeStats()
{
    DatasetStats stats;
    auto manifest = loadGlobalManifest();

    for (auto& [id, info] : manifest["samples"].items())
    {
        stats.totalSamples++;

        juce::String split = manifest["splitAssignments"][id].get<std::string>().c_str();
        if (split == "train")
            stats.trainSamples++;
        else if (split == "val")
            stats.valSamples++;
        else if (split == "test")
            stats.testSamples++;

        stats.samplesPerSplit[split]++;

        juce::String genre = info["genre"].get<std::string>().c_str();
        stats.samplesPerGenre[genre]++;

        // Calculate file size
        juce::String audioPath = info["audioPath"].get<std::string>().c_str();
        auto audioFile = rootDirectory_.getChildFile(audioPath);
        if (audioFile.existsAsFile())
        {
            stats.totalSizeBytes += audioFile.getSize();
        }
    }

    // Count duplicates and corrupted files
    auto duplicates = findDuplicates();
    stats.duplicateFiles = static_cast<int>(duplicates.size());

    auto report = verifyIntegrity();
    stats.corruptedFiles = report.corruptedFiles.size();

    return stats;
}

// ── Incremental Extension ──────────────────────────────────────────────────────

bool DatasetOrganizer::extendFromDirectory(const juce::File& sourceDir,
                                            const SplitConfig& config,
                                            bool checkDuplicates)
{
    if (!sourceDir.exists() || !sourceDir.isDirectory())
    {
        logError("Source directory does not exist: " + sourceDir.getFullPathName());
        return false;
    }

    if (!structureExists())
    {
        if (!initializeStructure())
        {
            logError("Failed to initialize dataset structure");
            return false;
        }
    }

    // Load existing hashes for duplicate detection
    auto manifest = loadGlobalManifest();
    std::set<std::string> existingHashes;

    if (checkDuplicates)
    {
        for (auto& [hash, id] : manifest["hashIndex"].items())
        {
            existingHashes.insert(hash);
        }
    }

    // Find all audio files in source directory
    juce::Array<juce::File> audioFiles;
    sourceDir.findChildFiles(audioFiles, juce::File::findFiles, true, "*.wav;*.aiff;*.flac;*.mp3");

    // Look for corresponding metadata files
    std::vector<std::pair<juce::File, nlohmann::json>> newSamples;

    for (const auto& audioFile : audioFiles)
    {
        // Check for duplicate
        if (checkDuplicates)
        {
            auto hash = computeFileHash(audioFile);
            if (existingHashes.count(hash.toStdString()) > 0)
            {
                continue; // Skip duplicate
            }
        }

        // Look for metadata file (same name, .json extension)
        auto metadataFile = audioFile.withFileExtension("json");
        nlohmann::json metadata;

        if (metadataFile.existsAsFile())
        {
            try
            {
                std::ifstream i(metadataFile.getFullPathName().toStdString());
                i >> metadata;
            }
            catch (const std::exception& e)
            {
                logError("Failed to load metadata for " + audioFile.getFileName() + ": " + e.what());
                continue;
            }
        }
        else
        {
            // Create minimal metadata
            metadata["genre"] = "unknown";
            metadata["intensity"] = 0.5f;
            metadata["sourceFile"] = audioFile.getFileName().toStdString();
        }

        newSamples.emplace_back(audioFile, metadata);
    }

    // Add new samples with auto-generated IDs
    for (const auto& [audioFile, metadata] : newSamples)
    {
        juce::String sampleId = generateSampleId();
        addSample(sampleId, audioFile, metadata);
    }

    // Perform stratified split on new samples
    performSplit(config);

    return true;
}

// ── Export for ML Frameworks ───────────────────────────────────────────────────

bool DatasetOrganizer::exportDatasetIndex(const juce::File& outputFile)
{
    auto manifest = loadGlobalManifest();

    nlohmann::json index;
    index["version"] = "1.0";
    index["generatedAt"] = juce::Time::getCurrentTime().toMilliseconds();
    index["rootDirectory"] = rootDirectory_.getFullPathName().toStdString();
    index["samples"] = nlohmann::json::array();

    for (auto& [id, info] : manifest["samples"].items())
    {
        nlohmann::json entry;
        entry["id"] = id;
        entry["audioPath"] = info["audioPath"];
        entry["metadataPath"] = info["metadataPath"];
        entry["genre"] = info["genre"];
        entry["intensity"] = info["intensity"];
        entry["split"] = manifest["splitAssignments"][id];

        index["samples"].push_back(entry);
    }

    try
    {
        std::ofstream o(outputFile.getFullPathName().toStdString());
        o << index.dump(4);
        return true;
    }
    catch (const std::exception& e)
    {
        logError("Failed to export dataset index: " + juce::String(e.what()));
        return false;
    }
}

bool DatasetOrganizer::exportTrainValTestSplit(const juce::File& outputDir)
{
    if (!createDirectoryIfNeeded(outputDir))
        return false;

    for (const auto& split : SPLITS)
    {
        auto manifest = loadManifest(split);

        nlohmann::json splitData;
        splitData["split"] = split.toStdString();
        splitData["samples"] = nlohmann::json::array();

        if (manifest.contains("samples") && manifest["samples"].is_array())
        {
            for (const auto& sample : manifest["samples"])
            {
                nlohmann::json entry;
                entry["id"] = sample["id"];
                entry["audioPath"] = sample["audioPath"];
                entry["genre"] = sample["genre"];
                entry["intensity"] = sample["intensity"];
                splitData["samples"].push_back(entry);
            }
        }

        auto outputFile = outputDir.getChildFile(split + ".json");
        try
        {
            std::ofstream o(outputFile.getFullPathName().toStdString());
            o << splitData.dump(4);
        }
        catch (const std::exception& e)
        {
            logError("Failed to export " + split + " split: " + juce::String(e.what()));
            return false;
        }
    }

    return true;
}

// ── Utility ────────────────────────────────────────────────────────────────────

juce::String DatasetOrganizer::generateSampleId()
{
    auto timestamp = juce::Time::getCurrentTime().toMilliseconds();
    auto randomValue = random_.nextInt(1000000);
    return juce::String::formatted("sample_%lld_%06d", timestamp, randomValue);
}

// ── Private Helper Methods ─────────────────────────────────────────────────────

bool DatasetOrganizer::createDirectoryIfNeeded(const juce::File& dir)
{
    if (dir.exists() && dir.isDirectory())
        return true;

    auto result = dir.createDirectory();
    if (result.failed())
    {
        logError("Failed to create directory: " + dir.getFullPathName() + " - " + result.getErrorMessage());
        return false;
    }

    return true;
}

juce::String DatasetOrganizer::computeFileHash(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    // Use MD5 hash for file content
    juce::FileInputStream stream(file);
    if (stream.failedToOpen())
        return {};

    juce::MD5 md5(stream);
    return md5.toHexString();
}

bool DatasetOrganizer::copyAudioFile(const juce::File& source, const juce::File& dest)
{
    if (source == dest)
        return true;

    // Use JUCE's copyFileTo which handles large files properly
    return source.copyFileTo(dest);
}

std::vector<juce::String> DatasetOrganizer::listSamplesInSplit(const juce::String& split)
{
    std::vector<juce::String> samples;
    auto manifest = loadGlobalManifest();

    for (auto& [id, assignedSplit] : manifest["splitAssignments"].items())
    {
        if (assignedSplit.get<std::string>() == split.toStdString())
        {
            samples.push_back(id);
        }
    }

    return samples;
}

std::vector<juce::String> DatasetOrganizer::getAllGenres()
{
    std::vector<juce::String> genres;
    auto manifest = loadGlobalManifest();

    for (auto& [id, info] : manifest["samples"].items())
    {
        juce::String genre = info["genre"].get<std::string>().c_str();
        if (std::find(genres.begin(), genres.end(), genre) == genres.end())
        {
            genres.push_back(genre);
        }
    }

    return genres;
}

nlohmann::json DatasetOrganizer::loadGlobalManifest()
{
    auto manifestFile = getMetadataDirectory().getChildFile("manifests.json");

    if (!manifestFile.existsAsFile())
    {
        // Return empty manifest structure
        nlohmann::json manifest;
        manifest["samples"] = nlohmann::json::object();
        manifest["splitAssignments"] = nlohmann::json::object();
        manifest["hashIndex"] = nlohmann::json::object();
        return manifest;
    }

    try
    {
        std::ifstream i(manifestFile.getFullPathName().toStdString());
        nlohmann::json j;
        i >> j;
        return j;
    }
    catch (const std::exception& e)
    {
        logError("Failed to load global manifest: " + juce::String(e.what()));
        nlohmann::json manifest;
        manifest["samples"] = nlohmann::json::object();
        manifest["splitAssignments"] = nlohmann::json::object();
        manifest["hashIndex"] = nlohmann::json::object();
        return manifest;
    }
}

bool DatasetOrganizer::saveGlobalManifest(const nlohmann::json& manifest)
{
    auto manifestFile = getMetadataDirectory().getChildFile("manifests.json");

    try
    {
        std::ofstream o(manifestFile.getFullPathName().toStdString());
        o << manifest.dump(4);
        return true;
    }
    catch (const std::exception& e)
    {
        logError("Failed to save global manifest: " + juce::String(e.what()));
        return false;
    }
}

juce::File DatasetOrganizer::getSampleFile(const juce::String& sampleId)
{
    auto manifest = loadGlobalManifest();

    if (manifest["samples"].contains(sampleId.toStdString()))
    {
        juce::String audioPath = manifest["samples"][sampleId.toStdString()]["audioPath"].get<std::string>().c_str();
        return rootDirectory_.getChildFile(audioPath);
    }

    return {};
}

juce::File DatasetOrganizer::getSampleMetadataFile(const juce::String& sampleId)
{
    auto manifest = loadGlobalManifest();

    if (manifest["samples"].contains(sampleId.toStdString()))
    {
        juce::String metadataPath = manifest["samples"][sampleId.toStdString()]["metadataPath"].get<std::string>().c_str();
        return rootDirectory_.getChildFile(metadataPath);
    }

    return {};
}

bool DatasetOrganizer::validateMetadata(const nlohmann::json& metadata)
{
    // Require at least genre field
    if (!metadata.contains("genre"))
        return false;

    return true;
}

juce::String DatasetOrganizer::getGenreFromMetadata(const nlohmann::json& metadata)
{
    if (metadata.contains("genre") && metadata["genre"].is_string())
    {
        return metadata["genre"].get<std::string>().c_str();
    }
    return "unknown";
}

float DatasetOrganizer::getIntensityFromMetadata(const nlohmann::json& metadata)
{
    if (metadata.contains("intensity") && metadata["intensity"].is_number())
    {
        return metadata["intensity"].get<float>();
    }
    return 0.5f; // Default medium intensity
}

void DatasetOrganizer::logError(const juce::String& message)
{
    // In production, this would log to a proper logging system
    DBG("DatasetOrganizer Error: " + message);
    (void)message; // Suppress warning when DBG is a no-op
}

} // namespace more_phi
