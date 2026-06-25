/* More-Phi — UI/MacroKnobStrip.cpp */
#include "MacroKnobStrip.h"
#include "Plugin/PluginProcessor.h"
#include "MorePhiLookAndFeel.h"
#include "UI/Theme/MorePhiTheme.h"

namespace more_phi {

// Macro knobs are currently hardcoded to the first 8 hosted parameters (0–7).
// H7: value readout (TextBoxBelow) + per-knob tooltips added. TODO (still pending,
//     larger feature): user-configurable mapping so any parameter can be assigned to
//     any knob — see docs/GUI_QA_REPORT.md (H7).
MacroKnobStrip::MacroKnobStrip(MorePhiProcessor& p) : proc_(p)
{
    for (int i = 0; i < 8; ++i)
    {
        knobs_[i].setSliderStyle(juce::Slider::RotaryVerticalDrag);
        knobs_[i].setTextBoxStyle(juce::Slider::TextBoxBelow, false, 44, 12);  // H7: value readout
        knobs_[i].setRange(0.0, 1.0, 0.001);
        knobs_[i].setValue(0.5);
        // Visual distinction: macro knobs use cyan fill instead of gold
        knobs_[i].setColour(juce::Slider::rotarySliderFillColourId,
                             Theme::Colours::cyan());
        knobs_[i].setColour(juce::Slider::thumbColourId,
                             Theme::Colours::cyan());
        knobs_[i].setTooltip("Macro " + juce::String(i + 1) + ": load a plugin to assign its first 8 parameters here.");
        knobs_[i].onValueChange = [this, i]()
        {
            if (!syncing_)
            {
                const bool success = proc_.enqueueParameterSet(
                    i, static_cast<float>(knobs_[i].getValue()));

                // Warn if command queue is full (parameter change dropped)
                if (!success)
                {
                    DBG("MorePhi: WARNING - Command queue overflow, macro knob " +
                        juce::String(i) + " change dropped");
                }
            }
        };
        addAndMakeVisible(knobs_[i]);

        labels_[i].setFont(MorePhiLookAndFeel::bodyFont(10.0f));
        labels_[i].setColour(juce::Label::textColourId, Theme::Colours::textDim());
        labels_[i].setJustificationType(juce::Justification::centred);
        labels_[i].setText("--", juce::dontSendNotification);
        addAndMakeVisible(labels_[i]);
    }

    startTimerHz(10);  // Sync knobs 10x/sec
}

void MacroKnobStrip::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(16);  // Space for "MACRO CONTROLS" section label
    int w = b.getWidth() / 8;
    for (int i = 0; i < 8; ++i)
    {
        auto col = b.removeFromLeft(w);
        labels_[i].setBounds(col.removeFromBottom(18));
        knobs_[i].setBounds(col.reduced(4));
    }
}

void MacroKnobStrip::paint(juce::Graphics& g)
{
    g.setColour(Theme::Colours::surface());
    g.fillRect(getLocalBounds());

    // Section label — "MACRO CONTROLS" in all-caps dim text (matches engine panel convention)
    g.setColour(Theme::Colours::textDim());
    g.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    g.drawText("MACRO CONTROLS", 8, 0, 140, 14, juce::Justification::centredLeft);

    g.setColour(Theme::Colours::border());
    g.drawLine(0, 0, static_cast<float>(getWidth()), 0, 0.5f);
}

void MacroKnobStrip::timerCallback()
{
    syncKnobsToPlugin();
}

void MacroKnobStrip::syncKnobsToPlugin()
{
    auto& bridge = proc_.getParameterBridge();
    int count = juce::jmin(8, bridge.getParameterCount());

    syncing_ = true;
    for (int i = 0; i < 8; ++i)
    {
        if (i < count)
        {
            knobs_[i].setValue(bridge.getParameterNormalized(i),
                               juce::dontSendNotification);
            labels_[i].setText(bridge.getParameterName(i),
                                juce::dontSendNotification);
            knobs_[i].setTooltip(bridge.getParameterName(i) +
                "  (macro " + juce::String(i + 1) + " of 8 - macros map to hosted params 1-8)");
            knobs_[i].setEnabled(true);
        }
        else
        {
            knobs_[i].setEnabled(false);
            knobs_[i].setTooltip("No hosted parameter assigned to this macro");
            labels_[i].setText("--", juce::dontSendNotification);
        }
    }
    syncing_ = false;
}

} // namespace more_phi
