/* MorphSnap — UI/ModeBar.cpp */
#include "ModeBar.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

ModeBar::ModeBar(MorphSnapProcessor& p) : proc_(p)
{
    const juce::StringArray labels = {"Direct", "Elastic", "Drift"};
    for (int i = 0; i < 3; ++i)
    {
        modeButtons_[i].setButtonText(labels[i]);
        modeButtons_[i].setRadioGroupId(1001);
        modeButtons_[i].setClickingTogglesState(true);
        modeButtons_[i].addListener(this);
        addAndMakeVisible(modeButtons_[i]);
    }
    modeButtons_[0].setToggleState(true, juce::dontSendNotification);

    smoothSlider_.setRange(0.0, 0.999, 0.001);
    smoothSlider_.setValue(0.95);
    smoothSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    smoothSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
    smoothSlider_.onValueChange = [this]() {
        proc_.smoothingRate.store(static_cast<float>(smoothSlider_.getValue()),
                                  std::memory_order_relaxed);
    };
    addAndMakeVisible(smoothSlider_);

    smoothLabel_.setText("Smooth", juce::dontSendNotification);
    smoothLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    smoothLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(smoothLabel_);
}

void ModeBar::resized()
{
    auto b = getLocalBounds().reduced(4, 2);
    for (auto& btn : modeButtons_)
    {
        btn.setBounds(b.removeFromLeft(70));
        b.removeFromLeft(4);
    }
    b.removeFromLeft(16);
    smoothLabel_.setBounds(b.removeFromLeft(50));
    b.removeFromLeft(4);
    smoothSlider_.setBounds(b.removeFromLeft(juce::jmin(b.getWidth(), 160)));
}

void ModeBar::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff16213e));
    g.fillRect(getLocalBounds());
}

void ModeBar::buttonClicked(juce::Button*)
{
    updateSelection();
}

void ModeBar::updateSelection()
{
    for (int i = 0; i < 3; ++i)
    {
        if (modeButtons_[i].getToggleState())
        {
            currentMode_ = i;
            proc_.physicsMode.store(i, std::memory_order_relaxed);
            break;
        }
    }
}

} // namespace morphsnap
