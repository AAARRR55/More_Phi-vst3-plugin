/*
 * More-Phi — AI/OzoneParameterMap.cpp
 *
 * !! IMPORTANT — READ BEFORE EDITING !!
 * All parameter indices below are PLACEHOLDERS (-1 = unmapped).
 * To populate with real values:
 *   1. Load Ozone 11 Advanced in More-Phi.
 *   2. Run:  python scripts/audit_ozone_params.py --out-map ozone11_param_map.json
 *   3. Open ozone11_param_map.json and copy the "id" values into the structs below.
 *
 * Normalization ranges were derived from Ozone 11 Advanced documentation and
 * should be verified by a round-trip test: set_parameter → get_parameter.
 */
#include "OzoneParameterMap.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

// ── Factory ───────────────────────────────────────────────────────────────────

OzoneParameterMap OzoneParameterMap::buildForOzone11()
{
    OzoneParameterMap m;

    // ── EQ Band indices ──────────────────────────────────────────────────────
    // Format: { freqIdx, gainIdx, qIdx, typeIdx, enabledIdx }
    // Replace -1 values with real indices from ozone11_param_map.json
    //
    // Typical Ozone 11 EQ parameter naming pattern:
    //   "EQ Band 1 Frequency", "EQ Band 1 Gain", "EQ Band 1 Q",
    //   "EQ Band 1 Type", "EQ Band 1 Enabled"
    m.eq[0] = { -1, -1, -1, -1, -1 };  // Band 1
    m.eq[1] = { -1, -1, -1, -1, -1 };  // Band 2
    m.eq[2] = { -1, -1, -1, -1, -1 };  // Band 3
    m.eq[3] = { -1, -1, -1, -1, -1 };  // Band 4
    m.eq[4] = { -1, -1, -1, -1, -1 };  // Band 5
    m.eq[5] = { -1, -1, -1, -1, -1 };  // Band 6
    m.eq[6] = { -1, -1, -1, -1, -1 };  // Band 7
    m.eq[7] = { -1, -1, -1, -1, -1 };  // Band 8

    // ── Dynamics module ──────────────────────────────────────────────────────
    // Typical naming: "Dynamics Threshold", "Dynamics Ratio",
    //   "Dynamics Attack", "Dynamics Release"
    m.dynamics = {
        -1,  // thresholdIdx
        -1,  // ratioIdx
        -1,  // attackIdx
        -1,  // releaseIdx
    };

    // ── Imager per-band widths ────────────────────────────────────────────────
    // Typical naming: "Imager Sub Width", "Imager Low Width",
    //   "Imager Mid Width", "Imager High Width"
    // Order: [0]=sub, [1]=low, [2]=mid, [3]=high
    m.imager.widthIdx = { -1, -1, -1, -1 };

    // ── Maximizer ────────────────────────────────────────────────────────────
    // Typical naming: "Maximizer Output Level", "Maximizer Ceiling"
    m.maximizer = {
        -1,  // outputLevelIdx
        -1,  // ceilingIdx
    };

    return m;
}

// ── Plugin detection ──────────────────────────────────────────────────────────

bool OzoneParameterMap::isOzone11(const juce::String& pluginName) noexcept
{
    // Case-insensitive substring match; handles both "Ozone 11" and "iZotope Ozone 11 Advanced"
    const auto lower = pluginName.toLowerCase();
    return lower.contains("ozone 11") || lower.contains("ozone11");
}

// ── Normalization helpers ─────────────────────────────────────────────────────

float OzoneParameterMap::normalizeGain(float gainDB, float minDB, float maxDB) noexcept
{
    if (maxDB <= minDB) return 0.5f;
    return std::clamp((gainDB - minDB) / (maxDB - minDB), 0.0f, 1.0f);
}

float OzoneParameterMap::normalizeFreq(float hz, float minHz, float maxHz) noexcept
{
    if (hz <= 0.0f || minHz <= 0.0f || maxHz <= minHz) return 0.0f;
    const float logMin = std::log2(minHz);
    const float logMax = std::log2(maxHz);
    const float logHz  = std::log2(std::clamp(hz, minHz, maxHz));
    return std::clamp((logHz - logMin) / (logMax - logMin), 0.0f, 1.0f);
}

float OzoneParameterMap::normalizeThreshold(float dBFS, float minDBFS, float maxDBFS) noexcept
{
    if (maxDBFS <= minDBFS) return 1.0f;
    return std::clamp((dBFS - minDBFS) / (maxDBFS - minDBFS), 0.0f, 1.0f);
}

float OzoneParameterMap::normalizeWidth(float width) noexcept
{
    // width [0..2]: 0=mono, 1=unity, 2=double. VST3 maps to [0..1].
    return std::clamp(width * 0.5f, 0.0f, 1.0f);
}

float OzoneParameterMap::normalizeLUFS(float lufs, float minLUFS, float maxLUFS) noexcept
{
    if (maxLUFS <= minLUFS) return 0.5f;
    return std::clamp((lufs - minLUFS) / (maxLUFS - minLUFS), 0.0f, 1.0f);
}

float OzoneParameterMap::normalizeCeiling(float dBTP, float minDBTP, float maxDBTP) noexcept
{
    if (maxDBTP <= minDBTP) return 1.0f;
    return std::clamp((dBTP - minDBTP) / (maxDBTP - minDBTP), 0.0f, 1.0f);
}

float OzoneParameterMap::encodeFilterType(const juce::String& typeName) noexcept
{
    // Approximate normalized values for Ozone 11 EQ filter type selector.
    // Verify against actual parameter values from the audit.
    const auto lower = typeName.toLowerCase();
    if (lower.contains("lowshelf") || lower.contains("low shelf"))  return 0.0f;
    if (lower.contains("peak")     || lower.contains("bell"))       return 0.25f;
    if (lower.contains("highshelf")|| lower.contains("high shelf")) return 0.5f;
    if (lower.contains("highpass") || lower.contains("high pass"))  return 0.75f;
    if (lower.contains("lowpass")  || lower.contains("low pass"))   return 1.0f;
    return 0.25f;  // default: peak
}

} // namespace more_phi
