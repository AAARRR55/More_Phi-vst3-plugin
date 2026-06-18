/*
 * More-Phi — UI/PerformancePanel.cpp
 * Opt-in performance toggles for the Engine tab.
 */
#include "PerformancePanel.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"
#include "UI/MorePhiLookAndFeel.h"

namespace more_phi {

using namespace Theme::Colours;

// ─────────────────────────────────────────────────────────────────────────────

PerformancePanel::PerformancePanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    auto& apvts = proc_.getAPVTS();

    setupToggleButton(coarseWritesToggle_);
    coarseWritesToggle_.setTooltip(
        "Larger write deadband: fewer setValue() calls to the hosted plugin during morphing.");
    ParameterBinding::bindToggleButton(coarseWritesToggle_, apvts, "coarseParameterWrites");

    setupToggleButton(disableTouchToggle_);
    disableTouchToggle_.setTooltip(
        "Skip the per-block batch parameter read and touch-detection logic (pure morph output).");
    ParameterBinding::bindToggleButton(disableTouchToggle_, apvts, "disableTouchDetection");

    setupToggleButton(throttleCommitsToggle_);
    throttleCommitsToggle_.setTooltip(
        "Push parameter updates to the hosted plugin only every 4th block (continuous-morph CPU relief).");
    ParameterBinding::bindToggleButton(throttleCommitsToggle_, apvts, "throttleParamCommits");

    addAndMakeVisible(coarseWritesToggle_);
    addAndMakeVisible(disableTouchToggle_);
    addAndMakeVisible(throttleCommitsToggle_);
}

// ─────────────────────────────────────────────────────────────────────────────

void PerformancePanel::paint(juce::Graphics& g)
{
    // Panel background — matches HybridBlendPanel.
    g.setColour(surface().withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    drawSectionLabel(g, "PERFORMANCE",
                     juce::Rectangle<int>(0, 0, kLabelWidth, getHeight()));
}

void PerformancePanel::resized()
{
    auto toggleArea = getLocalBounds();
    toggleArea.removeFromLeft(kLabelWidth);
    toggleArea.reduce(8, 4);

    juce::FlexBox row;
    row.flexDirection = juce::FlexBox::Direction::row;
    row.items.add(juce::FlexItem(coarseWritesToggle_)
                      .withFlex(1)
                      .withMargin(juce::FlexItem::Margin(0, 4, 0, 0)));
    row.items.add(juce::FlexItem(disableTouchToggle_)
                      .withFlex(1)
                      .withMargin(juce::FlexItem::Margin(0, 4, 0, 0)));
    row.items.add(juce::FlexItem(throttleCommitsToggle_).withFlex(1));
    row.performLayout(toggleArea);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void PerformancePanel::setupToggleButton(juce::TextButton& btn)
{
    btn.setClickingTogglesState(true);
    btn.setColour(juce::TextButton::buttonColourId,   surfaceLit());
    btn.setColour(juce::TextButton::buttonOnColourId, accent());
    btn.setColour(juce::TextButton::textColourOffId,  textBright());
    btn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
}

void PerformancePanel::drawSectionLabel(juce::Graphics& g,
                                         const juce::String& text,
                                         juce::Rectangle<int> bounds) const
{
    g.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(8, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
