/* More-Phi — UI/ParameterMapPanel.cpp */
#include "ParameterMapPanel.h"
#include "Plugin/PluginProcessor.h"
#include "MorePhiLookAndFeel.h"

namespace more_phi {

// ── ParameterRow ─────────────────────────────────────────────────────────────

ParameterRow::ParameterRow(int paramIndex, MorePhiProcessor& proc)
    : index_(paramIndex), proc_(proc)
{
    auto& bridge = proc_.getParameterBridge();
    nameLabel_.setText(bridge.getParameterName(index_), juce::dontSendNotification);
    nameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffeeeef2));
    nameLabel_.setFont(MorePhiLookAndFeel::bodyFont(11.0f));
    addAndMakeVisible(nameLabel_);

    slider_.setRange(0.0, 1.0, 0.001);
    slider_.setValue(bridge.getParameterNormalized(index_), juce::dontSendNotification);
    slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    slider_.onValueChange = [this]()
    {
        if (!syncing_)
        {
            const bool success = proc_.enqueueParameterSet(
                index_, static_cast<float>(slider_.getValue()));

            // Warn if command queue is full (parameter change dropped)
            if (!success)
            {
                DBG("MorePhi: WARNING - Command queue overflow, parameter " +
                    juce::String(index_) + " change dropped");
            }
        }
    };
    addAndMakeVisible(slider_);

    valueLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    valueLabel_.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    valueLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(valueLabel_);

    refresh();
}

void ParameterRow::resized()
{
    auto b = getLocalBounds().reduced(2, 0);
    const int valueW = juce::jlimit(42, 58, b.getWidth() / 5);
    const int nameW = juce::jlimit(84, 170, b.getWidth() / 2);
    nameLabel_.setBounds(b.removeFromLeft(nameW));
    valueLabel_.setBounds(b.removeFromRight(valueW));
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

ParameterMapPanel::ParameterMapPanel(MorePhiProcessor& proc) : proc_(proc)
{
    headerLabel_.setText("All Parameters", juce::dontSendNotification);
    headerLabel_.setFont(MorePhiLookAndFeel::bodyFont(13.0f, juce::Font::bold));
    headerLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe5c057));
    addAndMakeVisible(headerLabel_);

    searchField_.setTextToShowWhenEmpty("Filter parameters…", juce::Colour(0xff6e6e76));
    searchField_.setFont(MorePhiLookAndFeel::bodyFont(11.0f));
    searchField_.setColour(juce::TextEditor::textColourId, juce::Colour(0xffeeeef2));
    searchField_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff17181c));
    searchField_.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff323237));
    searchField_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xffe5c057));
    searchField_.addListener(this);
    addAndMakeVisible(searchField_);

    viewport_.setViewedComponent(&rowContainer_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    startTimerHz(5);  // Refresh values 5x/sec
}

void ParameterMapPanel::resized()
{
    auto b = getLocalBounds();
    auto headerRow = b.removeFromTop(28);
    headerLabel_.setBounds(headerRow.removeFromLeft(130));
    headerRow.removeFromLeft(8);
    searchField_.setBounds(headerRow.withHeight(22));

    viewport_.setBounds(b);

    // Count visible rows and reposition them stacked without gaps
    const int rowH = 24;
    int visibleCount = 0;
    int containerWidth = b.getWidth() - 14;
    for (auto& row : rows_)
    {
        if (row->isVisible())
        {
            row->setBounds(0, visibleCount * rowH, containerWidth, rowH);
            ++visibleCount;
        }
    }
    rowContainer_.setBounds(0, 0, containerWidth, rowH * visibleCount);
}

void ParameterMapPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff070709));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(juce::Colour(0xff323237));
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

    // Refresh only visible rows plus small overscan; large hosted plugins can
    // expose thousands of parameters and refreshing all rows every tick stalls UI.
    constexpr int rowH = 24;
    const auto visible = viewport_.getViewArea();
    const int first = juce::jlimit(0, static_cast<int>(rows_.size()),
                                   visible.getY() / rowH - 3);
    const int last = juce::jlimit(0, static_cast<int>(rows_.size()),
                                  (visible.getBottom() / rowH) + 4);
    for (int i = first; i < last; ++i)
        rows_[static_cast<size_t>(i)]->refresh();
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

    applyFilter();
    resized();
}

void ParameterMapPanel::textEditorTextChanged(juce::TextEditor&)
{
    filterText_ = searchField_.getText().toLowerCase().trim();
    applyFilter();
}

void ParameterMapPanel::applyFilter()
{
    auto& bridge = proc_.getParameterBridge();
    const bool filtering = filterText_.isNotEmpty();

    for (auto& row : rows_)
    {
        if (filtering)
        {
            juce::String name = bridge.getParameterName(row->getParamIndex());
            bool matches = name.toLowerCase().contains(filterText_);
            row->setVisible(matches);
        }
        else
        {
            row->setVisible(true);
        }
    }

    resized();
}

} // namespace more_phi
