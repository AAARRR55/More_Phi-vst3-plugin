/* More-Phi — UI/ModeBar.cpp */
#include "ModeBar.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

ModeBar::ModeBar(MorePhiProcessor& p) : proc_(p)
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
    smoothSlider_.onDragStart = [this]() {
        if (!smoothingGestureActive_)
        {
            if (auto* p = proc_.getAPVTS().getParameter("smoothing"))
                p->beginChangeGesture();
            smoothingGestureActive_ = true;
        }
    };
    smoothSlider_.onDragEnd = [this]() {
        if (smoothingGestureActive_)
        {
            if (auto* p = proc_.getAPVTS().getParameter("smoothing"))
                p->endChangeGesture();
            smoothingGestureActive_ = false;
        }
    };
    smoothSlider_.onValueChange = [this]() {
        // Route through APVTS for DAW automation support.
        if (auto* p = proc_.getAPVTS().getParameter("smoothing"))
        {
            const auto value = static_cast<float>(smoothSlider_.getValue());
            if (smoothingGestureActive_)
                p->setValueNotifyingHost(value);
            else
                ParameterBinding::setValueWithGesture(*p, value);
        }
    };
    addAndMakeVisible(smoothSlider_);

    smoothLabel_.setText("Smooth", juce::dontSendNotification);
    smoothLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    smoothLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(smoothLabel_);
}

void ModeBar::resized()
{
    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;
    fb.alignItems = juce::FlexBox::AlignItems::center;

    for (auto& btn : modeButtons_)
    {
        fb.items.add(juce::FlexItem(btn).withWidth(70.0f).withMargin(2));
    }

    fb.items.add(juce::FlexItem().withWidth(16)); // spacer
    fb.items.add(juce::FlexItem(smoothLabel_).withWidth(50.0f).withMargin(2));
    fb.items.add(juce::FlexItem(smoothSlider_).withWidth(160.0f).withMargin(2));
    fb.items.add(juce::FlexItem().withFlex(1)); // absorb remaining space

    fb.performLayout(getLocalBounds().reduced(4, 2));
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
            // Route through APVTS for DAW automation support.
            if (auto* p = proc_.getAPVTS().getParameter("physicsMode"))
                ParameterBinding::setValueWithGesture(*p, static_cast<float>(i) / 2.0f);
            break;
        }
    }
}

} // namespace more_phi
