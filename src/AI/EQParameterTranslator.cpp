/*
 * More-Phi — AI/EQParameterTranslator.cpp
 * LLM2Fx-style EQ parameter translation with heuristic warm-starts.
 */
#include "EQParameterTranslator.h"
#include <juce_core/juce_core.h>

namespace more_phi {

// Heuristic warm-starts for common genres (LLM2Fx fallback)
struct GenreEQPreset {
    const char* genre;
    AdaptiveEQ::BandParams bands[EQParameterTranslator::kNumLLMBands];
};

static const GenreEQPreset kGenrePresets[] = {
    {
        "electronic_dance",
        {
            { 60.f,   2.0f, 0.7f, AdaptiveEQ::BandType::LowShelf,  true },
            { 120.f,  1.5f, 1.2f, AdaptiveEQ::BandType::Peak,      true },
            { 250.f, -1.0f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 800.f, -0.5f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 2500.f, 0.5f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 6000.f, 1.0f, 0.8f, AdaptiveEQ::BandType::Peak,      true },
            { 12000.f,1.5f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
            { 18000.f,0.5f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
        }
    },
    {
        "hip_hop_rnb",
        {
            { 60.f,   3.0f, 0.7f, AdaptiveEQ::BandType::LowShelf,  true },
            { 100.f,  2.0f, 1.2f, AdaptiveEQ::BandType::Peak,      true },
            { 300.f, -1.5f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 1000.f, 0.0f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 3000.f, 0.5f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 8000.f, 1.0f, 0.8f, AdaptiveEQ::BandType::Peak,      true },
            { 14000.f,0.5f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
            { 18000.f,0.0f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
        }
    },
    {
        "folk_acoustic",
        {
            { 80.f,  -1.0f, 0.7f, AdaptiveEQ::BandType::LowShelf,  true },
            { 200.f, -0.5f, 1.2f, AdaptiveEQ::BandType::Peak,      true },
            { 400.f,  0.0f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 1200.f, 0.5f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 3000.f, 1.0f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 7000.f, 1.0f, 0.8f, AdaptiveEQ::BandType::Peak,      true },
            { 12000.f,0.5f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
            { 18000.f,0.0f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
        }
    },
    // Default neutral preset
    {
        "neutral",
        {
            { 80.f,   0.f, 0.7f, AdaptiveEQ::BandType::LowShelf,  true },
            { 200.f,  0.f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 500.f,  0.f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 1000.f, 0.f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 3000.f, 0.f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 6000.f, 0.f, 1.0f, AdaptiveEQ::BandType::Peak,      true },
            { 12000.f,0.f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
            { 18000.f,0.f, 0.7f, AdaptiveEQ::BandType::HighShelf, true },
        }
    },
};

void EQParameterTranslator::applyHeuristicWarmStart(const juce::String& genre) noexcept
{
    // Find matching preset
    const GenreEQPreset* chosen = &kGenrePresets[3];  // neutral default
    for (const auto& preset : kGenrePresets)
    {
        if (genre.containsIgnoreCase(preset.genre))
        {
            chosen = &preset;
            break;
        }
    }

    for (int b = 0; b < kNumLLMBands; ++b)
        pushBand(b, chosen->bands[b]);
}

bool EQParameterTranslator::applyFromJSON(const juce::String& json) noexcept
{
    return parseBandsFromJSON(json);
}

bool EQParameterTranslator::parseBandsFromJSON(const juce::String& json) noexcept
{
    // Parse JSON of the form:
    // {"bands":[{"freq":80,"gain":1.5,"Q":0.7,"type":"lowshelf"},...]
    if (json.isEmpty()) return false;

    juce::var parsed;
    if (juce::JSON::parse(json, parsed).failed()) return false;

    const juce::var* bandsVar = parsed.getDynamicObject()
                                ? parsed.getDynamicObject()->getProperties().getVarPointer("bands")
                                : nullptr;
    if (bandsVar == nullptr || !bandsVar->isArray()) return false;

    const juce::Array<juce::var>* bandsArr = bandsVar->getArray();
    if (bandsArr == nullptr) return false;

    const int n = std::min(bandsArr->size(), kNumLLMBands);
    for (int b = 0; b < n; ++b)
    {
        const juce::var& band = (*bandsArr)[b];
        if (!band.isObject()) continue;

        AdaptiveEQ::BandParams p;
        p.freqHz  = static_cast<float>(static_cast<double>(band["freq"]));
        p.gainDB  = static_cast<float>(static_cast<double>(band["gain"]));
        p.Q       = static_cast<float>(static_cast<double>(band["Q"]));
        p.enabled = true;

        const juce::String typeStr = band["type"].toString();
        if      (typeStr == "lowshelf")   p.type = AdaptiveEQ::BandType::LowShelf;
        else if (typeStr == "highshelf")  p.type = AdaptiveEQ::BandType::HighShelf;
        else if (typeStr == "lowpass")    p.type = AdaptiveEQ::BandType::LowPass;
        else if (typeStr == "highpass")   p.type = AdaptiveEQ::BandType::HighPass;
        else                              p.type = AdaptiveEQ::BandType::Peak;

        // Validate
        if (p.freqHz < 20.f || p.freqHz > 20000.f) continue;
        p.gainDB = std::clamp(p.gainDB, -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB);
        p.Q      = std::max(0.1f, p.Q);

        pushBand(b, p);
    }
    return true;
}

void EQParameterTranslator::timerCallback()
{
    // LLM translation stub: when LLMProvider integration is complete,
    // build context with ParameterContextBuilder, call LLMProvider::complete(),
    // parse response JSON, call applyFromJSON().
    // For now, apply heuristic warm-start based on current descriptor.
    applyHeuristicWarmStart(descriptor_);
    stop();  // Run once per manual trigger, not continuously
}

} // namespace more_phi
