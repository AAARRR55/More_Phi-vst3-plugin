/*
 * More-Phi — Core/GeneticEngine.cpp
 * SanityMode-aware breeding and randomization.
 */
#include "GeneticEngine.h"

namespace more_phi {

ParameterState GeneticEngine::breed(const ParameterState& parentA,
                                     const ParameterState& parentB,
                                     float crossoverRatio,
                                     float mutationStrength,
                                     juce::Random& rng,
                                     const SanityConfig& sanity)
{
    ParameterState offspring;
    // Use parameterCount (not values.size() which is always MAX_PARAMETERS).
    // W-13 FIX (audit): breed across the FULL parameter range of both parents
    // (max count), not the minimum. The old jmin silently zeroed any params
    // present in the larger parent but absent in the smaller one — applying the
    // offspring then snapped those hosted params to 0. Now crossover/blending
    // runs over the shared range; the tail (present only in the larger parent)
    // is inherited unchanged from that parent so nothing is unexpectedly zeroed.
    const int sharedCount = juce::jmin(parentA.parameterCount, parentB.parameterCount);
    const int maxCount = juce::jmax(parentA.parameterCount, parentB.parameterCount);
    juce::ignoreUnused(parentA.parameterCount, parentB.parameterCount);

    for (int i = 0; i < sharedCount; ++i)
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

    // W-13 FIX (audit): inherit the tail (params beyond the shared range) from
    // whichever parent actually defines them, so the offspring doesn't zero
    // hosted parameters that only one parent exposed.
    for (int i = sharedCount; i < maxCount; ++i)
    {
        const float inherited = (i < parentA.parameterCount) ? parentA.values[static_cast<size_t>(i)]
                                                              : parentB.values[static_cast<size_t>(i)];
        offspring.values[static_cast<size_t>(i)] = inherited;
    }

    offspring.occupied = true;
    offspring.parameterCount = maxCount;
    return offspring;
}

void GeneticEngine::smartRandomize(ParameterState& state,
                                     float amount,
                                     const std::unordered_set<int>& learnedParams,
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

} // namespace more_phi
