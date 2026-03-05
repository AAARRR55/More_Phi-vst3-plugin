/*
 * MorphSnap — AI/Dataset/ParameterSampler.h
 * Advanced parameter sampling for synthetic audio dataset generation.
 * Provides Latin Hypercube Sampling, stratified sampling by genre,
 * and physics-informed constraint validation.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <functional>

namespace morphsnap {

/** Distribution type for parameter sampling */
enum class DistributionType
{
    Linear,       ///< Uniform distribution in linear space
    Logarithmic,  ///< Uniform distribution in log space (for frequency-like params)
    Exponential   ///< Exponential distribution (more values at lower end)
};

/** Constraint definition for a single parameter */
struct ParameterConstraint
{
    float minValue = 0.0f;
    float maxValue = 1.0f;
    DistributionType distribution = DistributionType::Linear;
    juce::String name;
    juce::StringArray dependencyConstraints; ///< e.g., "attack < release", "gain <= 0.7"

    /** Default constructor */
    ParameterConstraint() = default;

    /** Constructor with basic parameters */
    ParameterConstraint(juce::String paramName,
                       float min = 0.0f,
                       float max = 1.0f,
                       DistributionType dist = DistributionType::Linear)
        : minValue(min), maxValue(max), distribution(dist), name(std::move(paramName)) {}
};

/** Configuration for sampling operations */
struct SamplingConfig
{
    int sampleCount = 1000;
    unsigned int seed = 42;
    juce::StringArray genreStrata;  ///< Format: "GenreName:percentage" e.g., "Electronic:0.25"
    juce::Array<ParameterConstraint> constraints;

    /** Add a genre stratum */
    void addGenreStratum(const juce::String& genre, float percentage)
    {
        genreStrata.add(genre + ":" + juce::String(percentage, 4));
    }

    /** Parse genre strata into a map */
    std::unordered_map<juce::String, float> parseGenreStrata() const
    {
        std::unordered_map<juce::String, float> result;
        for (const auto& stratum : genreStrata)
        {
            auto parts = juce::StringArray::fromTokens(stratum, ":", "");
            if (parts.size() == 2)
            {
                result[parts[0]] = parts[1].getFloatValue();
            }
        }
        return result;
    }
};

/**
 * Parameter sampler supporting Latin Hypercube Sampling, stratified sampling,
 * and physics-informed constraints for audio plugin parameter spaces.
 */
class ParameterSampler
{
public:
    ParameterSampler() = default;

    /**
     * Generate samples using Latin Hypercube Sampling (LHS).
     * LHS ensures uniform coverage of the parameter space by dividing each
     * dimension into equal intervals and sampling once per interval.
     *
     * @param config Sampling configuration
     * @param paramCount Number of parameters (dimensions)
     * @return Vector of parameter samples (each sample is a vector of paramCount floats)
     */
    std::vector<std::vector<float>> generateLHS(const SamplingConfig& config, int paramCount);

    /**
     * Generate stratified samples by genre.
     * Distributes samples across genre categories according to configured percentages.
     *
     * @param config Sampling configuration with genre strata
     * @param paramCount Number of parameters (dimensions)
     * @return Vector of parameter samples with genre assignments in metadata
     */
    std::vector<std::vector<float>> generateStratified(const SamplingConfig& config, int paramCount);

    /**
     * Validate a parameter set against all constraints.
     *
     * @param params Parameter values to validate
     * @param constraints Constraint definitions
     * @return true if all constraints are satisfied
     */
    bool validateConstraints(const std::vector<float>& params,
                            const juce::Array<ParameterConstraint>& constraints);

    /**
     * Apply constraints to a parameter set, modifying values in place.
     * This fixes constraint violations by adjusting parameter values.
     *
     * @param params Parameter values to modify
     * @param constraints Constraint definitions with parameter names
     */
    void applyConstraints(std::vector<float>& params,
                         const juce::Array<ParameterConstraint>& constraints);

    /**
     * Export sampling configuration to JSON format.
     *
     * @param config Configuration to export
     * @return JSON object with full configuration
     */
    nlohmann::json exportConfig(const SamplingConfig& config);

    /**
     * Export generated samples to JSON format with metadata.
     *
     * @param samples Generated parameter samples
     * @param config Configuration used for generation
     * @return JSON array with samples and metadata
     */
    nlohmann::json exportSamples(const std::vector<std::vector<float>>& samples,
                                 const SamplingConfig& config);

    /**
     * Set random seed for reproducibility.
     *
     * @param seed Seed value for the random number generator
     */
    void setSeed(unsigned int seed);

    /**
     * Get the current random seed.
     */
    unsigned int getSeed() const noexcept { return currentSeed_; }

    /**
     * Parse a dependency constraint expression and evaluate it.
     * Supported operators: <, <=, >, >=, ==
     * Examples: "attack < release", "gain <= 0.7", "freq1 == freq2"
     *
     * @param expression Constraint expression string
     * @param params Parameter values
     * @param constraints Constraint definitions (for parameter name lookup)
     * @return true if constraint is satisfied
     */
    bool evaluateDependencyConstraint(const juce::String& expression,
                                      const std::vector<float>& params,
                                      const juce::Array<ParameterConstraint>& constraints);

    /**
     * Get parameter index by name from constraints.
     *
     * @param name Parameter name
     * @param constraints Constraint definitions
     * @return Index of parameter, or -1 if not found
     */
    int getParameterIndex(const juce::String& name,
                         const juce::Array<ParameterConstraint>& constraints);

private:
    /**
     * Core Latin Hypercube Sampling implementation.
     * Generates a matrix of samples with one sample per interval per dimension.
     */
    std::vector<std::vector<float>> latinHypercubeSample(int dimensions, int samples);

    /**
     * Transform a uniform [0,1] value according to the specified distribution.
     */
    float transformDistribution(float uniformValue,
                               DistributionType type,
                               float minVal,
                               float maxVal);

    /**
     * Parse and apply a single constraint expression.
     * Returns true if constraint could be applied.
     */
    bool applySingleConstraint(std::vector<float>& params,
                               const juce::String& expression,
                               const juce::Array<ParameterConstraint>& constraints);

    /**
     * Shuffle a vector using the internal random generator.
     */
    void shuffleVector(std::vector<int>& vec);

    juce::Random random_;
    unsigned int currentSeed_ = 42;
};

} // namespace morphsnap
