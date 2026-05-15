/*
 * More-Phi — AI/EQParameterTranslator.h
 *
 * LLM2Fx-style EQ parameter translator.
 *
 * Translates a natural-language audio description + spectral analysis
 * into 8 parametric EQ band parameters (bands 0–7 of AdaptiveEQ).
 *
 * Architecture follows LLM2Fx:
 *   1. ParameterContextBuilder constructs a system prompt with DASP-style
 *      EQ function signatures and few-shot examples from PluginProfileDB.
 *   2. Analysis string summarizes per-band RMS and spectral tilt.
 *   3. LLMProvider runs inference and returns JSON.
 *   4. JSON is parsed, validated, and pushed to AdaptiveEQ via callback.
 *
 * The translator runs on the message thread (via juce::Timer at ~1 Hz when
 * active). The callback delivers results via enqueueParameterSet().
 *
 * Thread safety:
 *   All methods — message thread only.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <array>
#include "../Core/AdaptiveEQ.h"

namespace more_phi {

class EQParameterTranslator : public juce::Timer
{
public:
    static constexpr int kNumLLMBands = 8;   // bands 0-7 of AdaptiveEQ

    using EQUpdateCallback = std::function<void(int band, const AdaptiveEQ::BandParams&)>;

    EQParameterTranslator() = default;
    ~EQParameterTranslator() override { stopTimer(); }

    [[nodiscard]] juce::String getStatus() const { return "available"; }

    // ── Configuration ──────────────────────────────────────────────────────────

    /** Register callback to receive EQ updates. */
    void setUpdateCallback(EQUpdateCallback cb) { updateCallback_ = std::move(cb); }

    /** Set genre descriptor (e.g., "warm bass-heavy EDM"). */
    void setDescriptor(const juce::String& descriptor) { descriptor_ = descriptor; }

    /** Set spectral analysis summary for context building. */
    void setSpectralSummary(const juce::String& summary) { spectralSummary_ = summary; }

    // ── Immediate translation ──────────────────────────────────────────────────

    /**
     * Apply heuristic EQ warm-start based on genre descriptor.
     * Used when LLM is not available. Produces tasteful, safe defaults.
     */
    void applyHeuristicWarmStart(const juce::String& genre) noexcept;

    /** Apply a raw JSON EQ prescription (e.g., from MCP LLM response). */
    bool applyFromJSON(const juce::String& json) noexcept;

    // ── Timer-driven LLM translation ───────────────────────────────────────────

    /** Start automatic translation updates at ~0.2 Hz (every 5 s). */
    void start() { startTimer(5000); }
    void stop()  { stopTimer(); }

private:
    void timerCallback() override;

    bool parseBandsFromJSON(const juce::String& json) noexcept;
    void pushBand(int band, const AdaptiveEQ::BandParams& p) noexcept
    {
        if (updateCallback_) updateCallback_(band, p);
    }

    EQUpdateCallback updateCallback_;
    juce::String     descriptor_       { "neutral" };
    juce::String     spectralSummary_  {};
};

} // namespace more_phi
