/*
 * MorphSnap — Core/ParameterState.h
 * Normalized parameter vector storage for snapshots.
 * REAL-TIME SAFE: Uses fixed-size array to avoid allocations.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>

namespace morphsnap {

// Maximum supported parameters per hosted plugin
// Most plugins have < 500 parameters; this provides headroom
constexpr int MAX_PARAMETERS = 2048;

struct ParameterState
{
    std::array<float, MAX_PARAMETERS> values{};  // Fixed-size, no allocation
    char   name[64]{};                           // Fixed buffer — RT-safe (no heap)
    bool   occupied = false;
    int    parameterCount = 0;

    void capture(const float* src, int count)
    {
        parameterCount = (count > MAX_PARAMETERS) ? MAX_PARAMETERS : count;
        for (int i = 0; i < parameterCount; ++i)
            values[static_cast<size_t>(i)] = src[i];
        occupied = true;
    }

    void clear()
    {
        name[0]        = '\0';
        occupied       = false;
        parameterCount = 0;
        // Note: Don't zero values array - not necessary, saves cycles
    }

    // Convenience setter — copies up to 63 chars, always null-terminates
    void setName(const char* src)
    {
        if (!src) { name[0] = '\0'; return; }

        const size_t maxChars = sizeof(name) - 1;
        const size_t srcLen = std::strlen(src);
        const size_t copyLen = std::min(srcLen, maxChars);

        std::memcpy(name, src, copyLen);
        name[copyLen] = '\0';
    }

    // Convenience accessor for compatibility with vector-based code
    const float* data() const { return values.data(); }
    int size() const { return parameterCount; }
};

} // namespace morphsnap
