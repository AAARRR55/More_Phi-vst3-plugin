/*
 * More-Phi — UI/DriftControlPanel.cpp
 *
 * Implements the Drift physics-mode control strip. The three knobs are bound
 * to the driftSpeed / driftDistance / driftChaos APVTS params, which the
 * physics engine consumes when physicsMode == Drift. Previously these params
 * had no GUI surface (H1): selecting Drift in the ModeBar left the user with
 * only the defaults and no way to shape the drift.
 */
#include "DriftControlPanel.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"
#include "UI/MorePhiLookAndFeel.h"

namespace more_phi {

using namespace Theme::Colours;

namespace {
struct DriftKnobDef
{
    const char* label;
    double      min;
    double      max;
    double      defaultVal;
    const char* suffix;
    const char* apvtsId;
};

constexpr std::array<DriftKnobDef, 3> kKnobDefs
{{
    { "Speed",    0.01, 2.0, 0.3, "", "driftSpeed"    },
    { "Distance", 0.0,  1.0, 0.4, "", "driftDistance" },
    { "Chaos",    0.0,  1.0, 0.5, "", "driftChaos"    }
}};
} // namespace

DriftControlPanel::DriftControlPanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    auto& apvts = proc_.getAPVTS();

    static constexpr const char* knobParamIds[] = {
        "driftSpeed", "driftDistance", "driftChaos"
    };

    for (int i = 0; i < 3; ++i)
    {
        setupKnob(knobs_[i], kKnobDefs[i].min, kKnobDefs[i].max,
                  kKnobDefs[i].defaultVal, kKnobDefs[i].suffix);
        ParameterBinding::bindSlider(knobs_[i], apvts, knobParamIds[i]);
    }

    for (int i = 0; i < 3; ++i)
    {
        knobLabels_[i].setText(kKnobDefs[i].label, juce::dontSendNotification);
        knobLabels_[i].setFont(MorePhiLookAndFeel::bodyFont(10.0f));
        knobLabels_[i].setColour(juce::Label::textColourId, textDim());
        knobLabels_[i].setJustificationType(juce::Justification::centred);

        addAndMakeVisible(knobs_[i]);
        addAndMakeVisible(knobLabels_[i]);
    }
}

void DriftControlPanel::paint(juce::Graphics& g)
{
    // Panel background — matches Spectral/Granular panels.
    g.setColour(surface().withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    drawSectionLabel(g, "DRIFT",
                     juce::Rectangle<int>(0, 0, 100, 16));
}

void DriftControlPanel::resized()
{
    const int h = getHeight();
    const int pad = 8;
    const int topOffset = 18;   // space for the section label
    const int leftSectionW = 100 + pad;
    const int labelH = 14;

    const int knobAreaX = leftSectionW;
    const int knobAreaW = getWidth() - knobAreaX - pad;
    const int knobW = 60;
    const int knobH = juce::jmax(20, juce::jmin(46, h - topOffset - labelH - 4));
    const int startY = topOffset + juce::jmax(0, (h - topOffset - knobH - labelH - 4) / 2);

    juce::FlexBox knobRow;
    knobRow.flexDirection = juce::FlexBox::Direction::row;
    for (int i = 0; i < 3; ++i)
        knobRow.items.add(juce::FlexItem().withFlex(1));
    knobRow.performLayout(juce::Rectangle<int>(knobAreaX, 0, knobAreaW, h));

    const float colW = static_cast<float>(knobAreaW) / 3.0f;
    for (int i = 0; i < 3; ++i)
    {
        const int cx = knobAreaX + static_cast<int>(colW * i + colW / 2.0f);
        knobs_[i].setBounds(cx - knobW / 2, startY, knobW, knobH);
        knobLabels_[i].setBounds(cx - knobW / 2, startY + knobH + 2, knobW, labelH);
    }
}

void DriftControlPanel::setupKnob(juce::Slider& knob, double min, double max,
                                  double defaultVal, const juce::String& suffix)
{
    knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 14);
    knob.setRange(min, max, 0.001);
    knob.setValue(defaultVal, juce::dontSendNotification);
    knob.setTextValueSuffix(suffix);
    knob.setColour(juce::Slider::rotarySliderFillColourId, accent());
    knob.setColour(juce::Slider::rotarySliderOutlineColourId, surfaceLit());
    knob.setColour(juce::Slider::textBoxTextColourId, textBright());
    knob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void DriftControlPanel::drawSectionLabel(juce::Graphics& g,
                                         const juce::String& text,
                                         juce::Rectangle<int> bounds) const
{
    g.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
