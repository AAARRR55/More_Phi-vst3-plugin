/*
 * MorphSnap — Core/GeneticEngine.h
 * Crossover + mutation breeding and smart randomization.
 */
#pragma once

#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <set>

namespace morphsnap {

class GeneticEngine
{
public:
    // Breed two parents with crossover ratio [0=A, 1=B] and mutation strength [0,1]
    static ParameterState breed(const ParameterState& parentA,
                                const ParameterState& parentB,
                                float crossoverRatio,
                                float mutationStrength,
                                juce::Random& rng);

    // Randomize only learned params; amount [0=none, 1=full random]
    static void smartRandomize(ParameterState& state,
                               float amount,
                               const std::set<int>& learnedParams,
                               juce::Random& rng);
};

} // namespace morphsnap
