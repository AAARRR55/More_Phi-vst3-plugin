/*
 * More-Phi — Core/GeneticEngine.h
 * Crossover + mutation breeding and smart randomization.
 * SanityMode: protects "danger" parameters (Volume, Pitch, Bypass, OutputGain)
 * from being modified during breed/randomize operations.
 */
#pragma once

#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <unordered_set>

namespace more_phi {

/** Configuration for SanityMode — protects danger parameters from modification. */
struct SanityConfig
{
    bool enabled = true;
    std::unordered_set<int> protectedIndices;  // L-1 FIX: O(1) lookup instead of O(log n)
};

class GeneticEngine
{
public:
    // Breed two parents with crossover ratio [0=A, 1=B] and mutation strength [0,1]
    // Protected indices from sanity config will retain parentA's values unchanged.
    static ParameterState breed(const ParameterState& parentA,
                                const ParameterState& parentB,
                                float crossoverRatio,
                                float mutationStrength,
                                juce::Random& rng,
                                const SanityConfig& sanity = {});

    // Randomize only learned params; amount [0=none, 1=full random]
    // Protected indices from sanity config are excluded even if in learnedParams.
    static void smartRandomize(ParameterState& state,
                                float amount,
                                const std::unordered_set<int>& learnedParams,
                                juce::Random& rng,
                                const SanityConfig& sanity = {});
};

} // namespace more_phi
