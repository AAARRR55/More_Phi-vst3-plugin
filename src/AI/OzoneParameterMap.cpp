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
#include "Host/IPluginHostManager.h"
#include <cmath>
#include <algorithm>
#include <initializer_list>

namespace more_phi {

namespace {

bool containsAny(const juce::String& text, std::initializer_list<const char*> needles)
{
    for (const auto* needle : needles)
        if (text.contains(needle))
            return true;

    return false;
}

bool isDigit(juce::juce_wchar c) noexcept
{
    return c >= static_cast<juce::juce_wchar>('0') && c <= static_cast<juce::juce_wchar>('9');
}

bool hasDigitBoundaryBefore(const juce::String& text, int index) noexcept
{
    return index <= 0 || ! isDigit(text[index - 1]);
}

bool hasDigitBoundaryAfter(const juce::String& text, int index) noexcept
{
    const int afterIndex = index + 1;
    return afterIndex >= text.length() || ! isDigit(text[afterIndex]);
}

bool isLetterOrDigit(juce::juce_wchar c) noexcept
{
    return (c >= static_cast<juce::juce_wchar>('0') && c <= static_cast<juce::juce_wchar>('9'))
        || (c >= static_cast<juce::juce_wchar>('a') && c <= static_cast<juce::juce_wchar>('z'));
}

bool hasWordBoundaryBefore(const juce::String& text, int index) noexcept
{
    return index <= 0 || ! isLetterOrDigit(text[index - 1]);
}

bool hasWordBoundaryAfter(const juce::String& text, int index, int tokenLength) noexcept
{
    const int afterIndex = index + tokenLength;
    return afterIndex >= text.length() || ! isLetterOrDigit(text[afterIndex]);
}

bool containsWholeWord(const juce::String& text, const char* token)
{
    const int tokenLength = juce::String(token).length();
    int searchFrom = 0;

    while (true)
    {
        const int pos = text.indexOf(searchFrom, token);
        if (pos < 0)
            return false;

        if (hasWordBoundaryBefore(text, pos) && hasWordBoundaryAfter(text, pos, tokenLength))
            return true;

        searchFrom = pos + 1;
    }
}

bool containsEnableWord(const juce::String& text)
{
    return text.contains("enabled") || text.contains("enable")
        || text.contains("active") || containsWholeWord(text, "on");
}

bool isNonMainEQModuleParameter(const juce::String& text)
{
    return containsAny(text, {
        "dynamic eq", "dynamic equalizer",
        "vintage eq", "vintage equalizer",
        "match eq", "match equalizer"
    });
}

bool isMainEQParameterName(const juce::String& text)
{
    if (isNonMainEQModuleParameter(text))
        return false;

    return text.contains("eq band")
        || text.contains("equalizer")
        || text.contains("main eq");
}

int extractBandNumber(const juce::String& lowerName)
{
    auto parseSingleDigitAfter = [&lowerName](const char* token) -> int
    {
        int searchFrom = 0;
        while (true)
        {
            const int pos = lowerName.indexOf(searchFrom, token);
            if (pos < 0)
                return -1;

            const int digitPos = pos + juce::String(token).length();
            if (digitPos < lowerName.length() && isDigit(lowerName[digitPos])
                && hasDigitBoundaryAfter(lowerName, digitPos))
            {
                return static_cast<int>(lowerName[digitPos] - static_cast<juce::juce_wchar>('0'));
            }

            searchFrom = pos + 1;
        }
    };

    int band = parseSingleDigitAfter("eq band ");
    if (band >= 1 && band <= OzoneParameterMap::kEQBands)
        return band;

    band = parseSingleDigitAfter("band ");
    if (band >= 1 && band <= OzoneParameterMap::kEQBands)
        return band;

    for (int candidate = 1; candidate <= OzoneParameterMap::kEQBands; ++candidate)
    {
        const auto digit = juce::String(candidate);
        const int pos = lowerName.indexOf(digit);
        if (pos >= 0 && hasDigitBoundaryBefore(lowerName, pos) && hasDigitBoundaryAfter(lowerName, pos))
            return candidate;
    }

    return -1;
}

int countMapped(const OzoneParameterMap& map) noexcept
{
    int count = 0;

    for (const auto& band : map.eq)
    {
        count += band.freqIdx    >= 0 ? 1 : 0;
        count += band.gainIdx    >= 0 ? 1 : 0;
        count += band.qIdx       >= 0 ? 1 : 0;
        count += band.typeIdx    >= 0 ? 1 : 0;
        count += band.enabledIdx >= 0 ? 1 : 0;
    }

    count += map.dynamics.thresholdIdx >= 0 ? 1 : 0;
    count += map.dynamics.ratioIdx     >= 0 ? 1 : 0;
    count += map.dynamics.attackIdx    >= 0 ? 1 : 0;
    count += map.dynamics.releaseIdx   >= 0 ? 1 : 0;

    for (int idx : map.imager.widthIdx)
        count += idx >= 0 ? 1 : 0;

    count += map.maximizer.outputLevelIdx >= 0 ? 1 : 0;
    count += map.maximizer.ceilingIdx     >= 0 ? 1 : 0;

    return count;
}

} // namespace

// ── Factory ───────────────────────────────────────────────────────────────────

// AUDIT-FIX-5
bool OzoneParameterMap::hasAnyMapping() const noexcept
{
    for (int i = 0; i < kEQBands; ++i)
    {
        const auto& b = eq[static_cast<std::size_t>(i)];
        if (b.freqIdx >= 0 || b.gainIdx >= 0 || b.qIdx >= 0
            || b.typeIdx >= 0 || b.enabledIdx >= 0)
            return true;
    }
    if (dynamics.thresholdIdx >= 0 || dynamics.ratioIdx >= 0
        || dynamics.attackIdx >= 0 || dynamics.releaseIdx >= 0)
        return true;
    for (int i = 0; i < 4; ++i)
        if (imager.widthIdx[static_cast<std::size_t>(i)] >= 0)
            return true;
    if (maximizer.outputLevelIdx >= 0 || maximizer.ceilingIdx >= 0)
        return true;
    return false;
}

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

OzoneParameterMap OzoneParameterMap::buildFromHostedPlugin(const IParameterBridge& bridge)
{
    OzoneParameterMap m;
    const int totalParams = bridge.getParameterCount();

    for (int index = 0; index < totalParams; ++index)
    {
        const auto name = bridge.getParameterName(index).toLowerCase();
        if (name.isEmpty())
            continue;

        if (isMainEQParameterName(name))
        {
            const int bandNumber = extractBandNumber(name);
            if (bandNumber >= 1 && bandNumber <= kEQBands)
            {
                auto& band = m.eq[static_cast<size_t>(bandNumber - 1)];

                if (band.freqIdx < 0 && containsAny(name, { "frequency", "freq" }))
                    band.freqIdx = index;
                else if (band.gainIdx < 0 && containsAny(name, { "gain" }))
                    band.gainIdx = index;
                else if (band.qIdx < 0 && (containsWholeWord(name, "q") || name.contains("bandwidth")))
                    band.qIdx = index;
                else if (band.typeIdx < 0 && containsAny(name, { "type", "shape", "filter" }))
                    band.typeIdx = index;
                else if (band.enabledIdx < 0 && containsEnableWord(name))
                    band.enabledIdx = index;
            }
        }

        if (name.contains("dynamics"))
        {
            if (m.dynamics.thresholdIdx < 0 && name.contains("threshold"))
                m.dynamics.thresholdIdx = index;
            else if (m.dynamics.ratioIdx < 0 && name.contains("ratio"))
                m.dynamics.ratioIdx = index;
            else if (m.dynamics.attackIdx < 0 && name.contains("attack"))
                m.dynamics.attackIdx = index;
            else if (m.dynamics.releaseIdx < 0 && name.contains("release"))
                m.dynamics.releaseIdx = index;
        }

        if (name.contains("imager") && name.contains("width"))
        {
            if (m.imager.widthIdx[0] < 0 && containsAny(name, { "sub", "low bass", "band 1" }))
                m.imager.widthIdx[0] = index;
            else if (m.imager.widthIdx[1] < 0 && containsAny(name, { "low", "bass", "band 2" }))
                m.imager.widthIdx[1] = index;
            else if (m.imager.widthIdx[2] < 0 && containsAny(name, { "mid", "band 3" }))
                m.imager.widthIdx[2] = index;
            else if (m.imager.widthIdx[3] < 0 && containsAny(name, { "high", "air", "treble", "band 4" }))
                m.imager.widthIdx[3] = index;
        }

        if (name.contains("maximizer"))
        {
            if (m.maximizer.outputLevelIdx < 0
                && containsAny(name, { "output level", "threshold", "target" }))
            {
                m.maximizer.outputLevelIdx = index;
            }
            else if (m.maximizer.ceilingIdx < 0
                     && containsAny(name, { "ceiling", "true peak ceiling", "tp ceiling" }))
            {
                m.maximizer.ceilingIdx = index;
            }
        }
    }

    DBG("OzoneParameterMap::buildFromHostedPlugin mapped " + juce::String(countMapped(m))
        + " / " + juce::String(totalParams) + " hosted parameters");

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

float OzoneParameterMap::normalizeQ(float q, float minQ, float maxQ) noexcept
{
    // Q is linear in normalized VST3 space. Range [0.1, 8.0] matches
    // PluginSemanticMapper ("0.3 to 8.0") + EQParameterTranslator's 0.1 floor.
    // (Previously this was mis-encoded via normalizeFreq's log2 curve.)
    if (maxQ <= minQ) return 0.5f;
    return std::clamp((q - minQ) / (maxQ - minQ), 0.0f, 1.0f);
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
