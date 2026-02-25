/*
 * MorphSnap — Core/ParameterClassifier.cpp
 * Parameter classification and Learn Mode implementation.
 */
#include "ParameterClassifier.h"
#include "../Host/IPluginHostManager.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <sstream>

namespace morphsnap {

ParameterClassifier::ParameterClassifier()
{
    metadata_.fill(ParameterMetadata{});
}

void ParameterClassifier::analyzeParameters(const IParameterBridge& host)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    const int count = host.getParameterCount();
    parameterCount_.store(static_cast<uint32_t>(count));
    
    for (int i = 0; i < count && i < MAX_PARAMS; ++i)
    {
        auto& meta = metadata_[i];
        
        // Get parameter name
        juce::String name = host.getParameterName(i);
        std::strncpy(meta.name, name.toRawUTF8(), sizeof(meta.name) - 1);
        meta.name[sizeof(meta.name) - 1] = '\0';
        
        // Classify type
        meta.type = classifyParameter(i, host);
        
        // Set default exposure (exclude obvious non-sound params)
        meta.isExposed = (meta.type != ParameterType::Unknown);
        
        // Detect discrete parameters
        meta.isInterpolatable = (meta.type != ParameterType::Enumeration && 
                                  meta.type != ParameterType::Binary);
        
        // Sanity protection detection
        const char* nameLower = meta.name;
        if (std::strstr(nameLower, "volume") || 
            std::strstr(nameLower, "gain") ||
            std::strstr(nameLower, "output") ||
            std::strstr(nameLower, "bypass") ||
            std::strstr(nameLower, "mute") ||
            std::strstr(nameLower, "power"))
        {
            meta.isSanityProtected = true;
            meta.isExposed = false; // Hide from AI to prevent accidents
        }
        
        // Default values
        meta.minValue = 0.0f;
        meta.maxValue = 1.0f;
        meta.defaultValue = host.getParameterNormalized(i);
    }
    
    updateImportanceScores();
}

ParameterType ParameterClassifier::classifyParameter(int index, const IParameterBridge& host) const
{
    juce::String name = host.getParameterName(index);
    
    // First: name-based heuristics
    ParameterType nameType = detectTypeFromName(name.toRawUTF8());
    if (nameType != ParameterType::Unknown)
        return nameType;
    
    // Second: behavior-based detection
    return detectTypeFromBehavior(index, host);
}

ParameterType ParameterClassifier::detectTypeFromName(const char* name) const
{
    const char* lower = name;
    
    // Binary/On-Off detection
    if (std::strstr(lower, "bypass") ||
        std::strstr(lower, "mute") ||
        std::strstr(lower, "on/off") ||
        std::strstr(lower, "enable") ||
        std::strstr(lower, "power") ||
        std::strstr(lower, "active") ||
        std::strstr(lower, "invert"))
    {
        return ParameterType::Binary;
    }
    
    // Frequency detection
    if (std::strstr(lower, "freq") ||
        std::strstr(lower, "hz") ||
        std::strstr(lower, "cutoff") ||
        std::strstr(lower, "pitch") ||
        std::strstr(lower, "tune") ||
        std::strstr(lower, "rate"))  // LFO rate
    {
        return ParameterType::Frequency;
    }
    
    // Decibel detection
    if (std::strstr(lower, "db") ||
        std::strstr(lower, "decibel") ||
        std::strstr(lower, "volume") ||
        std::strstr(lower, "gain") ||
        std::strstr(lower, "level") ||
        std::strstr(lower, "drive"))
    {
        return ParameterType::Decibel;
    }
    
    // Enumeration detection (cannot interpolate)
    if (std::strstr(lower, "type") ||
        std::strstr(lower, "mode") ||
        std::strstr(lower, "shape") ||
        std::strstr(lower, "waveform") ||
        std::strstr(lower, "slope") ||
        std::strstr(lower, "order") ||
        std::strstr(lower, "source"))
    {
        return ParameterType::Enumeration;
    }
    
    // Percentage detection
    if (std::strstr(lower, "%") ||
        std::strstr(lower, "percent") ||
        std::strstr(lower, "mix") ||
        std::strstr(lower, "blend") ||
        std::strstr(lower, "balance") ||
        std::strstr(lower, "width"))
    {
        return ParameterType::Percentage;
    }
    
    // Category assignment
    // (This would set meta.category but we're returning type)
    
    return ParameterType::Unknown;
}

ParameterType ParameterClassifier::detectTypeFromBehavior(int index, const IParameterBridge& host) const
{
    // Sample parameter at multiple points to detect steppiness
    float samples[11];
    for (int i = 0; i <= 10; ++i)
    {
        float testValue = i / 10.0f;
        // Note: This is a const method, we can't actually set parameters here
        // In practice, this would need to be done during analysis with temporary values
        samples[i] = testValue;
    }
    
    // For now, assume continuous if no other indicators
    return ParameterType::Continuous;
}

void ParameterClassifier::setLearnConfiguration(const LearnConfiguration& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    learnConfig_ = config;
    updateImportanceScores();
}

LearnConfiguration ParameterClassifier::getLearnConfiguration() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return learnConfig_;
}

std::vector<int> ParameterClassifier::getExposedParameterIndices() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> exposed;
    
    const uint32_t count = parameterCount_.load();
    
    // Collect all exposed parameters
    for (uint32_t i = 0; i < count; ++i)
    {
        if (metadata_[i].isExposed && metadata_[i].importanceScore >= learnConfig_.exposureThreshold)
        {
            exposed.push_back(static_cast<int>(i));
        }
    }
    
    // Sort by importance score (highest first)
    std::sort(exposed.begin(), exposed.end(),
        [this](int a, int b) {
            return metadata_[a].importanceScore > metadata_[b].importanceScore;
        });
    
    // Limit to maxExposedParameters
    if (exposed.size() > learnConfig_.maxExposedParameters)
    {
        exposed.resize(learnConfig_.maxExposedParameters);
    }
    
    return exposed;
}

std::vector<int> ParameterClassifier::getInterpolatableIndices() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> interpolatable;
    
    const uint32_t count = parameterCount_.load();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (metadata_[i].isInterpolatable)
        {
            interpolatable.push_back(static_cast<int>(i));
        }
    }
    
    return interpolatable;
}

void ParameterClassifier::recordModification(int paramIndex)
{
    if (paramIndex < 0 || paramIndex >= MAX_PARAMS)
        return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto& meta = metadata_[paramIndex];
    
    meta.modificationCount++;
    meta.lastModified = std::chrono::steady_clock::now().time_since_epoch().count();
    
    // Increase importance on user modification
    meta.importanceScore = std::min(1.0f, meta.importanceScore + 0.1f);
}

void ParameterClassifier::recordAIAdjustment(int paramIndex)
{
    if (paramIndex < 0 || paramIndex >= MAX_PARAMS)
        return;
    
    // AI adjustments don't increase importance as much as user modifications
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_[paramIndex].lastModified = 
        std::chrono::steady_clock::now().time_since_epoch().count();
}

const ParameterMetadata& ParameterClassifier::getMetadata(int index) const
{
    static const ParameterMetadata emptyMeta{};
    if (index < 0 || index >= MAX_PARAMS)
        return emptyMeta;
    return metadata_[index];
}

void ParameterClassifier::setMetadata(int index, const ParameterMetadata& meta)
{
    if (index < 0 || index >= MAX_PARAMS)
        return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_[index] = meta;
}

void ParameterClassifier::exposeAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!metadata_[i].isSanityProtected)
            metadata_[i].isExposed = true;
    }
}

void ParameterClassifier::hideAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    for (uint32_t i = 0; i < count; ++i)
    {
        metadata_[i].isExposed = false;
    }
}

void ParameterClassifier::exposeCategory(const char* category)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (std::strstr(metadata_[i].category, category))
        {
            metadata_[i].isExposed = true;
        }
    }
}

void ParameterClassifier::hideCategory(const char* category)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (std::strstr(metadata_[i].category, category))
        {
            metadata_[i].isExposed = false;
        }
    }
}

TokenEstimate ParameterClassifier::estimateTokens(bool includeDescriptions) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TokenEstimate estimate;
    
    estimate.systemPromptTokens = BASE_SYSTEM_TOKENS;
    
    const auto exposed = getExposedParameterIndices();
    estimate.parameterTokens = static_cast<uint32_t>(exposed.size()) * TOKENS_PER_PARAM;
    
    if (includeDescriptions)
    {
        estimate.parameterTokens += static_cast<uint32_t>(exposed.size()) * TOKENS_PER_DESC;
    }
    
    estimate.recalculate();
    return estimate;
}

std::vector<int> ParameterClassifier::optimizeForTokenBudget(uint32_t maxTokens) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> selected;
    
    uint32_t currentTokens = BASE_SYSTEM_TOKENS;
    
    // Get exposed indices sorted by importance
    auto candidates = getExposedParameterIndices();
    
    for (int idx : candidates)
    {
        uint32_t paramCost = TOKENS_PER_PARAM + 
            (std::strlen(metadata_[idx].description) > 0 ? TOKENS_PER_DESC : 0);
        
        if (currentTokens + paramCost <= maxTokens)
        {
            selected.push_back(idx);
            currentTokens += paramCost;
        }
        else
        {
            break;
        }
    }
    
    return selected;
}

bool ParameterClassifier::isDiscrete(int index) const
{
    if (index < 0 || index >= MAX_PARAMS)
        return false;
    
    const auto& meta = metadata_[index];
    return meta.type == ParameterType::Discrete || 
           meta.type == ParameterType::Binary ||
           meta.type == ParameterType::Enumeration ||
           meta.stepCount > 0;
}

bool ParameterClassifier::isBinary(int index) const
{
    if (index < 0 || index >= MAX_PARAMS)
        return false;
    return metadata_[index].type == ParameterType::Binary;
}

std::vector<bool> ParameterClassifier::getDiscreteMap() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    std::vector<bool> map;
    map.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i)
    {
        map.push_back(isDiscrete(static_cast<int>(i)));
    }
    
    return map;
}

std::vector<bool> ParameterClassifier::getBinaryMap() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    std::vector<bool> map;
    map.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i)
    {
        map.push_back(metadata_[i].type == ParameterType::Binary);
    }
    
    return map;
}

bool ParameterClassifier::shouldSkipForMorph(int index) const
{
    if (index < 0 || index >= MAX_PARAMS)
        return true;
    
    const auto& meta = metadata_[index];
    
    // Skip non-interpolatable parameters during morph
    if (!meta.isInterpolatable)
        return true;
    
    // Skip sanity-protected parameters during morph
    if (meta.isSanityProtected)
        return true;
    
    // Skip binary switches
    if (meta.type == ParameterType::Binary)
        return true;
    
    // Skip enumerations
    if (meta.type == ParameterType::Enumeration)
        return true;
    
    return false;
}

void ParameterClassifier::updateImportanceScores()
{
    const uint32_t count = parameterCount_.load();
    
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    
    for (uint32_t i = 0; i < count; ++i)
    {
        auto& meta = metadata_[i];
        meta.importanceScore = calculateImportance(meta);
    }
}

float ParameterClassifier::calculateImportance(const ParameterMetadata& meta) const
{
    float score = 0.5f; // Base score
    
    // Modification frequency weight
    score += std::min(0.3f, meta.modificationCount * 0.05f);
    
    // Recency weight
    if (learnConfig_.prioritizeRecent && meta.lastModified > 0)
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto age = now - meta.lastModified;
        // Higher score for recent modifications (decay over time)
        float recencyBonus = 0.2f * std::exp(-age / 3600000000000.0f); // 1 hour half-life
        score += recencyBonus;
    }
    
    // Type-based adjustments
    switch (meta.type)
    {
        case ParameterType::Continuous:
        case ParameterType::Frequency:
        case ParameterType::Decibel:
            score += 0.1f; // Prefer continuous parameters
            break;
        case ParameterType::Binary:
            score -= 0.1f; // Deprioritize simple on/off
            break;
        case ParameterType::Enumeration:
            score -= 0.05f; // Slightly deprioritize enums
            break;
        default:
            break;
    }
    
    return std::clamp(score, 0.0f, 1.0f);
}

ParameterClassifier::Statistics ParameterClassifier::getStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statistics stats{};
    
    stats.totalParameters = parameterCount_.load();
    
    float totalImportance = 0.0f;
    for (uint32_t i = 0; i < stats.totalParameters; ++i)
    {
        const auto& meta = metadata_[i];
        
        if (meta.isExposed)
            stats.exposedParameters++;
        if (meta.isSanityProtected)
            stats.sanityProtectedCount++;
        
        switch (meta.type)
        {
            case ParameterType::Continuous:
            case ParameterType::Frequency:
            case ParameterType::Decibel:
            case ParameterType::Percentage:
                stats.continuousCount++;
                break;
            case ParameterType::Discrete:
                stats.discreteCount++;
                break;
            case ParameterType::Binary:
                stats.binaryCount++;
                break;
            default:
                break;
        }
        
        totalImportance += meta.importanceScore;
    }
    
    if (stats.totalParameters > 0)
    {
        stats.averageImportance = totalImportance / stats.totalParameters;
    }
    
    return stats;
}

void ParameterClassifier::clearLearningData()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = parameterCount_.load();
    
    for (uint32_t i = 0; i < count; ++i)
    {
        metadata_[i].modificationCount = 0;
        metadata_[i].lastModified = 0;
        metadata_[i].importanceScore = 0.5f;
    }
}

void ParameterClassifier::serialize(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Simple binary format: version (1) + count + metadata array
    out.clear();
    out.push_back(1); // Version
    
    const uint32_t count = parameterCount_.load();
    out.insert(out.end(), 
        reinterpret_cast<const uint8_t*>(&count), 
        reinterpret_cast<const uint8_t*>(&count) + sizeof(count));
    
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& meta = metadata_[i];
        out.insert(out.end(), 
            reinterpret_cast<const uint8_t*>(&meta),
            reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
    }
}

void ParameterClassifier::deserialize(const uint8_t* data, size_t size)
{
    if (size < 5) // version + count
        return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint8_t version = data[0];
    if (version != 1)
        return; // Unsupported version
    
    uint32_t count;
    std::memcpy(&count, data + 1, sizeof(count));
    
    size_t expectedSize = 1 + sizeof(count) + count * sizeof(ParameterMetadata);
    if (size < expectedSize)
        return;
    
    parameterCount_.store(count);
    
    const uint8_t* metaData = data + 1 + sizeof(count);
    for (uint32_t i = 0; i < count && i < MAX_PARAMS; ++i)
    {
        std::memcpy(&metadata_[i], metaData + i * sizeof(ParameterMetadata), 
                   sizeof(ParameterMetadata));
    }
}

} // namespace morphsnap
