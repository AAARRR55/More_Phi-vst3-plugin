/*
 * More-Phi — UI/GranularControlPanel.cpp
 *
 * Implements the granular engine control strip.
 * All controls route through APVTS for DAW automation support.
 * syncStateFromAPVTS() bridges APVTS → engine setters on the audio thread.
 */
#include "GranularControlPanel.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

using namespace Theme::Colours;

    // Knob metadata
    struct KnobDef
    {
        const char* label;
        double      min;
        double      max;
        double      defaultVal;
        const char* suffix;
        const char* apvtsId;  // APVTS parameter ID
    };

    constexpr std::array<KnobDef, 4> kKnobDefs
    {{
        { "Size",    20.0,  200.0, 50.0,  " ms",  "grainSize"    },
        { "Density",  5.0,  100.0, 20.0,  " g/s", "grainDensity" },
        { "Pitch",    0.0,    2.0,  0.0,  " st",  "grainPitch"   },
        { "Scatter",  0.0,    1.0,  0.0,  "",     "grainScatter" }
    }};

// ─────────────────────────────────────────────────────────────────────────────

GranularControlPanel::GranularControlPanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    auto& apvts = proc_.getAPVTS();
    auto& engine = proc_.getGranularEngine();

    // ── Active toggle ─────────────────────────────────────────────────────────
    setupToggleButton(activeToggle_);
    activeToggle_.setToggleState(engine.isActive(), juce::dontSendNotification);
    ParameterBinding::bindToggleButton(activeToggle_, apvts, "granularActive");
    addAndMakeVisible(activeToggle_);

    // ── Knobs: route through APVTS ─────────────────────────────────────────────
    static constexpr const char* knobParamIds[] = {
        "grainSize", "grainDensity", "grainPitch", "grainScatter"
    };

    for (int i = 0; i < 4; ++i)
    {
        setupKnob(knobs_[i], kKnobDefs[i].min, kKnobDefs[i].max,
                   kKnobDefs[i].defaultVal, kKnobDefs[i].suffix);
        ParameterBinding::bindSlider(knobs_[i], apvts, knobParamIds[i]);
    }

    // ── Labels ────────────────────────────────────────────────────────────────
    for (int i = 0; i < 4; ++i)
    {
        knobLabels_[i].setText(kKnobDefs[i].label, juce::dontSendNotification);
        knobLabels_[i].setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
        knobLabels_[i].setColour(juce::Label::textColourId,
                                  textDim());
        knobLabels_[i].setJustificationType(juce::Justification::centred);

        addAndMakeVisible(knobs_[i]);
        addAndMakeVisible(knobLabels_[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void GranularControlPanel::paint(juce::Graphics& g)
{
    // Panel background
    g.setColour(surface().withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Border
    g.setColour(border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    const float h = static_cast<float>(getHeight());

    // Divider between left toggle section and knob section
    const float divX = 100.0f;
    g.setColour(border());
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

    // ── Center section: 4 knobs in a FlexBox row ──────────────────────────────
    {
        const int knobAreaX = 100 + pad;
        const int knobAreaW = getWidth() - knobAreaX - pad;
        const int knobW     = 60;
        const int knobH     = 50;
        const int labelH    = 14;
        const int totalH    = knobH + labelH + 4;
        const int startY    = topOffset + (h - topOffset - totalH) / 2;

        // FlexBox row with equal-width columns for each knob
        juce::FlexBox knobRow;
        knobRow.flexDirection = juce::FlexBox::Direction::row;

        for (int i = 0; i < 4; ++i)
            knobRow.items.add(juce::FlexItem().withFlex(1));
        knobRow.performLayout(juce::Rectangle<int>(knobAreaX, 0, knobAreaW, h));

        // Position knobs centered within each FlexBox column
        const float colW = static_cast<float>(knobAreaW) / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            const int cx = knobAreaX + static_cast<int>(colW * i + colW / 2.0f);
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
    btn.setColour(juce::TextButton::buttonColourId,   surfaceLit());
    btn.setColour(juce::TextButton::buttonOnColourId, accent());
    btn.setColour(juce::TextButton::textColourOffId,  textBright());
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
                   accent());
    knob.setColour(juce::Slider::rotarySliderOutlineColourId,
                   surfaceLit());
    knob.setColour(juce::Slider::textBoxTextColourId,
                   textBright());
    knob.setColour(juce::Slider::textBoxOutlineColourId,
                   juce::Colours::transparentBlack);
}

void GranularControlPanel::drawSectionLabel(juce::Graphics& g,
                                              const juce::String& text,
                                              juce::Rectangle<int> bounds) const
{
    g.setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
