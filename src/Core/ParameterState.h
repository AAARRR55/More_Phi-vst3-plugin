/*
 * MorphSnap — Core/ParameterState.h
 * Normalized parameter vector storage for snapshots.
 */
#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace morphsnap {

struct ParameterState
{
    std::vector<float> values;       // Normalized [0,1] per parameter
    std::string        name;
    bool               occupied = false;
    int                parameterCount = 0;  // Track host plugin changes

    void capture(const float* src, int count)
    {
        parameterCount = count;
        values.assign(src, src + count);
        occupied = true;
    }

    void clear()
    {
        values.clear();
        name.clear();
        occupied = false;
        parameterCount = 0;
    }
};

} // namespace morphsnap
