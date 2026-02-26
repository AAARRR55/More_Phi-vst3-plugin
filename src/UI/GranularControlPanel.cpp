/*
 * MorphSnap — UI/GranularControlPanel.cpp
 *
 * Implements the granular engine control strip.
 * Direct engine calls — no APVTS, no parameter automation.
 */
#include "GranularControlPanel.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

// ── Color constants ───────────────────────────────────────────────────────────
namespace {
    constexpr juce::uint32 kBgColour       = 0xff16213e;
    constexpr juce::uint32 kSurfaceColour  = 0xff1a2742;
    constexpr juce::uint32 kAccentColour   = 0xffec415d;
    constexpr juce::uint32 kTextColour     = 0xffe8eaed;
    constexpr juce::uint32 kTextDimColour  = 0xff4a5568;
    constexpr juce::uint32 kBorderColour   = 0xff1e3a5f;

    // Knob metadata
    struct KnobDef
    {
        const char* label;
        double      min;
        double      max;
        double      defaultVal;
        const char* suffix;
    };

    constexpr std::array<KnobDef, 4> kKnobDefs
    {{
        { "Size",    20.0,  200.0, 50.0,  " ms" },
        { "Density",  5.0,  100.0, 20.0,  " g/s" },
        { "Pitch",    0.0,    2.0,  0.0,  " st" },
        { "Scatter",  0.0,    1.0,  0.0,  "" }
    }};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

GranularControlPanel::GranularControlPanel(MorphSnapProcessor& proc)
    : proc_(proc)
{
    auto& engine = proc_.getGranularEngine();

    // ── Active toggle ─────────────────────────────────────────────────────────
    setupToggleButton(activeToggle_);
    activeToggle_.setToggleState(engine.isActive(), juce::dontSendNotification);
    activeToggle_.onClick = [this, &engine]()
    {
        engine.setActive(activeToggle_.getToggleState());
    };
    addAndMakeVisible(activeToggle_);

    // ── Knob 0: Grain Size ────────────────────────────────────────────────────
    setupKnob(knobs_[0], kKnobDefs[0].min, kKnobDefs[0].max,
               kKnobDefs[0].defaultVal, kKnobDefs[0].suffix);
    knobs_[0].onValueChange = [this, &engine]()
    {
        engine.setGrainSize(static_cast<float>(knobs_[0].getValue()));
    };

    // ── Knob 1: Density ───────────────────────────────────────────────────────
    setupKnob(knobs_[1], kKnobDefs[1].min, kKnobDefs[1].max,
               kKnobDefs[1].defaultVal, kKnobDefs[1].suffix);
    knobs_[1].onValueChange = [this, &engine]()
    {
        engine.setGrainDensity(static_cast<float>(knobs_[1].getValue()));
    };

    // ── Knob 2: Pitch Randomization ───────────────────────────────────────────
    setupKnob(knobs_[2], kKnobDefs[2].min, kKnobDefs[2].max,
               kKnobDefs[2].defaultVal, kKnobDefs[2].suffix);
    knobs_[2].onValueChange = [this, &engine]()
    {
        engine.setPitchRandomization(static_cast<float>(knobs_[2].getValue()));
    };

    // ── Knob 3: Position Randomization ───────────────────────────────────────
    setupKnob(knobs_[3], kKnobDefs[3].min, kKnobDefs[3].max,
               kKnobDefs[3].defaultVal, kKnobDefs[3].suffix);
    knobs_[3].onValueChange = [this, &engine]()
    {
        engine.setPositionRandomization(static_cast<float>(knobs_[3].getValue()));
    };

    // ── Labels ────────────────────────────────────────────────────────────────
    for (int i = 0; i < 4; ++i)
    {
        knobLabels_[i].setText(kKnobDefs[i].label, juce::dontSendNotification);
        knobLabels_[i].setFont(juce::Font(juce::FontOptions(8.5f)));
        knobLabels_[i].setColour(juce::Label::textColourId,
                                  juce::Colour(kTextDimColour));
        knobLabels_[i].setJustificationType(juce::Justification::centred);

        addAndMakeVisible(knobs_[i]);
        addAndMakeVisible(knobLabels_[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void GranularControlPanel::paint(juce::Graphics& g)
{
    // Panel background
    g.setColour(juce::Colour(kBgColour).withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Border
    g.setColour(juce::Colour(kBorderColour));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    const float h = static_cast<float>(getHeight());

    // Divider between left toggle section and knob section
    const float divX = 100.0f;
    g.setColour(juce::Colour(kBorderColour));
    g.drawLine(divX, 6.0f, divX, h - 6.0f, 0.5f);

    // Section labels
    drawSectionLabel(g, "GRANULAR",
                     juce::Rectangle<int>(0, 0, 100, 16));
    drawSectionLabel(g, "GRAIN PARAMETERS",
                     juce::Rectangle<int>(106, 0, getWidth() - 106, 16));
}

void GranularControlPanel::resized()
{
    const int h = getHeight();
    const int pad = 8;
    const int topOffset = 18; // space for section label

    // ── Left section: active toggle ───────────────────────────────────────────
    {
        const int btnH = 28;
        activeToggle_.setBounds(pad, topOffset + (h - topOffset - btnH) / 2,
                                 84, btnH);
    }

    // ── Center section: 4 knobs in a row ─────────────────────────────────────
    {
        const int knobAreaX = 100 + pad;
        const int knobAreaW = getWidth() - knobAreaX - pad;
        const int knobW     = 60;
        const int knobH     = 50;
        const int labelH    = 14;
        const int totalH    = knobH + labelH + 4;
        const int startY    = topOffset + (h - topOffset - totalH) / 2;

        // Distribute 4 knobs evenly across knobAreaW
        const int spacing = knobAreaW / 4;
        for (int i = 0; i < 4; ++i)
        {
            const int cx = knobAreaX + spacing * i + spacing / 2;
            knobs_[i].setBounds(cx - knobW / 2, startY, knobW, knobH);
            knobLabels_[i].setBounds(cx - knobW / 2, startY + knobH + 2,
                                      knobW, labelH);
        }
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void GranularControlPanel::setupToggleButton(juce::TextButton& btn)
{
    btn.setClickingTogglesState(true);
    btn.setColour(juce::TextButton::buttonColourId,   juce::Colour(kSurfaceColour));
    btn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(kAccentColour));
    btn.setColour(juce::TextButton::textColourOffId,  juce::Colour(kTextColour));
    btn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
}

void GranularControlPanel::setupKnob(juce::Slider& knob, double min, double max,
                                      double defaultVal, const juce::String& suffix)
{
    knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 14);
    knob.setRange(min, max, 0.001);
    knob.setValue(defaultVal, juce::dontSendNotification);
    knob.setTextValueSuffix(suffix);
    knob.setColour(juce::Slider::rotarySliderFillColourId,
                   juce::Colour(kAccentColour));
    knob.setColour(juce::Slider::rotarySliderOutlineColourId,
                   juce::Colour(kSurfaceColour));
    knob.setColour(juce::Slider::textBoxTextColourId,
                   juce::Colour(kTextColour));
    knob.setColour(juce::Slider::textBoxOutlineColourId,
                   juce::Colours::transparentBlack);
}

void GranularControlPanel::drawSectionLabel(juce::Graphics& g,
                                              const juce::String& text,
                                              juce::Rectangle<int> bounds) const
{
    g.setFont(juce::Font(juce::FontOptions(8.5f)));
    g.setColour(juce::Colour(kTextDimColour));
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace morphsnap
