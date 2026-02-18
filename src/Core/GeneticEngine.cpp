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
    const size_t count = juce::jmin(parentA.values.size(), parentB.values.size());
    offspring.values.resize(count);

    for (size_t i = 0; i < count; ++i)
    {
        float blended = parentA.values[i] * (1.0f - crossoverRatio)
                      + parentB.values[i] * crossoverRatio;
        float mutation = (rng.nextFloat() * 2.0f - 1.0f) * mutationStrength;
        offspring.values[i] = juce::jlimit(0.0f, 1.0f, blended + mutation);
    }

    offspring.occupied = true;
    offspring.parameterCount = static_cast<int>(count);
    offspring.name = "Offspring";
    return offspring;
}

void GeneticEngine::smartRandomize(ParameterState& state,
                                    float amount,
                                    const std::set<int>& learnedParams,
                                    juce::Random& rng)
{
    for (int i : learnedParams)
    {
        if (i < 0 || i >= static_cast<int>(state.values.size())) continue;
        float offset = (rng.nextFloat() * 2.0f - 1.0f) * amount;
        state.values[i] = juce::jlimit(0.0f, 1.0f, state.values[i] + offset);
    }
}

} // namespace morphsnap
