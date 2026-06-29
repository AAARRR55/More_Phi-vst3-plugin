/*
 * More-Phi — Core/ParameterClassifier.h
 * Parameter classification system for Learn Mode.
 * Classifies parameters as continuous, discrete, or binary for AI exposure control.
 */
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

#include "ParameterState.h"   // MAX_PARAMETERS

namespace more_phi {

// Parameter type classification
enum class ParameterType : uint8_t
{
    Unknown = 0,
    Continuous,      // 0.0-1.0 range with meaningful intermediate values
    Discrete,        // Integer-like steps (e.g., waveform selector)
    Binary,          // On/off only (e.g., bypass, mute)
    Frequency,       // Hz-based (logarithmic perception)
    Decibel,         // dB-based (logarithmic perception)
    Percentage,      // 0-100% (linear)
    Enumeration,     // Named options (cannot interpolate)
    
    Count
};

// Parameter metadata for AI understanding
struct ParameterMetadata
{
    ParameterType type = ParameterType::Unknown;
    bool isExposed = true;           // Shown to AI (Learn Mode)
    bool isInterpolatable = true;    // Can be morphed
    bool isSanityProtected = false;  // Volume/bypass protection
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.5f;
    uint16_t stepCount = 0;          // 0 = continuous, >0 = discrete steps
    char name[64] = {};
    char description[128] = {};      // Human-readable description
    char category[32] = {};          // "Oscillator", "Filter", "Envelope", etc.
    
    // AI-specific metadata
    float importanceScore = 0.5f;    // 0-1, learned from user behavior
    uint32_t modificationCount = 0;  // How often user changes this
    uint64_t lastModified = 0;       // Timestamp for recency weighting
};

// Learn Mode configuration
struct LearnConfiguration
{
    bool enabled = false;
    float exposureThreshold = 0.3f;  // importanceScore > this to expose
    bool autoLearn = true;           // Track user modifications
    bool prioritizeRecent = true;    // Weight recent changes higher
    uint32_t maxExposedParameters = 50;  // Limit for token management
};

// Token estimation for cost management
struct TokenEstimate
{
    uint32_t systemPromptTokens = 0;
    uint32_t parameterTokens = 0;
    uint32_t contextTokens = 0;
    uint32_t totalTokens = 0;
    float estimatedCostUsd = 0.0f;
    
    void addParameter(uint32_t count) { 
        parameterTokens += count * 8;  // ~8 tokens per param
        recalculate();
    }
    
    void recalculate() {
        totalTokens = systemPromptTokens + parameterTokens + contextTokens;
        // Claude 3.5 Sonnet: $3/M input, $15/M output
        estimatedCostUsd = (totalTokens / 1'000'000.0f) * 3.0f;
    }
};

class ParameterClassifier
{
public:
    ParameterClassifier();
    
    // Core classification
    void analyzeParameters(const class IParameterBridge& host);
    ParameterType classifyParameter(int index, const class IParameterBridge& host) const;
    
    // Learn Mode
    void setLearnConfiguration(const LearnConfiguration& config);
    LearnConfiguration getLearnConfiguration() const;
    
    // Parameter exposure for AI
    std::vector<int> getExposedParameterIndices() const;
    std::vector<int> getInterpolatableIndices() const;
    
    // User behavior tracking
    void recordModification(int paramIndex);
    void recordAIAdjustment(int paramIndex);
    
    // Metadata access
    ParameterMetadata getMetadata(int index) const;
    void setMetadata(int index, const ParameterMetadata& meta);
    
    // Batch operations
    void exposeAll();
    void hideAll();
    void exposeCategory(const char* category);
    void hideCategory(const char* category);
    
    // Token management
    TokenEstimate estimateTokens(bool includeDescriptions = true) const;
    std::vector<int> optimizeForTokenBudget(uint32_t maxTokens) const;
    
    // Discrete parameter handling
    bool isDiscrete(int index) const;
    bool isBinary(int index) const;
    std::vector<bool> getDiscreteMap() const;
    std::vector<bool> getBinaryMap() const;
    
    // Morphing safety
    bool shouldSkipForMorph(int index) const;
    
    // Serialization
    void serialize(std::vector<uint8_t>& out) const;
    void deserialize(const uint8_t* data, size_t size);
    
    // Statistics
    struct Statistics
    {
        uint32_t totalParameters = 0;
        uint32_t exposedParameters = 0;
        uint32_t continuousCount = 0;
        uint32_t discreteCount = 0;
        uint32_t binaryCount = 0;
        uint32_t sanityProtectedCount = 0;
        float averageImportance = 0.0f;
    };
    Statistics getStatistics() const;
    
    // Reset learning
    void clearLearningData();
    
private:
    std::array<ParameterMetadata, MAX_PARAMETERS> metadata_;
    std::atomic<uint32_t> parameterCount_{0};
    LearnConfiguration learnConfig_;
    mutable std::mutex mutex_;
    
    // Analysis helpers
    ParameterType detectTypeFromName(const char* name) const;
    ParameterType detectTypeFromBehavior(int index, const class IParameterBridge& host) const;
    std::vector<int> getExposedParameterIndicesNoLock() const;
    bool isDiscreteNoLock(int index) const;
    void updateImportanceScores();
    float calculateImportance(const ParameterMetadata& meta) const;
    
    // Token constants
    static constexpr uint32_t TOKENS_PER_PARAM = 8;
    static constexpr uint32_t TOKENS_PER_DESC = 12;
    static constexpr uint32_t BASE_SYSTEM_TOKENS = 200;
};

} // namespace more_phi
