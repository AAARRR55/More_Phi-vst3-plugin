/*
 * MorphSnap — Core/GeneticEngine.h
 * Crossover + mutation breeding and smart randomization.
 * SanityMode: protects "danger" parameters (Volume, Pitch, Bypass, OutputGain)
 * from being modified during breed/randomize operations.
 */
#pragma once

#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <set>

namespace morphsnap {

/** Configuration for SanityMode — protects danger parameters from modification. */
struct SanityConfig
{
    bool enabled = true;
    std::set<int> protectedIndices;  // Parameter indices to protect (e.g. Volume, Pitch, Bypass)
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
                               const std::set<int>& learnedParams,
                               juce::Random& rng,
                               const SanityConfig& sanity = {});
};

} // namespace morphsnap
