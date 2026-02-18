/*
 * MorphSnap - UI/SnapFader.cpp
 */
#include "SnapFader.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

SnapFader::SnapFader(MorphSnapProcessor& processor)
    : proc_(processor)
{
    slider_.setSliderStyle(juce::Slider::LinearVertical);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setRange(0.0, 1.0, 0.001);
    slider_.setDoubleClickReturnValue(true, 0.0);
    slider_.setValue(proc_.faderPos.load(std::memory_order_relaxed), juce::dontSendNotification);

    slider_.onValueChange = [this]()
    {
        if (!suppressSliderCallback_)
            pushFaderValue(static_cast<float>(slider_.getValue()));
    };

    slider_.onDragStart = [this]()
    {
        proc_.morphSource.store(1, std::memory_order_relaxed);
    };

    title_.setText("SNAP", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    title_.setColour(juce::Label::textColourId, juce::Colour(0xffa0a0b0));

    valueLabel_.setText("0%", juce::dontSendNotification);
    valueLabel_.setJustificationType(juce::Justification::centred);
    valueLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffffffff));

    addAndMakeVisible(slider_);
    addAndMakeVisible(title_);
    addAndMakeVisible(valueLabel_);

    startTimerHz(24);
}

void SnapFader::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff16213e));
    g.fillRoundedRectangle(bounds, 8.0f);

    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
}

void SnapFader::resized()
{
    auto area = getLocalBounds().reduced(6);
    title_.setBounds(area.removeFromTop(18));
    valueLabel_.setBounds(area.removeFromBottom(18));
    slider_.setBounds(area.reduced(4, 2));
}

void SnapFader::timerCallback()
{
    const float faderValue = proc_.faderPos.load(std::memory_order_relaxed);
    if (!slider_.isMouseButtonDown())
    {
        suppressSliderCallback_ = true;
        slider_.setValue(faderValue, juce::dontSendNotification);
        suppressSliderCallback_ = false;
    }

    valueLabel_.setText(juce::String(faderValue * 100.0f, 0) + "%",
                        juce::dontSendNotification);
}

void SnapFader::pushFaderValue(float normalizedValue)
{
    const auto clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);
    proc_.faderPos.store(clamped, std::memory_order_relaxed);
    proc_.morphSource.store(1, std::memory_order_relaxed);

    if (auto* faderParam = proc_.getAPVTS().getParameter("faderPos"))
        faderParam->setValueNotifyingHost(clamped);
}

} // namespace morphsnap
