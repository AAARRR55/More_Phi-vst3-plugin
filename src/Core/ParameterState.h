/*
 * More-Phi — Core/ParameterState.h
 * Normalized parameter vector storage for snapshots.
 * REAL-TIME SAFE: Uses fixed-size array to avoid allocations.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>

namespace more_phi {

// Maximum supported parameters per hosted plugin
// Most plugins have < 500 parameters; this provides headroom
constexpr int MAX_PARAMETERS = 2048;

// Reference block duration: 512 samples @ 44.1 kHz.
// Used as the canonical dt for smoothing time-constant derivation (MorphProcessor)
// and the legacy call-site default (DiscreteParameterHandler). Shared here so both
// consumers refer to a single constant.
constexpr float kRefDt = 512.0f / 44100.0f;

struct ParameterState
{
    std::array<float, MAX_PARAMETERS> values{};  // 2048 floats, fixed-size, no heap
    char   name[64]{};                            // Fixed char buffer, RT-safe
    bool   occupied = false;
    int    parameterCount = 0;
    float  mass = 1.0f;                          // Gravity well: [0.1, 3.0], default 1.0

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
        // Note: Don't zero values array — not necessary, saves cycles.
        // Callers MUST check `occupied` before reading `values`; stale data
        // from the previous occupant persists in the array after clear().
        //
        // WARNING: Any NEW code path that reads `values` MUST check `occupied`
        // first, otherwise it will silently consume stale data from the slot's
        // previous occupant. See copySlotValues() for the canonical pattern.
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

} // namespace more_phi
