/* More-Phi — UI/ModeBar.cpp
 * V2: Separates Morph Source (2D Pad | Fader) from Physics Mode (Direct | Elastic | Drift).
 * All changes route through APVTS for DAW automation support.
 */
#include "ModeBar.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Bindings/ParameterBinding.h"
#include "UI/MorePhiLookAndFeel.h"
#include <cmath>

namespace more_phi {

ModeBar::ModeBar(MorePhiProcessor& p) : proc_(p)
{
    // ── Morph Source buttons: 2D Pad | Fader ────────────────────────────────
    sourceLabel_.setText("Source", juce::dontSendNotification);
    sourceLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    sourceLabel_.setFont(MorePhiLookAndFeel::bodyFont(MorePhiLookAndFeel::kMinControlLabel));
    sourceLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(sourceLabel_);

    const juce::StringArray sourceLabels = {"2D Pad", "Fader"};
    for (int i = 0; i < 2; ++i)
    {
        sourceButtons_[i].setButtonText(sourceLabels[i]);
        sourceButtons_[i].setComponentID(i == 0 ? "modebar.source.pad" : "modebar.source.fader");
        sourceButtons_[i].setRadioGroupId(1002);
        sourceButtons_[i].setClickingTogglesState(true);
        sourceButtons_[i].addListener(this);
        addAndMakeVisible(sourceButtons_[i]);
    }
    sourceButtons_[0].setToggleState(true, juce::dontSendNotification);
    sourceButtons_[0].setTooltip(
        "2D Pad: morph by dragging the cursor on the XY pad between snapshot positions "
        "arranged around the clock face.");
    sourceButtons_[1].setTooltip(
        "Fader: morph along a single axis using the vertical slider, interpolating "
        "between occupied snapshots in clock order.");

    // ── Physics mode buttons: Direct | Elastic | Drift ──────────────────────
    const juce::StringArray modeLabels = {"Direct", "Elastic", "Drift"};
    for (int i = 0; i < 3; ++i)
    {
        modeButtons_[i].setButtonText(modeLabels[i]);
        modeButtons_[i].setComponentID("modebar.physics." + modeLabels[i].toLowerCase());
        modeButtons_[i].setRadioGroupId(1001);
        modeButtons_[i].setClickingTogglesState(true);
        modeButtons_[i].addListener(this);
        addAndMakeVisible(modeButtons_[i]);
    }
    modeButtons_[0].setToggleState(true, juce::dontSendNotification);
    modeButtons_[0].setTooltip(
        "Direct mode: cursor position drives morph instantly with no physics simulation. "
        "Parameters update at the raw cursor position.");
    modeButtons_[1].setTooltip(
        "Elastic mode: spring-physics cursor with momentum and inertia. "
        "Feels like a weighted object pulled toward your target.");
    modeButtons_[2].setTooltip(
        "Drift mode: Perlin-noise wandering around the target position. "
        "Adjust speed, distance, and chaos on the Engine tab for evolving, organic movement.");

    modeLabel_.setText("Mode", juce::dontSendNotification);
    modeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    modeLabel_.setFont(MorePhiLookAndFeel::bodyFont(MorePhiLookAndFeel::kMinControlLabel));
    modeLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(modeLabel_);

    // ── Smoothing slider ────────────────────────────────────────────────────
    smoothSlider_.setRange(0.0, 0.999, 0.001);
    smoothSlider_.setValue(0.95);
    smoothSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    smoothSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
    smoothSlider_.setTooltip(
        "Smoothing: blends morph output over time. 0 = instant jumps between positions, "
        "higher values = gradual, gliding transitions. "
        "Maximum is 0.999 (not 1.0) to prevent numerical instability in the filter.");
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
        if (syncing_)
            return;

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
    smoothLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    smoothLabel_.setFont(MorePhiLookAndFeel::bodyFont(MorePhiLookAndFeel::kMinControlLabel));
    smoothLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(smoothLabel_);

    // R1b: Elastic preset combo (Slow / Medium / Heavy). Indices match the
    // PhysicsEngine::ElasticPreset enum (0/1/2). The processor stores the value
    // in an atomic applied every block, so onChange writes straight to it — no
    // new APVTS parameter (which would bump the state version). Default Medium.
    elasticPresetCombo_.addItem("Slow",   1);
    elasticPresetCombo_.addItem("Medium", 2);
    elasticPresetCombo_.addItem("Heavy",  3);
    elasticPresetCombo_.setSelectedItemIndex(juce::jlimit(0, 2, proc_.getElasticPreset()),
                                             juce::dontSendNotification);
    elasticPresetCombo_.setTooltip(
        "Elastic preset: spring stiffness and damping. Slow = loose, gliding "
        "transitions; Medium = balanced; Heavy = tight, snappy response. Only "
        "applies while Elastic physics mode is active.");
    elasticPresetCombo_.onChange = [this]()
    {
        if (elasticPresetSyncing_)
            return;
        const int idx = elasticPresetCombo_.getSelectedItemIndex();
        if (idx >= 0 && idx < 3)
            proc_.setElasticPreset(idx);
    };
    addAndMakeVisible(elasticPresetCombo_);

    elasticLabel_.setText("Preset", juce::dontSendNotification);
    elasticLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    elasticLabel_.setFont(MorePhiLookAndFeel::bodyFont(MorePhiLookAndFeel::kMinControlLabel));
    elasticLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(elasticLabel_);

    syncButtonsToState();
    syncSmoothingToState();
    startTimerHz(10);
}

ModeBar::~ModeBar()
{
    stopTimer();
}

void ModeBar::resized()
{
    auto area = getLocalBounds().reduced(8, 6);
    const bool compact = area.getWidth() < 660;

    const auto addFixed = [](juce::FlexBox& fb, juce::Component& component,
                             float width, float height, float margin = 1.0f)
    {
        fb.items.add(juce::FlexItem(component).withWidth(width).withHeight(height).withMargin(margin));
    };

    if (compact)
    {
        auto topRow = area.removeFromTop(28);
        area.removeFromTop(4);
        const float topH = static_cast<float>(topRow.getHeight());

        juce::FlexBox selectors;
        selectors.flexDirection = juce::FlexBox::Direction::row;
        selectors.alignItems = juce::FlexBox::AlignItems::center;
        addFixed(selectors, sourceLabel_, 52.0f, topH);
        for (auto& btn : sourceButtons_)
            addFixed(selectors, btn, 62.0f, topH);
        selectors.items.add(juce::FlexItem().withWidth(8.0f));
        addFixed(selectors, modeLabel_, 42.0f, topH);
        for (auto& btn : modeButtons_)
            addFixed(selectors, btn, 62.0f, topH);
        selectors.items.add(juce::FlexItem().withWidth(8.0f));
        // R1b: Elastic preset on the compact top row too (hidden unless Elastic).
        addFixed(selectors, elasticLabel_, 46.0f, topH);
        selectors.items.add(juce::FlexItem(elasticPresetCombo_).withWidth(88.0f).withHeight(topH).withMargin(1));
        selectors.items.add(juce::FlexItem().withFlex(1));
        selectors.performLayout(topRow);

        juce::FlexBox smoothing;
        smoothing.flexDirection = juce::FlexBox::Direction::row;
        smoothing.alignItems = juce::FlexBox::AlignItems::center;
        addFixed(smoothing, smoothLabel_, 58.0f, static_cast<float>(area.getHeight()));
        smoothing.items.add(juce::FlexItem(smoothSlider_).withFlex(1).withHeight(static_cast<float>(area.getHeight())).withMargin(1));
        smoothing.performLayout(area);
        return;
    }

    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;
    fb.alignItems = juce::FlexBox::AlignItems::center;
    const float rowH = static_cast<float>(area.getHeight());

    addFixed(fb, sourceLabel_, 52.0f, rowH);
    for (auto& btn : sourceButtons_)
        addFixed(fb, btn, 66.0f, rowH);
    fb.items.add(juce::FlexItem().withWidth(12.0f));
    addFixed(fb, modeLabel_, 44.0f, rowH);
    for (auto& btn : modeButtons_)
        addFixed(fb, btn, 66.0f, rowH);
    fb.items.add(juce::FlexItem().withWidth(12.0f));
    addFixed(fb, smoothLabel_, 56.0f, rowH);
    fb.items.add(juce::FlexItem(smoothSlider_).withWidth(150.0f).withHeight(rowH).withMargin(1));

    // R1b: Elastic preset (only visible while Elastic is active).
    fb.items.add(juce::FlexItem().withWidth(12.0f));
    addFixed(fb, elasticLabel_, 52.0f, rowH);
    fb.items.add(juce::FlexItem(elasticPresetCombo_).withWidth(96.0f).withHeight(rowH).withMargin(1));

    fb.items.add(juce::FlexItem().withFlex(1));
    fb.performLayout(area);
}

void ModeBar::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff0d0d10));
    g.fillRect(getLocalBounds());
}

void ModeBar::buttonClicked(juce::Button*)
{
    updateModeSelection();
}

void ModeBar::timerCallback()
{
    syncButtonsToState();
    syncSmoothingToState();
}

void ModeBar::updateModeSelection()
{
    auto& apvts = proc_.getAPVTS();

    for (int i = 0; i < 2; ++i)
    {
        if (sourceButtons_[i].getToggleState())
        {
            ParameterBinding::setChoiceIndexWithGesture(apvts, "morphSource", i, 2);
            proc_.setMorphSource(i);
            break;
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        if (modeButtons_[i].getToggleState())
        {
            ParameterBinding::setChoiceIndexWithGesture(apvts, "physicsMode", i, 3);
            proc_.setPhysicsMode(i);
            break;
        }
    }
}

void ModeBar::syncButtonsToState()
{
    auto& apvts = proc_.getAPVTS();

    int src = proc_.getMorphSource();
    if (auto* raw = apvts.getRawParameterValue("morphSource"))
        src = juce::roundToInt(raw->load(std::memory_order_relaxed));
    if (src >= 0 && src < 2)
    {
        for (int i = 0; i < 2; ++i)
            sourceButtons_[i].setToggleState(i == src, juce::dontSendNotification);
    }

    int phys = proc_.getPhysicsMode();
    if (auto* raw = apvts.getRawParameterValue("physicsMode"))
        phys = juce::roundToInt(raw->load(std::memory_order_relaxed));
    if (phys >= 0 && phys < 3)
    {
        for (int i = 0; i < 3; ++i)
            modeButtons_[i].setToggleState(i == phys, juce::dontSendNotification);
    }

    // R1b: keep the Elastic preset combo in sync with the processor and reveal
    // it only while Elastic is the active physics mode.
    const int preset = juce::jlimit(0, 2, proc_.getElasticPreset());
    if (elasticPresetCombo_.getSelectedItemIndex() != preset)
    {
        elasticPresetSyncing_ = true;
        elasticPresetCombo_.setSelectedItemIndex(preset, juce::dontSendNotification);
        elasticPresetSyncing_ = false;
    }
    updateElasticPresetVisibility();
}

void ModeBar::updateElasticPresetVisibility()
{
    // Elastic == physics mode index 1. Hide the preset controls on Direct/Drift
    // so they never look inert.
    const bool isElastic = modeButtons_[1].getToggleState();
    elasticPresetCombo_.setVisible(isElastic);
    elasticLabel_.setVisible(isElastic);
}

void ModeBar::syncSmoothingToState()
{
    if (smoothingGestureActive_)
        return;

    float value = proc_.getSmoothingRate();
    if (auto* raw = proc_.getAPVTS().getRawParameterValue("smoothing"))
        value = raw->load(std::memory_order_relaxed);

    value = juce::jlimit(0.0f, 0.999f, value);
    if (std::abs(static_cast<float>(smoothSlider_.getValue()) - value) <= 0.0005f)
        return;

    syncing_ = true;
    smoothSlider_.setValue(value, juce::dontSendNotification);
    syncing_ = false;
}

} // namespace more_phi
