/*
 * MorphSnap — AI/TokenOptimizer.h
 * Token cost estimation, budgeting, and optimization for MCP/AI integration.
 * Tracks usage and provides strategies for cost-effective AI interactions.
 */
#pragma once

#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <chrono>
#include <mutex>

namespace morphsnap {

// Token and cost tracking
struct TokenUsage
{
    uint32_t promptTokens = 0;
    uint32_t completionTokens = 0;
    uint32_t totalTokens() const { return promptTokens + completionTokens; }
    
    float estimatedCostUsd = 0.0f;  // Based on model pricing
    std::chrono::steady_clock::time_point timestamp;
    std::string operation;          // "set_parameter", "analyze", etc.
};

// Cost model configuration for different LLMs
struct CostModel
{
    std::string modelName;
    float inputCostPer1M;    // USD per 1M input tokens
    float outputCostPer1M;   // USD per 1M output tokens
    uint32_t contextWindow;  // Maximum context size
    
    float calculateCost(uint32_t inputTokens, uint32_t outputTokens) const
    {
        return (inputTokens / 1'000'000.0f) * inputCostPer1M +
               (outputTokens / 1'000'000.0f) * outputCostPer1M;
    }
};

// Predefined cost models
namespace CostModels {
    inline CostModel Claude35Sonnet() {
        return { "claude-3-5-sonnet", 3.0f, 15.0f, 200'000 };
    }
    inline CostModel GPT4Turbo() {
        return { "gpt-4-turbo", 10.0f, 30.0f, 128'000 };
    }
    inline CostModel GPT35Turbo() {
        return { "gpt-3.5-turbo", 0.5f, 1.5f, 16'384 };
    }
    inline CostModel LocalLLM() {
        return { "local", 0.0f, 0.0f, 32'000 };  // No API cost
    }
}

// Token budget configuration
struct TokenBudget
{
    uint32_t maxTokensPerRequest = 4000;      // Target max per request
    uint32_t maxTokensPerSession = 100'000;   // Session limit
    float maxCostPerSessionUsd = 5.0f;        // Cost limit
    bool enableCompression = true;            // Use parameter compression
    bool prioritizeImportantParams = true;    // Use importance scores
    uint32_t keepLastN_Parameters = 50;       // Only send last N changed
};

// Batching strategies for parameter updates
enum class BatchStrategy
{
    Immediate,      // Send immediately (highest cost, lowest latency)
    Debounce100ms,  // Batch for 100ms
    Debounce500ms,  // Batch for 500ms
    OnSnapshot,     // Only send on snapshot recall/capture
    Manual          // User triggers send
};

class TokenOptimizer
{
public:
    TokenOptimizer();
    
    // Configuration
    void setCostModel(const CostModel& model);
    void setTokenBudget(const TokenBudget& budget);
    void setBatchStrategy(BatchStrategy strategy);
    
    // Token estimation (before sending)
    struct Estimate
    {
        uint32_t systemTokens = 0;
        uint32_t contextTokens = 0;
        uint32_t parameterTokens = 0;
        uint32_t totalTokens = 0;
        float estimatedCostUsd = 0.0f;
        bool withinBudget = true;
        std::vector<std::string> warnings;
    };
    
    Estimate estimateRequest(
        uint32_t parameterCount,
        bool includeDescriptions = true,
        const std::vector<std::string>& contextLines = {}) const;
    
    // Estimate for specific operations
    Estimate estimateSetParameter(int count) const;
    Estimate estimateAnalyzePlugin(int paramCount) const;
    Estimate estimateMorphRequest() const;
    
    // Session tracking
    void recordUsage(const TokenUsage& usage);
    
    struct SessionStats
    {
        uint32_t totalRequests = 0;
        uint32_t totalPromptTokens = 0;
        uint32_t totalCompletionTokens = 0;
        float totalCostUsd = 0.0f;
        std::chrono::steady_clock::time_point startTime;
        
        uint32_t totalTokens() const { 
            return totalPromptTokens + totalCompletionTokens; 
        }
    };
    SessionStats getSessionStats() const;
    
    // Budget monitoring
    bool isBudgetExceeded() const;
    float getBudgetRemainingUsd() const;
    uint32_t getTokenBudgetRemaining() const;
    
    // Parameter optimization
    struct OptimizedPayload
    {
        std::vector<int> selectedParameterIndices;
        uint32_t estimatedTokens;
        float estimatedCost;
        std::string optimizationReason;  // Why these were selected
    };
    
    OptimizedPayload optimizeParameters(
        const std::vector<int>& availableParams,
        const class ParameterClassifier* classifier = nullptr,
        uint32_t customMaxTokens = 0) const;
    
    // Smart batching
    void queueParameterUpdate(int paramIndex, float value);
    void queueBatchUpdate(const std::vector<std::pair<int, float>>& updates);
    
    struct BatchResult
    {
        bool shouldSend;
        std::vector<std::pair<int, float>> batchedUpdates;
        Estimate estimate;
    };
    BatchResult processBatch();  // Call periodically (e.g., every 50ms)
    void flushBatch();           // Force immediate send
    
    // Context window management
    void setSystemPrompt(const std::string& prompt);
    uint32_t getSystemPromptTokenCount() const;
    
    void addContextLine(const std::string& line);
    void clearContext();
    uint32_t getContextTokenCount() const;
    
    // Compression strategies
    struct CompressionResult
    {
        std::string compressedData;
        uint32_t originalTokens;
        uint32_t compressedTokens;
        float compressionRatio;
    };
    
    CompressionResult compressParameters(
        const std::vector<float>& values,
        const std::vector<int>& indices) const;
    
    // Rate limiting
    void setRateLimit(uint32_t maxRequestsPerMinute);
    bool canMakeRequest() const;
    float getTimeUntilNextRequest() const;  // seconds
    
    // Reporting
    std::string generateUsageReport() const;
    std::string generateOptimizationSuggestions() const;
    
    // Reset
    void resetSession();
    
    // Real-time display data (for UI)
    struct DisplayData
    {
        float sessionCost;
        uint32_t sessionTokens;
        uint32_t budgetRemaining;
        float costPerMinute;
        std::string status;  // "OK", "WARNING", "LIMIT_REACHED"
    };
    DisplayData getDisplayData() const;

private:
    CostModel costModel_ = CostModels::Claude35Sonnet();
    TokenBudget budget_;
    BatchStrategy batchStrategy_ = BatchStrategy::Debounce100ms;
    
    std::deque<TokenUsage> usageHistory_;
    mutable std::mutex usageMutex_;
    
    std::vector<std::pair<int, float>> pendingBatch_;
    mutable std::mutex batchMutex_;
    std::chrono::steady_clock::time_point lastBatchTime_;
    
    std::string systemPrompt_;
    std::vector<std::string> contextLines_;
    mutable std::mutex contextMutex_;
    
    std::atomic<uint32_t> rateLimit_{60};  // Requests per minute
    mutable std::deque<std::chrono::steady_clock::time_point> requestTimestamps_;
    mutable std::mutex rateMutex_;
    
    SessionStats sessionStats_;
    mutable std::mutex statsMutex_;
    
    // Token counting (approximate)
    uint32_t estimateTokensInString(const std::string& text) const;
    uint32_t estimateTokensForParameters(uint32_t paramCount, bool withDescriptions) const;
    
    // Optimization helpers
    std::vector<int> prioritizeParameters(
        const std::vector<int>& params,
        const ParameterClassifier* classifier) const;
    
    void updateStats(const TokenUsage& usage);
    void cleanupOldTimestamps() const;
};

// UI integration helper
class TokenOptimizerUI
{
public:
    // Format cost for display (e.g., "$0.023" or "2.3¢")
    static std::string formatCost(float costUsd);
    
    // Format token count (e.g., "1,234" or "1.2k")
    static std::string formatTokenCount(uint32_t tokens);
    
    // Get status color/status for UI
    static std::string getBudgetStatus(float used, float total);
    
    // Generate tooltip/help text
    static std::string getOptimizationExplanation(const TokenOptimizer::Estimate& est);
};

} // namespace morphsnap
