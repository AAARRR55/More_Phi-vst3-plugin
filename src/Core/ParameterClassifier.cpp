/*
 * More-Phi — Core/ParameterClassifier.cpp
 * Parameter classification and Learn Mode implementation.
 */
#include "ParameterClassifier.h"
#include "../Host/IPluginHostManager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>

namespace more_phi {

ParameterClassifier::ParameterClassifier()
{
    metadata_.fill(ParameterMetadata{});
}

void ParameterClassifier::analyzeParameters(const IParameterBridge& host)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    const int count = host.getParameterCount();
    const uint32_t clampedCount = static_cast<uint32_t>(juce::jlimit(0, MAX_PARAMETERS, count));
    parameterCount_.store(clampedCount);

    const auto assignCategory = [](const juce::String& lowerName, ParameterMetadata& meta)
    {
        const juce::String category =
            (lowerName.contains("osc") || lowerName.contains("wave")) ? "Oscillator" :
            (lowerName.contains("filter") || lowerName.contains("cutoff") || lowerName.contains("reson")) ? "Filter" :
            (lowerName.contains("attack") || lowerName.contains("decay") || lowerName.contains("sustain") || lowerName.contains("release")) ? "Envelope" :
            (lowerName.contains("lfo") || lowerName.contains("mod")) ? "Modulation" :
            (lowerName.contains("gain") || lowerName.contains("drive") || lowerName.contains("compress")) ? "Dynamics" :
            (lowerName.contains("delay") || lowerName.contains("reverb") || lowerName.contains("mix") || lowerName.contains("width")) ? "Space" :
            "General";

        std::snprintf(meta.category, sizeof(meta.category), "%s", category.toRawUTF8());
    };
    
    for (uint32_t i = 0; i < clampedCount; ++i)
    {
        auto& meta = metadata_[i];
        
        // Get parameter name
        const int paramIndex = static_cast<int>(i);
        juce::String name = host.getParameterName(paramIndex);
        const juce::String lowerName = name.toLowerCase();
        std::snprintf(meta.name, sizeof(meta.name), "%s", name.toRawUTF8());
        
        // Classify type
        meta.type = classifyParameter(paramIndex, host);
        meta.isSanityProtected = false;
        meta.stepCount = 0;
        
        // Set default exposure (exclude obvious non-sound params)
        meta.isExposed = (meta.type != ParameterType::Unknown);
        
        // Detect discrete parameters
        meta.isInterpolatable = (meta.type != ParameterType::Enumeration &&
                                 meta.type != ParameterType::Binary &&
                                 meta.type != ParameterType::Discrete);
        if (meta.type == ParameterType::Binary)
            meta.stepCount = 2;
        else if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
        {
            // FIX C5: Query real step count from host. Fallback to 1 (not 0) so
            // DiscreteParameterHandler::valueToStep() always has a valid step range.
            const int steps = host.getNumSteps(static_cast<int>(i));
            meta.stepCount = (steps > 0) ? static_cast<uint16_t>(steps) : 1;
        }

        // Sanity protection detection
        if (lowerName.contains("volume") ||
            lowerName.contains("gain") ||
            lowerName.contains("output") ||
            lowerName.contains("bypass") ||
            lowerName.contains("mute") ||
            lowerName.contains("power"))
        {
            meta.isSanityProtected = true;
            meta.isExposed = false; // Hide from AI to prevent accidents
        }

        assignCategory(lowerName, meta);
        
        // Default values
        meta.minValue = 0.0f;
        meta.maxValue = 1.0f;
        meta.defaultValue = host.getParameterNormalized(paramIndex);
    }
    
    updateImportanceScores();
}

ParameterType ParameterClassifier::classifyParameter(int index, const IParameterBridge& host) const
{
    juce::String name = host.getParameterName(index);
    
    // First: name-based heuristics
    ParameterType nameType = detectTypeFromName(name.toRawUTF8());
    
    // M-9 FIX: Override fragile name-based heuristics when host provides clear metadata
    if (nameType != ParameterType::Unknown)
    {
        if (host.isDiscrete(index))
        {
            const int steps = host.getParameterNumSteps(index);
            if (steps == 2)
                return ParameterType::Binary;
            if (steps > 2)
                return ParameterType::Discrete;
        }
        return nameType;
    }
    
    // Second: behavior-based detection
    return detectTypeFromBehavior(index, host);
}

ParameterType ParameterClassifier::detectTypeFromName(const char* name) const
{
    if (name == nullptr)
        return ParameterType::Unknown;
    const juce::String lower = juce::String(name).toLowerCase();
    
    // Binary/On-Off detection
    if (lower.contains("bypass") ||
        lower.contains("mute") ||
        lower.contains("on/off") ||
        lower.contains("enable") ||
        lower.contains("power") ||
        lower.contains("active") ||
        lower.contains("invert"))
    {
        return ParameterType::Binary;
    }
    
    // Frequency detection
    if (lower.contains("freq") ||
        lower.contains("hz") ||
        lower.contains("cutoff") ||
        lower.contains("pitch") ||
        lower.contains("tune") ||
        lower.contains("rate"))  // LFO rate
    {
        return ParameterType::Frequency;
    }
    
    // Decibel detection
    if (lower.contains("db") ||
        lower.contains("decibel") ||
        lower.contains("volume") ||
        lower.contains("gain") ||
        lower.contains("level") ||
        lower.contains("drive"))
    {
        return ParameterType::Decibel;
    }
    
    // Enumeration detection (cannot interpolate)
    if (lower.contains("type") ||
        lower.contains("mode") ||
        lower.contains("shape") ||
        lower.contains("waveform") ||
        lower.contains("slope") ||
        lower.contains("order") ||
        lower.contains("source"))
    {
        return ParameterType::Enumeration;
    }
    
    // Percentage detection
    if (lower.contains("%") ||
        lower.contains("percent") ||
        lower.contains("mix") ||
        lower.contains("blend") ||
        lower.contains("balance") ||
        lower.contains("width"))
    {
        return ParameterType::Percentage;
    }

    return ParameterType::Unknown;
}

ParameterType ParameterClassifier::detectTypeFromBehavior(int index, const IParameterBridge& host) const
{
    if (host.isDiscrete(index))
    {
        const juce::String lower = host.getParameterName(index).toLowerCase();
        if (lower.contains("bypass") ||
            lower.contains("mute") ||
            lower.contains("enable") ||
            lower.contains("on/off"))
        {
            return ParameterType::Binary;
        }
        return ParameterType::Discrete;
    }

    // Fall back to continuous when no explicit hint exists.
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
    return getExposedParameterIndicesNoLock();
}

std::vector<int> ParameterClassifier::getExposedParameterIndicesNoLock() const
{
    std::vector<int> exposed;

    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);

    for (uint32_t i = 0; i < count; ++i)
    {
        if (metadata_[i].isExposed &&
            metadata_[i].importanceScore >= learnConfig_.exposureThreshold)
        {
            exposed.push_back(static_cast<int>(i));
        }
    }

    std::sort(exposed.begin(), exposed.end(),
              [this](int a, int b) {
                  return metadata_[a].importanceScore > metadata_[b].importanceScore;
              });

    if (exposed.size() > learnConfig_.maxExposedParameters)
        exposed.resize(learnConfig_.maxExposedParameters);

    return exposed;
}

std::vector<int> ParameterClassifier::getInterpolatableIndices() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> interpolatable;
    
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
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
    if (paramIndex < 0 || paramIndex >= MAX_PARAMETERS)
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
    if (paramIndex < 0 || paramIndex >= MAX_PARAMETERS)
        return;
    
    // AI adjustments don't increase importance as much as user modifications
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_[paramIndex].lastModified = 
        std::chrono::steady_clock::now().time_since_epoch().count();
}

ParameterMetadata ParameterClassifier::getMetadata(int index) const
{
    static const ParameterMetadata emptyMeta{};
    if (index < 0 || index >= MAX_PARAMETERS)
        return emptyMeta;
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_[index];
}

void ParameterClassifier::setMetadata(int index, const ParameterMetadata& meta)
{
    if (index < 0 || index >= MAX_PARAMETERS)
        return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_[index] = meta;
}

void ParameterClassifier::exposeAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!metadata_[i].isSanityProtected)
            metadata_[i].isExposed = true;
    }
}

void ParameterClassifier::hideAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    for (uint32_t i = 0; i < count; ++i)
    {
        metadata_[i].isExposed = false;
    }
}

void ParameterClassifier::exposeCategory(const char* category)
{
    if (category == nullptr)
        return;
    const juce::String categoryLower = juce::String(category).toLowerCase();

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (juce::String(metadata_[i].category).toLowerCase().contains(categoryLower))
        {
            metadata_[i].isExposed = true;
        }
    }
}

void ParameterClassifier::hideCategory(const char* category)
{
    if (category == nullptr)
        return;
    const juce::String categoryLower = juce::String(category).toLowerCase();

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (juce::String(metadata_[i].category).toLowerCase().contains(categoryLower))
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
    
    const auto exposed = getExposedParameterIndicesNoLock();
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
    auto candidates = getExposedParameterIndicesNoLock();
    
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
    std::lock_guard<std::mutex> lock(mutex_);
    return isDiscreteNoLock(index);
}

bool ParameterClassifier::isDiscreteNoLock(int index) const
{
    if (index < 0 || index >= MAX_PARAMETERS)
        return false;

    const auto& meta = metadata_[index];
    return meta.type == ParameterType::Discrete ||
           meta.type == ParameterType::Binary ||
           meta.type == ParameterType::Enumeration ||
           meta.stepCount > 0;
}

bool ParameterClassifier::isBinary(int index) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= MAX_PARAMETERS)
        return false;
    return metadata_[index].type == ParameterType::Binary;
}

std::vector<bool> ParameterClassifier::getDiscreteMap() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    std::vector<bool> map;
    map.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i)
    {
        map.push_back(isDiscreteNoLock(static_cast<int>(i)));
    }
    
    return map;
}

std::vector<bool> ParameterClassifier::getBinaryMap() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= MAX_PARAMETERS)
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
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);

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
        const auto now = static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto age = static_cast<int64_t>(now - meta.lastModified);
        // Higher score for recent modifications (decay over time)
        const float recencyBonus = 0.2f * std::exp(-static_cast<float>(age) / 3600000000000.0f); // 1 hour half-life
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
    
    stats.totalParameters = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    
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
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
    
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
    
    const uint32_t count = std::min<uint32_t>(parameterCount_.load(), MAX_PARAMETERS);
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
    
    parameterCount_.store(std::min<uint32_t>(count, MAX_PARAMETERS));
    
    const uint8_t* metaData = data + 1 + sizeof(count);
    for (uint32_t i = 0; i < count && i < MAX_PARAMETERS; ++i)
    {
        std::memcpy(&metadata_[i], metaData + i * sizeof(ParameterMetadata), 
                   sizeof(ParameterMetadata));
    }

    // H12 FIX: Zero out stale metadata beyond the deserialized count.
    for (uint32_t i = count; i < MAX_PARAMETERS; ++i)
        metadata_[i] = ParameterMetadata{};
}

} // namespace more_phi
