/*
 * More-Phi — AI/TokenOptimizer.cpp
 * Token optimization and cost management implementation.
 */
#include "TokenOptimizer.h"
#include "../Core/ParameterClassifier.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace more_phi {

TokenOptimizer::TokenOptimizer()
{
    sessionStats_.startTime = std::chrono::steady_clock::now();
    lastBatchTime_ = std::chrono::steady_clock::now();
}

void TokenOptimizer::setCostModel(const CostModel& model)
{
    costModel_ = model;
}

void TokenOptimizer::setTokenBudget(const TokenBudget& budget)
{
    std::lock_guard<std::mutex> lock(budgetMutex_);
    budget_ = budget;
}

TokenBudget TokenOptimizer::getTokenBudget() const
{
    std::lock_guard<std::mutex> lock(budgetMutex_);
    return budget_;
}

void TokenOptimizer::setBatchStrategy(BatchStrategy strategy)
{
    batchStrategy_ = strategy;
}

TokenOptimizer::Estimate TokenOptimizer::estimateRequest(
    uint32_t parameterCount,
    bool includeDescriptions,
    const std::vector<std::string>& contextLines) const
{
    Estimate est;
    
    est.systemTokens = estimateTokensInString(systemPrompt_);
    
    for (const auto& line : contextLines)
    {
        est.contextTokens += estimateTokensInString(line);
    }
    
    est.parameterTokens = estimateTokensForParameters(parameterCount, includeDescriptions);
    est.totalTokens = est.systemTokens + est.contextTokens + est.parameterTokens;
    
    // Assume output is ~20% of input for parameter operations
    uint32_t estimatedOutput = est.totalTokens / 5;
    est.estimatedCostUsd = costModel_.calculateCost(est.totalTokens, estimatedOutput);
    
    // Check budget
    SessionStats stats = getSessionStats();
    const TokenBudget budget = getTokenBudget();
    uint32_t totalTokens = stats.totalTokens() + est.totalTokens;
    float totalCost = stats.totalCostUsd + est.estimatedCostUsd;
    
    est.withinBudget = (totalTokens <= budget.maxTokensPerSession) &&
                       (totalCost <= budget.maxCostPerSessionUsd);
    
    if (totalTokens > budget.maxTokensPerSession * 0.9f)
    {
        est.warnings.push_back("Approaching token budget limit");
    }
    if (totalCost > budget.maxCostPerSessionUsd * 0.9f)
    {
        est.warnings.push_back("Approaching cost budget limit");
    }
    if (est.totalTokens > costModel_.contextWindow * 0.8f)
    {
        est.warnings.push_back("Approaching context window limit");
    }
    
    return est;
}

TokenOptimizer::Estimate TokenOptimizer::estimateSetParameter(int count) const
{
    return estimateRequest(count, false, {});
}

TokenOptimizer::Estimate TokenOptimizer::estimateAnalyzePlugin(int paramCount) const
{
    // Analysis includes descriptions
    return estimateRequest(paramCount, true, {});
}

TokenOptimizer::Estimate TokenOptimizer::estimateMorphRequest() const
{
    // Morph requests are small (just position + mode)
    return estimateRequest(2, false, {});
}

void TokenOptimizer::recordUsage(const TokenUsage& usage)
{
    {
        std::lock_guard<std::mutex> lock(usageMutex_);
        usageHistory_.push_back(usage);
        
        // Keep last 1000 entries
        while (usageHistory_.size() > 1000)
        {
            usageHistory_.pop_front();
        }
    }
    
    updateStats(usage);
}

TokenOptimizer::SessionStats TokenOptimizer::getSessionStats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return sessionStats_;
}

bool TokenOptimizer::isBudgetExceeded() const
{
    SessionStats stats = getSessionStats();
    const TokenBudget budget = getTokenBudget();
    return (stats.totalTokens() >= budget.maxTokensPerSession) ||
           (stats.totalCostUsd >= budget.maxCostPerSessionUsd);
}

float TokenOptimizer::getBudgetRemainingUsd() const
{
    SessionStats stats = getSessionStats();
    const TokenBudget budget = getTokenBudget();
    return std::max(0.0f, budget.maxCostPerSessionUsd - stats.totalCostUsd);
}

uint32_t TokenOptimizer::getTokenBudgetRemaining() const
{
    SessionStats stats = getSessionStats();
    const TokenBudget budget = getTokenBudget();
    return std::max(0u, budget.maxTokensPerSession - stats.totalTokens());
}

TokenOptimizer::OptimizedPayload TokenOptimizer::optimizeParameters(
    const std::vector<int>& availableParams,
    const ParameterClassifier* classifier,
    uint32_t customMaxTokens) const
{
    OptimizedPayload result;
    const TokenBudget budget = getTokenBudget();
    
    uint32_t maxTokens = customMaxTokens > 0 ? customMaxTokens : budget.maxTokensPerRequest;
    
    // Start with prioritized list
    std::vector<int> prioritized = availableParams;
    if (classifier && budget.prioritizeImportantParams)
    {
        prioritized = prioritizeParameters(availableParams, classifier);
    }
    
    // Calculate how many we can include
    uint32_t systemTokens = estimateTokensInString(systemPrompt_);
    uint32_t contextTokens = getContextTokenCount();
    uint32_t tokensPerParam = 8;  // Approximate
    
    uint32_t availableForParams = (maxTokens > systemTokens + contextTokens) ? 
                                   maxTokens - systemTokens - contextTokens : 0;
    uint32_t maxParams = availableForParams / tokensPerParam;
    
    // Select parameters
    if (prioritized.size() <= maxParams)
    {
        result.selectedParameterIndices = prioritized;
        result.optimizationReason = "All parameters fit within budget";
    }
    else
    {
        // Take top N by importance
        result.selectedParameterIndices.assign(
            prioritized.begin(), 
            prioritized.begin() + maxParams
        );
        
        std::ostringstream reason;
        reason << "Limited to " << maxParams << " most important parameters "
               << "(excluded " << (prioritized.size() - maxParams) << ")";
        result.optimizationReason = reason.str();
    }
    
    result.estimatedTokens = systemTokens + contextTokens + 
        static_cast<uint32_t>(result.selectedParameterIndices.size()) * tokensPerParam;
    result.estimatedCost = costModel_.calculateCost(result.estimatedTokens, 
                                                     result.estimatedTokens / 5);
    
    return result;
}

void TokenOptimizer::queueParameterUpdate(int paramIndex, float value)
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    pendingBatch_.push_back({paramIndex, value});
}

void TokenOptimizer::queueBatchUpdate(const std::vector<std::pair<int, float>>& updates)
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    pendingBatch_.insert(pendingBatch_.end(), updates.begin(), updates.end());
}

TokenOptimizer::BatchResult TokenOptimizer::processBatch()
{
    BatchResult result;
    
    auto now = std::chrono::steady_clock::now();
    
    // Check if we should send based on strategy
    bool shouldSend = false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastBatchTime_).count();
    
    {
        std::lock_guard<std::mutex> lock(batchMutex_);
        
        if (pendingBatch_.empty())
        {
            result.shouldSend = false;
            return result;
        }
        
        switch (batchStrategy_)
        {
            case BatchStrategy::Immediate:
                shouldSend = true;
                break;
            case BatchStrategy::Debounce100ms:
                shouldSend = elapsed >= 100;
                break;
            case BatchStrategy::Debounce500ms:
                shouldSend = elapsed >= 500;
                break;
            case BatchStrategy::OnSnapshot:
                shouldSend = false;  // Manual trigger only
                break;
            case BatchStrategy::Manual:
                shouldSend = false;
                break;
        }
        
        // Also send if batch is getting large
        if (pendingBatch_.size() >= 10)
        {
            shouldSend = true;
        }
        
        if (shouldSend)
        {
            result.batchedUpdates = pendingBatch_;
            pendingBatch_.clear();
            lastBatchTime_ = now;
        }
    }
    
    result.shouldSend = shouldSend;
    if (shouldSend)
    {
        result.estimate = estimateSetParameter(
            static_cast<int>(result.batchedUpdates.size())
        );
    }
    
    return result;
}

void TokenOptimizer::flushBatch()
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    pendingBatch_.clear();
}

void TokenOptimizer::setSystemPrompt(const std::string& prompt)
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    systemPrompt_ = prompt;
}

uint32_t TokenOptimizer::getSystemPromptTokenCount() const
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    return estimateTokensInString(systemPrompt_);
}

void TokenOptimizer::addContextLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    contextLines_.push_back(line);
    
    // Keep context manageable
    while (contextLines_.size() > 20)
    {
        contextLines_.erase(contextLines_.begin());
    }
}

void TokenOptimizer::clearContext()
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    contextLines_.clear();
}

uint32_t TokenOptimizer::getContextTokenCount() const
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    uint32_t total = 0;
    for (const auto& line : contextLines_)
    {
        total += estimateTokensInString(line);
    }
    return total;
}

TokenOptimizer::CompressionResult TokenOptimizer::compressParameters(
    const std::vector<float>& values,
    const std::vector<int>& indices) const
{
    CompressionResult result;
    
    // Original estimate
    result.originalTokens = static_cast<uint32_t>(indices.size()) * 8;
    
    // Simple compression: group consecutive indices
    std::ostringstream compressed;
    compressed << std::fixed << std::setprecision(3);

    const auto valueForEntry = [&values](size_t entryPos, int parameterIndex) -> float
    {
        if (entryPos < values.size())
            return values[entryPos];

        if (parameterIndex >= 0 &&
            static_cast<size_t>(parameterIndex) < values.size())
        {
            return values[static_cast<size_t>(parameterIndex)];
        }

        return 0.0f;
    };
    
    int rangeStart = -1;
    int prevIdx = -1;
    
    for (size_t i = 0; i < indices.size(); ++i)
    {
        int idx = indices[i];
        
        if (rangeStart == -1)
        {
            rangeStart = idx;
            prevIdx = idx;
        }
        else if (idx == prevIdx + 1)
        {
            // Continue range
            prevIdx = idx;
        }
        else
        {
            // End previous range, start new one
            if (rangeStart == prevIdx)
            {
                compressed << rangeStart << ":" << valueForEntry(i - 1, rangeStart) << ";";
            }
            else
            {
                compressed << rangeStart << "-" << prevIdx << ":[...];";
            }
            rangeStart = idx;
            prevIdx = idx;
        }
    }
    
    // Final range
    if (rangeStart != -1)
    {
        if (rangeStart == prevIdx)
        {
            compressed << rangeStart << ":" << valueForEntry(indices.size() - 1, rangeStart);
        }
        else
        {
            compressed << rangeStart << "-" << prevIdx << ":[...]";
        }
    }
    
    result.compressedData = compressed.str();
    result.compressedTokens = estimateTokensInString(result.compressedData);
    
    result.compressionRatio = result.originalTokens > 0 ?
        static_cast<float>(result.compressedTokens) / result.originalTokens : 1.0f;
    
    return result;
}

void TokenOptimizer::setRateLimit(uint32_t maxRequestsPerMinute)
{
    rateLimit_.store(std::max(1u, maxRequestsPerMinute));
}

bool TokenOptimizer::canMakeRequest() const
{
    std::lock_guard<std::mutex> lock(rateMutex_);
    cleanupOldTimestampsLocked();
    return requestTimestamps_.size() < rateLimit_.load();
}

bool TokenOptimizer::tryConsumeRequestSlot()
{
    std::lock_guard<std::mutex> lock(rateMutex_);
    cleanupOldTimestampsLocked();
    if (requestTimestamps_.size() >= rateLimit_.load())
        return false;
    requestTimestamps_.push_back(std::chrono::steady_clock::now());
    return true;
}

void TokenOptimizer::recordRequestTimestamp()
{
    std::lock_guard<std::mutex> lock(rateMutex_);
    cleanupOldTimestampsLocked();
    requestTimestamps_.push_back(std::chrono::steady_clock::now());
}

float TokenOptimizer::getTimeUntilNextRequest() const
{
    std::lock_guard<std::mutex> lock(rateMutex_);
    cleanupOldTimestampsLocked();
    
    if (requestTimestamps_.size() < rateLimit_.load())
    {
        return 0.0f;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto oldest = requestTimestamps_.front();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count();
    
    return std::max(0.0f, 60.0f - elapsed);
}

std::string TokenOptimizer::generateUsageReport() const
{
    // FIX C16: Enforce consistent lock order budget -> stats to avoid deadlock.
    const TokenBudget budget = getTokenBudget();
    SessionStats stats = getSessionStats();
    
    std::ostringstream report;
    report << "=== MorePhi AI Usage Report ===\n\n";
    
    report << "Session Duration: ";
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        now - stats.startTime).count();
    report << elapsed << " minutes\n\n";
    
    report << "Total Requests: " << stats.totalRequests << "\n";
    report << "Total Tokens: " << stats.totalTokens() << "\n";
    report << "  - Prompt: " << stats.totalPromptTokens << "\n";
    report << "  - Completion: " << stats.totalCompletionTokens << "\n\n";
    
    report << "Total Cost: " << TokenOptimizerUI::formatCost(stats.totalCostUsd) << "\n";
    report << "Budget Used: " << std::fixed << std::setprecision(1)
           << (stats.totalCostUsd / std::max(0.0001f, budget.maxCostPerSessionUsd) * 100) << "%\n\n";
    
    report << "Average per Request:\n";
    if (stats.totalRequests > 0)
    {
        report << "  - Tokens: " << (stats.totalTokens() / stats.totalRequests) << "\n";
        report << "  - Cost: " << TokenOptimizerUI::formatCost(
            stats.totalCostUsd / stats.totalRequests) << "\n";
    }
    
    return report.str();
}

std::string TokenOptimizer::generateOptimizationSuggestions() const
{
    std::ostringstream suggestions;
    suggestions << "=== Optimization Suggestions ===\n\n";

    // FIX C16: Enforce consistent lock order budget -> stats.
    const TokenBudget budget = getTokenBudget();
    SessionStats stats = getSessionStats();
    
    if (stats.totalCostUsd > budget.maxCostPerSessionUsd * 0.5f)
    {
        suggestions << "WARNING: You've used over 50% of your cost budget.\n";
        suggestions << "Consider:\n";
        suggestions << "  - Reducing the number of exposed parameters\n";
        suggestions << "  - Using a less expensive model (GPT-3.5 vs Claude)\n";
        suggestions << "  - Enabling parameter compression\n\n";
    }
    
    if (batchStrategy_ == BatchStrategy::Immediate)
    {
        suggestions << "TIP: You're using 'Immediate' batching. Consider 'Debounce100ms'\n";
        suggestions << "to batch rapid parameter changes and reduce token usage.\n\n";
    }
    
    suggestions << "Current Settings:\n";
    suggestions << "  - Model: " << costModel_.modelName << "\n";
    suggestions << "  - Batch Strategy: " << static_cast<int>(batchStrategy_) << "\n";
    suggestions << "  - Max Parameters: " << budget.keepLastN_Parameters << "\n";
    
    return suggestions.str();
}

void TokenOptimizer::resetSession()
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    sessionStats_ = SessionStats{};
    sessionStats_.startTime = std::chrono::steady_clock::now();
    
    {
        std::lock_guard<std::mutex> usageLock(usageMutex_);
        usageHistory_.clear();
    }

    {
        std::lock_guard<std::mutex> rateLock(rateMutex_);
        requestTimestamps_.clear();
    }
}

TokenOptimizer::DisplayData TokenOptimizer::getDisplayData() const
{
    DisplayData data;
    // FIX C16: Enforce consistent lock order budget -> stats.
    const TokenBudget budget = getTokenBudget();
    SessionStats stats = getSessionStats();
    
    data.sessionCost = stats.totalCostUsd;
    data.sessionTokens = stats.totalTokens();
    data.budgetRemaining = getTokenBudgetRemaining();
    
    auto now = std::chrono::steady_clock::now();
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
        now - stats.startTime).count();
    data.costPerMinute = minutes > 0 ? stats.totalCostUsd / minutes : 0.0f;
    
    float budgetUsed = stats.totalCostUsd / std::max(0.0001f, budget.maxCostPerSessionUsd);
    if (budgetUsed >= 1.0f)
    {
        data.status = "LIMIT_REACHED";
    }
    else if (budgetUsed >= 0.9f)
    {
        data.status = "WARNING";
    }
    else
    {
        data.status = "OK";
    }
    
    return data;
}

// Private helpers

uint32_t TokenOptimizer::estimateTokensInString(const std::string& text) const
{
    // Rough approximation: ~4 characters per token on average
    return static_cast<uint32_t>(text.length() / 4 + 1);
}

uint32_t TokenOptimizer::estimateTokensForParameters(uint32_t paramCount, bool withDescriptions) const
{
    // ~8 tokens per parameter (name + value + formatting)
    // +12 tokens with descriptions
    uint32_t perParam = withDescriptions ? 20 : 8;
    return paramCount * perParam;
}

std::vector<int> TokenOptimizer::prioritizeParameters(
    const std::vector<int>& params,
    const ParameterClassifier* classifier) const
{
    if (!classifier)
    {
        return params;
    }
    
    // Get importance scores and sort
    std::vector<std::pair<int, float>> scoredParams;
    for (int param : params)
    {
        const auto& meta = classifier->getMetadata(param);
        scoredParams.push_back({param, meta.importanceScore});
    }
    
    std::sort(scoredParams.begin(), scoredParams.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
    
    std::vector<int> result;
    for (const auto& p : scoredParams)
    {
        result.push_back(p.first);
    }
    
    return result;
}

void TokenOptimizer::updateStats(const TokenUsage& usage)
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    
    sessionStats_.totalRequests++;
    sessionStats_.totalPromptTokens += usage.promptTokens;
    sessionStats_.totalCompletionTokens += usage.completionTokens;
    sessionStats_.totalCostUsd += usage.estimatedCostUsd;
}

void TokenOptimizer::cleanupOldTimestampsLocked() const
{
    // Caller must hold rateMutex_; only touches mutable members
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::minutes(1);

    while (!requestTimestamps_.empty() && requestTimestamps_.front() < cutoff)
    {
        requestTimestamps_.pop_front();
    }
}

void TokenOptimizer::cleanupOldTimestamps()
{
    std::lock_guard<std::mutex> lock(rateMutex_);
    cleanupOldTimestampsLocked();
}


//=============================================================================
// TokenOptimizerUI Implementation
//=============================================================================

std::string TokenOptimizerUI::formatCost(float costUsd)
{
    if (costUsd >= 1.0f)
    {
        return "$" + std::to_string(costUsd).substr(0, 4);
    }
    else if (costUsd >= 0.01f)
    {
        int cents = static_cast<int>(costUsd * 100);
        return std::to_string(cents) + "¢";
    }
    else
    {
        return std::to_string(static_cast<int>(costUsd * 1000)) + "m¢";
    }
}

std::string TokenOptimizerUI::formatTokenCount(uint32_t tokens)
{
    if (tokens >= 1'000'000)
    {
        float m = tokens / 1'000'000.0f;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << m << "M";
        return oss.str();
    }
    else if (tokens >= 1'000)
    {
        float k = tokens / 1'000.0f;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << k << "k";
        return oss.str();
    }
    else
    {
        return std::to_string(tokens);
    }
}

std::string TokenOptimizerUI::getBudgetStatus(float used, float total)
{
    float ratio = used / total;
    if (ratio >= 1.0f) return "LIMIT_REACHED";
    if (ratio >= 0.9f) return "CRITICAL";
    if (ratio >= 0.75f) return "WARNING";
    if (ratio >= 0.5f) return "ELEVATED";
    return "OK";
}

std::string TokenOptimizerUI::getOptimizationExplanation(const TokenOptimizer::Estimate& est)
{
    std::ostringstream explanation;
    
    explanation << "Estimated " << est.totalTokens << " tokens\n";
    explanation << "  System: " << est.systemTokens << "\n";
    explanation << "  Context: " << est.contextTokens << "\n";
    explanation << "  Parameters: " << est.parameterTokens << "\n\n";
    explanation << "Cost: " << formatCost(est.estimatedCostUsd);
    
    if (!est.warnings.empty())
    {
        explanation << "\n\nWarnings:\n";
        for (const auto& warning : est.warnings)
        {
            explanation << "  - " << warning << "\n";
        }
    }
    
    return explanation.str();
}

} // namespace more_phi
