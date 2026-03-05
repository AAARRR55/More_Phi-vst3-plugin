/*
 * MorphSnap — AI/Dataset/ParameterSampler.cpp
 * Implementation of advanced parameter sampling for synthetic audio dataset generation.
 */
#include "ParameterSampler.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

namespace morphsnap {

// ============================================================================
// Public Methods
// ============================================================================

void ParameterSampler::setSeed(unsigned int seed)
{
    currentSeed_ = seed;
    random_.setSeed(seed);
}

std::vector<std::vector<float>> ParameterSampler::generateLHS(const SamplingConfig& config,
                                                              int paramCount)
{
    setSeed(config.seed);

    // Generate base LHS samples in [0,1]^paramCount
    auto samples = latinHypercubeSample(paramCount, config.sampleCount);

    // Apply distribution transforms based on constraints
    for (auto& sample : samples)
    {
        // Ensure sample has correct size
        if (static_cast<int>(sample.size()) != paramCount)
        {
            sample.resize(paramCount, 0.5f);
        }

        // Apply distribution transforms from constraints
        for (int i = 0; i < std::min(static_cast<int>(config.constraints.size()), paramCount); ++i)
        {
            const auto& constraint = config.constraints.getReference(i);
            sample[i] = transformDistribution(sample[i],
                                             constraint.distribution,
                                             constraint.minValue,
                                             constraint.maxValue);
        }

        // Apply physics-informed constraints
        applyConstraints(sample, config.constraints);
    }

    return samples;
}

std::vector<std::vector<float>> ParameterSampler::generateStratified(const SamplingConfig& config,
                                                                     int paramCount)
{
    setSeed(config.seed);

    auto genreMap = config.parseGenreStrata();

    // If no strata defined, fall back to regular LHS
    if (genreMap.empty())
    {
        return generateLHS(config, paramCount);
    }

    // Normalize percentages to ensure they sum to 1.0
    float totalPercentage = 0.0f;
    for (const auto& [genre, percentage] : genreMap)
    {
        totalPercentage += percentage;
    }

    std::vector<std::vector<float>> allSamples;
    allSamples.reserve(config.sampleCount);

    // Generate samples for each stratum
    for (const auto& [genre, percentage] : genreMap)
    {
        int stratumSamples = static_cast<int>(std::round(
            (percentage / totalPercentage) * static_cast<float>(config.sampleCount)));

        if (stratumSamples <= 0)
            continue;

        // Create config for this stratum
        SamplingConfig stratumConfig = config;
        stratumConfig.sampleCount = stratumSamples;
        stratumConfig.seed = currentSeed_ + static_cast<unsigned int>(allSamples.size());

        // Generate LHS samples for this stratum
        auto stratumResults = latinHypercubeSample(paramCount, stratumSamples);

        // Apply distribution transforms and constraints
        for (auto& sample : stratumResults)
        {
            if (static_cast<int>(sample.size()) != paramCount)
            {
                sample.resize(paramCount, 0.5f);
            }

            for (int i = 0; i < std::min(static_cast<int>(config.constraints.size()), paramCount); ++i)
            {
                const auto& constraint = config.constraints.getReference(i);
                sample[i] = transformDistribution(sample[i],
                                                 constraint.distribution,
                                                 constraint.minValue,
                                                 constraint.maxValue);
            }

            applyConstraints(sample, config.constraints);
        }

        // Append to all samples
        allSamples.insert(allSamples.end(),
                         std::make_move_iterator(stratumResults.begin()),
                         std::make_move_iterator(stratumResults.end()));
    }

    // Shuffle the combined samples to mix strata
    std::shuffle(allSamples.begin(), allSamples.end(),
                 std::mt19937(config.seed));

    return allSamples;
}

bool ParameterSampler::validateConstraints(const std::vector<float>& params,
                                           const juce::Array<ParameterConstraint>& constraints)
{
    // Check bounds constraints
    for (int i = 0; i < std::min(static_cast<int>(constraints.size()),
                                  static_cast<int>(params.size())); ++i)
    {
        const auto& constraint = constraints.getReference(i);
        if (params[i] < constraint.minValue || params[i] > constraint.maxValue)
        {
            return false;
        }
    }

    // Check dependency constraints
    for (int i = 0; i < static_cast<int>(constraints.size()); ++i)
    {
        const auto& constraint = constraints.getReference(i);
        for (const auto& dep : constraint.dependencyConstraints)
        {
            if (!evaluateDependencyConstraint(dep, params, constraints))
            {
                return false;
            }
        }
    }

    return true;
}

void ParameterSampler::applyConstraints(std::vector<float>& params,
                                        const juce::Array<ParameterConstraint>& constraints)
{
    // Apply bounds constraints first
    for (int i = 0; i < std::min(static_cast<int>(constraints.size()),
                                  static_cast<int>(params.size())); ++i)
    {
        const auto& constraint = constraints.getReference(i);
        params[i] = juce::jlimit(constraint.minValue, constraint.maxValue, params[i]);
    }

    // Apply dependency constraints (may need multiple passes)
    for (int pass = 0; pass < 3; ++pass) // Multiple passes for transitive constraints
    {
        bool anyApplied = false;

        for (int i = 0; i < static_cast<int>(constraints.size()); ++i)
        {
            const auto& constraint = constraints.getReference(i);

            for (const auto& dep : constraint.dependencyConstraints)
            {
                if (applySingleConstraint(params, dep, constraints))
                {
                    anyApplied = true;
                }
            }
        }

        if (!anyApplied)
            break;
    }
}

nlohmann::json ParameterSampler::exportConfig(const SamplingConfig& config)
{
    nlohmann::json json;

    json["sampleCount"] = config.sampleCount;
    json["seed"] = config.seed;

    // Export genre strata
    json["genreStrata"] = nlohmann::json::array();
    auto genreMap = config.parseGenreStrata();
    for (const auto& [genre, percentage] : genreMap)
    {
        nlohmann::json stratum;
        stratum["genre"] = genre.toStdString();
        stratum["percentage"] = percentage;
        json["genreStrata"].push_back(stratum);
    }

    // Export constraints
    json["constraints"] = nlohmann::json::array();
    for (const auto& constraint : config.constraints)
    {
        nlohmann::json c;
        c["name"] = constraint.name.toStdString();
        c["minValue"] = constraint.minValue;
        c["maxValue"] = constraint.maxValue;
        c["distribution"] = [d = constraint.distribution]() {
            switch (d)
            {
                case DistributionType::Linear: return "linear";
                case DistributionType::Logarithmic: return "logarithmic";
                case DistributionType::Exponential: return "exponential";
            }
            return "linear";
        }();

        if (!constraint.dependencyConstraints.isEmpty())
        {
            c["dependencies"] = nlohmann::json::array();
            for (const auto& dep : constraint.dependencyConstraints)
            {
                c["dependencies"].push_back(dep.toStdString());
            }
        }

        json["constraints"].push_back(c);
    }

    json["timestamp"] = juce::Time::getCurrentTime().toMilliseconds();

    return json;
}

nlohmann::json ParameterSampler::exportSamples(const std::vector<std::vector<float>>& samples,
                                               const SamplingConfig& config)
{
    nlohmann::json json;

    // Include configuration
    json["config"] = exportConfig(config);

    // Include samples
    json["samples"] = nlohmann::json::array();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        nlohmann::json sample;
        sample["id"] = i;
        sample["parameters"] = samples[i];
        json["samples"].push_back(sample);
    }

    json["totalSamples"] = samples.size();

    return json;
}

bool ParameterSampler::evaluateDependencyConstraint(const juce::String& expression,
                                                    const std::vector<float>& params,
                                                    const juce::Array<ParameterConstraint>& constraints)
{
    // Parse expression: "param1 < param2" or "param1 <= value"
    juce::StringArray tokens;
    tokens.addTokens(expression, " ", "\"");

    if (tokens.size() < 3)
        return true; // Invalid expression, pass by default

    juce::String leftOperand = tokens[0];
    juce::String op = tokens[1];
    juce::String rightOperand = tokens[2];

    // Get left value (must be a parameter)
    int leftIdx = getParameterIndex(leftOperand, constraints);
    if (leftIdx < 0 || leftIdx >= static_cast<int>(params.size()))
        return true;

    float leftValue = params[leftIdx];

    // Get right value (could be parameter or literal)
    float rightValue = 0.0f;
    int rightIdx = getParameterIndex(rightOperand, constraints);

    if (rightIdx >= 0 && rightIdx < static_cast<int>(params.size()))
    {
        rightValue = params[rightIdx];
    }
    else
    {
        // Try to parse as numeric literal
        rightValue = rightOperand.getFloatValue();
    }

    // Evaluate comparison
    if (op == "<")
        return leftValue < rightValue;
    else if (op == "<=")
        return leftValue <= rightValue;
    else if (op == ">")
        return leftValue > rightValue;
    else if (op == ">=")
        return leftValue >= rightValue;
    else if (op == "==" || op == "=")
        return std::abs(leftValue - rightValue) < 1e-6f;

    return true; // Unknown operator, pass by default
}

int ParameterSampler::getParameterIndex(const juce::String& name,
                                        const juce::Array<ParameterConstraint>& constraints)
{
    for (int i = 0; i < constraints.size(); ++i)
    {
        if (constraints.getReference(i).name.equalsIgnoreCase(name))
        {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Private Methods
// ============================================================================

std::vector<std::vector<float>> ParameterSampler::latinHypercubeSample(int dimensions,
                                                                        int samples)
{
    if (dimensions <= 0 || samples <= 0)
        return {};

    std::vector<std::vector<float>> result(samples, std::vector<float>(dimensions));

    // For each dimension, create a stratified sample
    for (int d = 0; d < dimensions; ++d)
    {
        // Create permutation of [0, samples-1]
        std::vector<int> permutation(samples);
        for (int i = 0; i < samples; ++i)
        {
            permutation[i] = i;
        }
        shuffleVector(permutation);

        // Map permutation to stratified values in [0,1]
        for (int i = 0; i < samples; ++i)
        {
            // Value in interval [i/samples, (i+1)/samples]
            // Center of interval is (i + 0.5) / samples
            float intervalStart = static_cast<float>(permutation[i]) / static_cast<float>(samples);
            float intervalEnd = static_cast<float>(permutation[i] + 1) / static_cast<float>(samples);

            // Sample uniformly within the interval
            result[i][d] = intervalStart + random_.nextFloat() * (intervalEnd - intervalStart);
        }
    }

    return result;
}

float ParameterSampler::transformDistribution(float uniformValue,
                                              DistributionType type,
                                              float minVal,
                                              float maxVal)
{
    // Clamp input to [0,1]
    uniformValue = juce::jlimit(0.0f, 1.0f, uniformValue);

    float result = 0.0f;

    switch (type)
    {
        case DistributionType::Linear:
            // Linear mapping: just scale to range
            result = minVal + uniformValue * (maxVal - minVal);
            break;

        case DistributionType::Logarithmic:
            // Log-spaced distribution (for frequency-like parameters)
            // Avoid log(0) by ensuring minimum positive value
            {
                float logMin = std::log10(std::max(minVal, 1e-6f));
                float logMax = std::log10(std::max(maxVal, 1e-6f));
                float logValue = logMin + uniformValue * (logMax - logMin);
                result = std::pow(10.0f, logValue);
            }
            break;

        case DistributionType::Exponential:
            // Exponential distribution (more values at lower end)
            // Using inverse transform: x = -ln(1-u) / lambda
            {
                // Avoid log(0) by clamping
                float u = juce::jlimit(1e-6f, 1.0f - 1e-6f, uniformValue);
                float expValue = -std::log(1.0f - u);

                // Normalize to [0,1] range (max ~4.6 for u near 1)
                float maxExp = 5.0f; // Approximate max for reasonable range
                float normalizedExp = juce::jmin(expValue / maxExp, 1.0f);

                result = minVal + normalizedExp * (maxVal - minVal);
            }
            break;
    }

    // Final clamp to ensure we're in bounds
    return juce::jlimit(minVal, maxVal, result);
}

bool ParameterSampler::applySingleConstraint(std::vector<float>& params,
                                             const juce::String& expression,
                                             const juce::Array<ParameterConstraint>& constraints)
{
    // Parse expression: "param1 < param2" or "param1 <= value"
    juce::StringArray tokens;
    tokens.addTokens(expression, " ", "\"");

    if (tokens.size() < 3)
        return false;

    juce::String leftOperand = tokens[0];
    juce::String op = tokens[1];
    juce::String rightOperand = tokens[2];

    int leftIdx = getParameterIndex(leftOperand, constraints);
    if (leftIdx < 0 || leftIdx >= static_cast<int>(params.size()))
        return false;

    // Get right value
    float rightValue = 0.0f;
    int rightIdx = getParameterIndex(rightOperand, constraints);

    bool rightIsParam = (rightIdx >= 0 && rightIdx < static_cast<int>(params.size()));

    if (rightIsParam)
    {
        rightValue = params[rightIdx];
    }
    else
    {
        rightValue = rightOperand.getFloatValue();
    }

    float leftValue = params[leftIdx];
    bool violated = false;

    // Check if constraint is violated
    if (op == "<")
        violated = !(leftValue < rightValue);
    else if (op == "<=")
        violated = !(leftValue <= rightValue);
    else if (op == ">")
        violated = !(leftValue > rightValue);
    else if (op == ">=")
        violated = !(leftValue >= rightValue);
    else if (op == "==" || op == "=")
        violated = !(std::abs(leftValue - rightValue) < 1e-6f);
    else
        return false;

    if (!violated)
        return false;

    // Apply fix based on operator type
    if (op == "<")
    {
        // Make left < right
        if (rightIsParam)
        {
            // Set left to be less than right
            float margin = 0.01f * std::abs(rightValue);
            params[leftIdx] = rightValue - std::max(margin, 0.001f);
        }
        else
        {
            params[leftIdx] = rightValue - 0.001f;
        }
    }
    else if (op == "<=")
    {
        // Make left <= right
        params[leftIdx] = rightValue;
    }
    else if (op == ">")
    {
        // Make left > right
        if (rightIsParam)
        {
            float margin = 0.01f * std::abs(rightValue);
            params[leftIdx] = rightValue + std::max(margin, 0.001f);
        }
        else
        {
            params[leftIdx] = rightValue + 0.001f;
        }
    }
    else if (op == ">=")
    {
        // Make left >= right
        params[leftIdx] = rightValue;
    }
    else if (op == "==" || op == "=")
    {
        // Make left == right
        params[leftIdx] = rightValue;
    }

    // Apply bounds constraint after modification
    if (leftIdx < static_cast<int>(constraints.size()))
    {
        const auto& c = constraints.getReference(leftIdx);
        params[leftIdx] = juce::jlimit(c.minValue, c.maxValue, params[leftIdx]);
    }

    return true;
}

void ParameterSampler::shuffleVector(std::vector<int>& vec)
{
    // Fisher-Yates shuffle using juce::Random
    for (size_t i = vec.size(); i > 1; --i)
    {
        size_t j = static_cast<size_t>(random_.nextInt(static_cast<int>(i)));
        std::swap(vec[i - 1], vec[j]);
    }
}

} // namespace morphsnap
