/* More-Phi — UI/MacroKnobStrip.cpp */
#include "MacroKnobStrip.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

// M-7 FIX: Macro knobs are currently hardcoded to the first 8 hosted parameters (0–7).
// TODO: Implement user-configurable mapping so any parameter can be assigned to any knob.
MacroKnobStrip::MacroKnobStrip(MorePhiProcessor& p) : proc_(p)
{
    for (int i = 0; i < 8; ++i)
    {
        knobs_[i].setSliderStyle(juce::Slider::RotaryVerticalDrag);
        knobs_[i].setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        knobs_[i].setRange(0.0, 1.0, 0.001);
        knobs_[i].setValue(0.5);
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

        labels_[i].setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
        labels_[i].setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
        labels_[i].setJustificationType(juce::Justification::centred);
        labels_[i].setText("P" + juce::String(i + 1), juce::dontSendNotification);
        addAndMakeVisible(labels_[i]);
    }

    startTimerHz(10);  // Sync knobs 10x/sec
}

void MacroKnobStrip::resized()
{
    auto b = getLocalBounds();
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
    g.setColour(juce::Colour(0xff0d0d10));
    g.fillRect(getLocalBounds());
    g.setColour(juce::Colour(0xff0f3460));
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
            knobs_[i].setEnabled(true);
        }
        else
        {
            knobs_[i].setEnabled(false);
            labels_[i].setText("-", juce::dontSendNotification);
        }
    }
    syncing_ = false;
}

} // namespace more_phi
