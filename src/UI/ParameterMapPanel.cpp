/* MorphSnap — UI/ParameterMapPanel.cpp */
#include "ParameterMapPanel.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

// ── ParameterRow ─────────────────────────────────────────────────────────────

ParameterRow::ParameterRow(int paramIndex, MorphSnapProcessor& proc)
    : index_(paramIndex), proc_(proc)
{
    morphToggle_.setToggleState(true, juce::dontSendNotification);
    morphToggle_.setTooltip("Include in morphing");
    addAndMakeVisible(morphToggle_);

    auto& bridge = proc_.getParameterBridge();
    nameLabel_.setText(bridge.getParameterName(index_), juce::dontSendNotification);
    nameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe8eaed));
    nameLabel_.setFont(juce::Font(11.0f));
    addAndMakeVisible(nameLabel_);

    slider_.setRange(0.0, 1.0, 0.001);
    slider_.setValue(bridge.getParameterNormalized(index_), juce::dontSendNotification);
    slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    slider_.onValueChange = [this]()
    {
        if (!syncing_)
            proc_.getParameterBridge().setParameterNormalized(
                index_, static_cast<float>(slider_.getValue()));
    };
    addAndMakeVisible(slider_);

    valueLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b95a5));
    valueLabel_.setFont(juce::Font(10.0f));
    valueLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(valueLabel_);

    refresh();
}

void ParameterRow::resized()
{
    auto b = getLocalBounds().reduced(2, 0);
    morphToggle_.setBounds(b.removeFromLeft(24));
    b.removeFromLeft(2);
    nameLabel_.setBounds(b.removeFromLeft(140));
    valueLabel_.setBounds(b.removeFromRight(45));
    b.removeFromRight(4);
    slider_.setBounds(b);
}

void ParameterRow::refresh()
{
    auto& bridge = proc_.getParameterBridge();
    if (index_ >= bridge.getParameterCount()) return;

    syncing_ = true;
    float val = bridge.getParameterNormalized(index_);
    slider_.setValue(val, juce::dontSendNotification);
    valueLabel_.setText(juce::String(val, 3), juce::dontSendNotification);
    syncing_ = false;
}

// ── ParameterMapPanel ────────────────────────────────────────────────────────

ParameterMapPanel::ParameterMapPanel(MorphSnapProcessor& proc) : proc_(proc)
{
    headerLabel_.setText("Parameter Mapping", juce::dontSendNotification);
    headerLabel_.setFont(juce::Font(13.0f, juce::Font::bold));
    headerLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffec415d));
    addAndMakeVisible(headerLabel_);

    selectAllBtn_.onClick = [this]()
    {
        for (auto& row : rows_)
            row->findChildWithID("morphToggle");
        // Simple: just iterate and check all
        for (auto& r : rows_)
        {
            if (auto* toggle = dynamic_cast<juce::ToggleButton*>(r->getChildComponent(0)))
                toggle->setToggleState(true, juce::dontSendNotification);
        }
    };
    selectNoneBtn_.onClick = [this]()
    {
        for (auto& r : rows_)
        {
            if (auto* toggle = dynamic_cast<juce::ToggleButton*>(r->getChildComponent(0)))
                toggle->setToggleState(false, juce::dontSendNotification);
        }
    };
    addAndMakeVisible(selectAllBtn_);
    addAndMakeVisible(selectNoneBtn_);

    viewport_.setViewedComponent(&rowContainer_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    startTimerHz(5);  // Refresh values 5x/sec
}

void ParameterMapPanel::resized()
{
    auto b = getLocalBounds();
    auto headerRow = b.removeFromTop(28);
    headerLabel_.setBounds(headerRow.removeFromLeft(150));
    selectNoneBtn_.setBounds(headerRow.removeFromRight(45));
    headerRow.removeFromRight(4);
    selectAllBtn_.setBounds(headerRow.removeFromRight(35));

    viewport_.setBounds(b);

    // Size the container to fit all rows
    const int rowH = 24;
    rowContainer_.setBounds(0, 0, b.getWidth() - 14, rowH * static_cast<int>(rows_.size()));
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        rows_[i]->setBounds(0, i * rowH, rowContainer_.getWidth(), rowH);
}

void ParameterMapPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff0d1b2a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(juce::Colour(0xff1e3a5f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
}

void ParameterMapPanel::timerCallback()
{
    auto& bridge = proc_.getParameterBridge();
    int count = bridge.getParameterCount();

    // Rebuild if plugin changed
    if (count != lastParamCount_)
    {
        rebuildForPlugin();
        lastParamCount_ = count;
        return;
    }

    // Refresh values
    for (auto& row : rows_)
        row->refresh();
}

void ParameterMapPanel::rebuildForPlugin()
{
    rows_.clear();
    rowContainer_.removeAllChildren();

    auto& bridge = proc_.getParameterBridge();
    int count = bridge.getParameterCount();

    for (int i = 0; i < count; ++i)
    {
        auto row = std::make_unique<ParameterRow>(i, proc_);
        rowContainer_.addAndMakeVisible(row.get());
        rows_.push_back(std::move(row));
    }

    resized();
}

std::set<int> ParameterMapPanel::getMorphEnabledParams() const
{
    std::set<int> enabled;
    for (const auto& row : rows_)
    {
        if (row->isMorphEnabled())
            enabled.insert(row->getParamIndex());
    }
    return enabled;
}

} // namespace morphsnap
