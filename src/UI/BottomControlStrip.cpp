/*
 * MorphSnap — UI/BottomControlStrip.cpp
 * Bottom control strip: SanityMode, RecallMode, Sidechain controls.
 * Glassmorphic panel with coral accent highlights.
 *
 * NOTE: All APVTS attachments are wrapped in null guards because FL Studio
 *       calls createEditor() before parameters may be fully initialized.
 */
#include "BottomControlStrip.h"
#include "Plugin/PluginProcessor.h"
#include "MorphSnapLookAndFeel.h"

namespace morphsnap {

BottomControlStrip::BottomControlStrip(MorphSnapProcessor& p)
    : processor(p)
{
    // ── SanityMode toggle ──────────────────────────────────────────────────
    sanityToggle_.setColour(juce::ToggleButton::tickColourId,
                            juce::Colour(0xffec415d));
    addAndMakeVisible(sanityToggle_);

    if (auto* param = processor.getAPVTS().getParameter("sanityEnabled"))
    {
        sanityAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "sanityEnabled", sanityToggle_);
    }

    // ── RecallMode buttons ─────────────────────────────────────────────────
    recallFastBtn_.setClickingTogglesState(false);
    recallFullBtn_.setClickingTogglesState(false);
    recallFastBtn_.onClick = [this]() {
        if (auto* param = processor.getAPVTS().getParameter("recallMode"))
            param->setValueNotifyingHost(0.0f);  // Fast = index 0
        updateRecallButtons();
    };
    recallFullBtn_.onClick = [this]() {
        if (auto* param = processor.getAPVTS().getParameter("recallMode"))
            param->setValueNotifyingHost(1.0f);  // Full = index 1
        updateRecallButtons();
    };
    addAndMakeVisible(recallFastBtn_);
    addAndMakeVisible(recallFullBtn_);
    updateRecallButtons();

    // ── Sidechain controls ─────────────────────────────────────────────────
    sidechainToggle_.setColour(juce::ToggleButton::tickColourId,
                                juce::Colour(0xffec415d));
    addAndMakeVisible(sidechainToggle_);

    if (auto* param = processor.getAPVTS().getParameter("sidechainEnabled"))
    {
        scEnableAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "sidechainEnabled", sidechainToggle_);
    }

    thresholdKnob_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    thresholdKnob_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
    thresholdKnob_.setTextValueSuffix(" dB");
    addAndMakeVisible(thresholdKnob_);

    if (auto* param = processor.getAPVTS().getParameter("sidechainThreshold"))
    {
        scThreshAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "sidechainThreshold", thresholdKnob_);
    }

    thresholdLabel_.setFont(juce::Font(juce::FontOptions(9.0f)));
    thresholdLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b95a5));
    thresholdLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(thresholdLabel_);

    // ── Listen Mode toggle ────────────────────────────────────────────────
    listenToggle_.setColour(juce::ToggleButton::tickColourId,
                            juce::Colour(0xff4fc3f7));
    addAndMakeVisible(listenToggle_);

    if (auto* param = processor.getAPVTS().getParameter("listenMode"))
    {
        listenAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "listenMode", listenToggle_);
    }

    // ── Recall Toggle ─────────────────────────────────────────────────────
    recallToggle_.setColour(juce::ToggleButton::tickColourId,
                            juce::Colour(0xff81c784));
    recallToggle_.setToggleState(true, juce::dontSendNotification);  // Default: on
    addAndMakeVisible(recallToggle_);

    if (auto* param = processor.getAPVTS().getParameter("recallToggle"))
    {
        recallToggleAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "recallToggle", recallToggle_);
    }

    // ── Link Mode toggle ───────────────────────────────────────────────────
    linkToggle_.setColour(juce::ToggleButton::tickColourId,
                          juce::Colour(0xffffb74d));  // Amber
    addAndMakeVisible(linkToggle_);

    if (auto* param = processor.getAPVTS().getParameter("linkMode"))
    {
        linkAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "linkMode", linkToggle_);
    }
}

void BottomControlStrip::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Glassmorphic background
    g.setColour(juce::Colour(0xff16213e).withAlpha(0.85f));
    g.fillRoundedRectangle(bounds.reduced(4, 0), 6.0f);

    // Subtle top border glow
    g.setColour(juce::Colour(0x30ec415d));
    g.drawRoundedRectangle(bounds.reduced(4, 0), 6.0f, 1.0f);

    // Section dividers
    g.setColour(juce::Colour(0xff1e3a5f));
    int divX1 = getWidth() / 3;
    int divX2 = 2 * getWidth() / 3;
    g.drawLine(static_cast<float>(divX1), 6.0f,
               static_cast<float>(divX1), static_cast<float>(getHeight() - 6), 1.0f);
    g.drawLine(static_cast<float>(divX2), 6.0f,
               static_cast<float>(divX2), static_cast<float>(getHeight() - 6), 1.0f);

    // Section labels
    g.setColour(juce::Colour(0xff4a5568));
    g.setFont(juce::Font(juce::FontOptions(8.5f)));
    g.drawText("SAFETY", 10, 2, divX1 - 20, 12, juce::Justification::centredLeft);
    g.drawText("RECALL", divX1 + 10, 2, divX2 - divX1 - 20, 12, juce::Justification::centredLeft);
    g.drawText("SIDECHAIN", divX2 + 10, 2, getWidth() - divX2 - 20, 12, juce::Justification::centredLeft);
}

void BottomControlStrip::resized()
{
    auto area = getLocalBounds().reduced(8, 14);
    int sectionWidth = area.getWidth() / 3;

    // Section 1: Safety (Sanity + Listen Mode)
    auto safetyArea = area.removeFromLeft(sectionWidth);
    safetyArea = safetyArea.reduced(4, 0);
    int thirdH = safetyArea.getHeight() / 3;
    sanityToggle_.setBounds(safetyArea.removeFromTop(thirdH));
    listenToggle_.setBounds(safetyArea.removeFromTop(thirdH));
    linkToggle_.setBounds(safetyArea);

    // Section 2: Recall (Mode buttons + Sustain toggle)
    auto recallArea = area.removeFromLeft(sectionWidth);
    recallArea = recallArea.reduced(4, 0);
    auto recallTop = recallArea.removeFromTop(recallArea.getHeight() / 2);
    int halfW = recallTop.getWidth() / 2;
    recallFastBtn_.setBounds(recallTop.removeFromLeft(halfW).reduced(2, 2));
    recallFullBtn_.setBounds(recallTop.reduced(2, 2));
    recallToggle_.setBounds(recallArea);

    // Section 3: Sidechain
    auto scArea = area;
    scArea = scArea.reduced(4, 0);
    sidechainToggle_.setBounds(scArea.removeFromLeft(40));
    thresholdLabel_.setBounds(scArea.removeFromBottom(12));
    thresholdKnob_.setBounds(scArea);
}

void BottomControlStrip::updateRecallButtons()
{
    auto* param = processor.getAPVTS().getParameter("recallMode");
    bool isFast = (param == nullptr) || param->getValue() < 0.5f;

    auto coralColour = juce::Colour(0xffec415d);
    auto dimColour = juce::Colour(0xff16213e);

    recallFastBtn_.setColour(juce::TextButton::buttonColourId,
                              isFast ? coralColour : dimColour);
    recallFastBtn_.setColour(juce::TextButton::textColourOnId,
                              isFast ? juce::Colours::white : juce::Colour(0xff8b95a5));
    recallFullBtn_.setColour(juce::TextButton::buttonColourId,
                              !isFast ? coralColour : dimColour);
    recallFullBtn_.setColour(juce::TextButton::textColourOnId,
                              !isFast ? juce::Colours::white : juce::Colour(0xff8b95a5));
}

} // namespace morphsnap
