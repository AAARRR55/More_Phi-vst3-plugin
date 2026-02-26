/*
 * MorphSnap — Core/GeneticEngine.cpp
 * SanityMode-aware breeding and randomization.
 */
#include "GeneticEngine.h"

namespace morphsnap {

ParameterState GeneticEngine::breed(const ParameterState& parentA,
                                     const ParameterState& parentB,
                                     float crossoverRatio,
                                     float mutationStrength,
                                     juce::Random& rng,
                                     const SanityConfig& sanity)
{
    ParameterState offspring;
    // Use parameterCount (not values.size() which is always MAX_PARAMETERS)
    const int count = juce::jmin(parentA.parameterCount, parentB.parameterCount);
    if (parentA.parameterCount != parentB.parameterCount)
    {
        DBG("GeneticEngine::breed() - Parameter count mismatch: "
            + juce::String(parentA.parameterCount) + " vs "
            + juce::String(parentB.parameterCount) + ", using min: "
            + juce::String(count));
    }

    for (int i = 0; i < count; ++i)
    {
        // SanityMode: skip protected parameters — keep parentA's value unchanged
        if (sanity.enabled && sanity.protectedIndices.count(i) > 0)
        {
            offspring.values[static_cast<size_t>(i)] = parentA.values[static_cast<size_t>(i)];
            continue;
        }

        float blended = parentA.values[static_cast<size_t>(i)] * (1.0f - crossoverRatio)
                      + parentB.values[static_cast<size_t>(i)] * crossoverRatio;
        float mutation = (rng.nextFloat() * 2.0f - 1.0f) * mutationStrength;
        offspring.values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, blended + mutation);
    }

    offspring.occupied = true;
    offspring.parameterCount = count;
    offspring.setName("Offspring");
    return offspring;
}

void GeneticEngine::smartRandomize(ParameterState& state,
                                    float amount,
                                    const std::set<int>& learnedParams,
                                    juce::Random& rng,
                                    const SanityConfig& sanity)
{
    for (int i : learnedParams)
    {
        if (i < 0 || i >= state.parameterCount) continue;

        // SanityMode: skip protected parameters even if they're in learnedParams
        if (sanity.enabled && sanity.protectedIndices.count(i) > 0)
            continue;

        float offset = (rng.nextFloat() * 2.0f - 1.0f) * amount;
        state.values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, state.values[static_cast<size_t>(i)] + offset);
    }
}

} // namespace morphsnap
