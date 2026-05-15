/*
 * More-Phi — Core/MSMatrix.h
 *
 * Mid/Side encode and decode operations for the mastering chain.
 * All operations are sample-level and buffer-level, header-only, noexcept.
 *
 * Convention:
 *   encode: L,R  →  M = (L+R)/2,  S = (L−R)/2
 *   decode: M,S  →  L = M+S,       R = M−S
 *
 * The ÷2 in encode and the matching ×2 implicit in decode ensure that
 * a round-trip through encode→decode produces exactly the original signal.
 *
 * Thread safety: stateless static helpers — safe from any thread.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

struct MSMatrix
{
    // ── Sample-level helpers ─────────────────────────────────────────────────

    static void encode(float& left, float& right) noexcept
    {
        const float m = (left  + right) * 0.5f;
        const float s = (left  - right) * 0.5f;
        left  = m;
        right = s;
    }

    static void decode(float& mid, float& side) noexcept
    {
        const float l = mid + side;
        const float r = mid - side;
        mid  = l;
        side = r;
    }

    // ── Buffer-level helpers ─────────────────────────────────────────────────

    static void encodeBuffer(juce::AudioBuffer<float>& buf) noexcept
    {
        if (buf.getNumChannels() < 2) return;
        float* L = buf.getWritePointer(0);
        float* R = buf.getWritePointer(1);
        const int n = buf.getNumSamples();
        for (int i = 0; i < n; ++i)
            encode(L[i], R[i]);
    }

    static void decodeBuffer(juce::AudioBuffer<float>& buf) noexcept
    {
        if (buf.getNumChannels() < 2) return;
        float* M = buf.getWritePointer(0);
        float* S = buf.getWritePointer(1);
        const int n = buf.getNumSamples();
        for (int i = 0; i < n; ++i)
            decode(M[i], S[i]);
    }
};

} // namespace more_phi
