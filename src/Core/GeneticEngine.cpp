/*
 * MorphSnap — Core/GeneticEngine.cpp
 */
#include "GeneticEngine.h"

namespace morphsnap {

ParameterState GeneticEngine::breed(const ParameterState& parentA,
                                     const ParameterState& parentB,
                                     float crossoverRatio,
                                     float mutationStrength,
                                     juce::Random& rng)
{
    ParameterState offspring;
    // Use parameterCount (not values.size() which is always MAX_PARAMETERS)
    const int count = juce::jmin(parentA.parameterCount, parentB.parameterCount);

    for (int i = 0; i < count; ++i)
    {
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
                                    juce::Random& rng)
{
    for (int i : learnedParams)
    {
        if (i < 0 || i >= state.parameterCount) continue;
        float offset = (rng.nextFloat() * 2.0f - 1.0f) * amount;
        state.values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, state.values[static_cast<size_t>(i)] + offset);
    }
}

} // namespace morphsnap
