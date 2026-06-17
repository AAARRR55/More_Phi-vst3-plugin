/*
 * More-Phi — UI/BottomControlStrip.cpp
 * Bottom control strip: SanityMode, RecallMode, Sidechain controls.
 * Glassmorphic panel with coral accent highlights.
 *
 * NOTE: All APVTS attachments are wrapped in null guards because FL Studio
 *       calls createEditor() before parameters may be fully initialized.
 */
#include "BottomControlStrip.h"
#include "Plugin/PluginProcessor.h"
#include "MorePhiLookAndFeel.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

BottomControlStrip::BottomControlStrip(MorePhiProcessor& p)
    : processor(p)
{
    // ── Safety mode selectors ───────────────────────────────────────────────
    for (auto* button : { &sanityToggle_, &listenToggle_, &linkToggle_ })
    {
        button->setClickingTogglesState(true);
        button->setColour(juce::TextButton::buttonColourId,
                          juce::Colour(0xff0d0d10));
        button->setColour(juce::TextButton::textColourOffId,
                          juce::Colour(0xffeeeef2));
        button->setColour(juce::TextButton::textColourOnId,
                          juce::Colours::white);
        addAndMakeVisible(*button);
    }

    sanityToggle_.setColour(juce::TextButton::buttonOnColourId,
                            juce::Colour(0xffe5c057));

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
            ParameterBinding::setValueWithGesture(*param, 0.0f);  // Fast = index 0
        updateRecallButtons();
    };
    recallFullBtn_.onClick = [this]() {
        if (auto* param = processor.getAPVTS().getParameter("recallMode"))
            ParameterBinding::setValueWithGesture(*param, 1.0f);  // Full = index 1
        updateRecallButtons();
    };
    addAndMakeVisible(recallFastBtn_);
    addAndMakeVisible(recallFullBtn_);
    updateRecallButtons();

    // ── Sidechain controls ─────────────────────────────────────────────────
    sidechainToggle_.setColour(juce::ToggleButton::tickColourId,
                                juce::Colour(0xffe5c057));
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

    thresholdLabel_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
    thresholdLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    thresholdLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(thresholdLabel_);

    // ── Listen Mode toggle ────────────────────────────────────────────────
    listenToggle_.setColour(juce::TextButton::buttonOnColourId,
                            juce::Colour(0xff4fc3f7));

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
    linkToggle_.setColour(juce::TextButton::buttonOnColourId,
                          juce::Colour(0xffffb74d));

    if (auto* param = processor.getAPVTS().getParameter("linkMode"))
    {
        linkAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "linkMode", linkToggle_);
    }

    // ── Output Gain knob ───────────────────────────────────────────────────
    outputGainKnob_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    outputGainKnob_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 14);
    outputGainKnob_.setTextValueSuffix(" dB");
    outputGainKnob_.setColour(juce::Slider::rotarySliderFillColourId,
                               juce::Colour(0xffe5c057));
    outputGainKnob_.setColour(juce::Slider::rotarySliderOutlineColourId,
                               juce::Colour(0xff17181c));
    outputGainKnob_.setColour(juce::Slider::textBoxTextColourId,
                               juce::Colour(0xffeeeef2));
    outputGainKnob_.setColour(juce::Slider::textBoxOutlineColourId,
                               juce::Colours::transparentBlack);
    addAndMakeVisible(outputGainKnob_);

    if (auto* param = processor.getAPVTS().getParameter("outputGain"))
    {
        gainAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "outputGain", outputGainKnob_);
    }

    outputGainLabel_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
    outputGainLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    outputGainLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(outputGainLabel_);

    // ── Bypass button ──────────────────────────────────────────────────────
    bypassBtn_.setClickingTogglesState(true);
    bypassBtn_.setColour(juce::TextButton::buttonColourId,
                          juce::Colour(0xff0d0d10));
    bypassBtn_.setColour(juce::TextButton::buttonOnColourId,
                          juce::Colour(0xffe5c057));
    bypassBtn_.setColour(juce::TextButton::textColourOffId,
                          juce::Colour(0xffeeeef2));
    bypassBtn_.setColour(juce::TextButton::textColourOnId,
                          juce::Colours::white);
    addAndMakeVisible(bypassBtn_);

    if (auto* param = processor.getAPVTS().getParameter("bypass"))
    {
        bypassAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "bypass", bypassBtn_);
    }
}

void BottomControlStrip::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const bool compact = getWidth() < 760;

    // Glassmorphic background
    g.setColour(juce::Colour(0xff0d0d10).withAlpha(0.85f));
    g.fillRoundedRectangle(bounds.reduced(4, 0), 6.0f);

    // Subtle top border glow
    g.setColour(juce::Colour(0x30ec415d));
    g.drawRoundedRectangle(bounds.reduced(4, 0), 6.0f, 1.0f);

    // Section dividers
    g.setColour(juce::Colour(0xff323237));
    if (compact)
    {
        const int midX = getWidth() / 2;
        const int midY = getHeight() / 2;
        g.drawLine(static_cast<float>(midX), 4.0f,
                   static_cast<float>(midX), static_cast<float>(getHeight() - 4), 1.0f);
        g.drawLine(8.0f, static_cast<float>(midY),
                   static_cast<float>(getWidth() - 8), static_cast<float>(midY), 1.0f);

        g.setColour(juce::Colour(0x704a5568));
        g.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
        g.drawText("SAFETY", 10, 3, midX - 20, 12, juce::Justification::centredLeft);
        g.drawText("RECALL", midX + 10, 3, midX - 20, 12, juce::Justification::centredLeft);
        g.drawText("OUTPUT", 10, midY + 3, midX - 20, 12, juce::Justification::centredLeft);
        g.drawText("SC", midX + 10, midY + 3, midX - 20, 12, juce::Justification::centredLeft);
        return;
    }

    int q = getWidth() / 4;
    int divX1 = q;
    int divX2 = 2 * q;
    int divX3 = 3 * q;
    g.drawLine(static_cast<float>(divX1), 4.0f,
               static_cast<float>(divX1), static_cast<float>(getHeight() - 4), 1.0f);
    g.drawLine(static_cast<float>(divX2), 4.0f,
               static_cast<float>(divX2), static_cast<float>(getHeight() - 4), 1.0f);
    g.drawLine(static_cast<float>(divX3), 4.0f,
               static_cast<float>(divX3), static_cast<float>(getHeight() - 4), 1.0f);

    g.setColour(juce::Colour(0x704a5568));
    g.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
    g.drawText("SAFETY", 10, 3, divX1 - 20, 12, juce::Justification::centredLeft);
    g.drawText("RECALL", divX1 + 10, 3, divX2 - divX1 - 20, 12, juce::Justification::centredLeft);
    g.drawText("OUTPUT", divX2 + 10, 3, divX3 - divX2 - 20, 12, juce::Justification::centredLeft);
    g.drawText("SC", divX3 + 10, 3, getWidth() - divX3 - 20, 12, juce::Justification::centredLeft);
}

void BottomControlStrip::resized()
{
    if (getWidth() < 760)
    {
        auto area = getLocalBounds().reduced(8, 14);
        auto topRow = area.removeFromTop(area.getHeight() / 2);
        auto bottomRow = area;
        auto safetyArea = topRow.removeFromLeft(topRow.getWidth() / 2).reduced(4, 2);
        auto recallArea = topRow.reduced(4, 2);
        auto outputArea = bottomRow.removeFromLeft(bottomRow.getWidth() / 2).reduced(4, 2);
        auto scArea = bottomRow.reduced(4, 2);

        auto removeLabelSpace = [](juce::Rectangle<int>& cell)
        {
            cell.removeFromTop(10);
        };

        removeLabelSpace(safetyArea);
        juce::FlexBox safetyRow;
        safetyRow.flexDirection = juce::FlexBox::Direction::row;
        safetyRow.items.add(juce::FlexItem(sanityToggle_).withFlex(1).withMargin(2));
        safetyRow.items.add(juce::FlexItem(listenToggle_).withFlex(1).withMargin(2));
        safetyRow.items.add(juce::FlexItem(linkToggle_).withFlex(1).withMargin(2));
        safetyRow.performLayout(safetyArea);

        removeLabelSpace(recallArea);
        juce::FlexBox recallRow;
        recallRow.flexDirection = juce::FlexBox::Direction::row;
        recallRow.items.add(juce::FlexItem(recallFastBtn_).withFlex(1).withMargin(2));
        recallRow.items.add(juce::FlexItem(recallFullBtn_).withFlex(1).withMargin(2));
        recallRow.items.add(juce::FlexItem(recallToggle_).withFlex(1).withMargin(2));
        recallRow.performLayout(recallArea);

        removeLabelSpace(outputArea);
        juce::FlexBox outputRow;
        outputRow.flexDirection = juce::FlexBox::Direction::row;
        outputRow.items.add(juce::FlexItem(outputGainKnob_).withWidth(52.0f).withMargin(1));
        outputRow.items.add(juce::FlexItem(outputGainLabel_).withFlex(1).withMargin(1));
        outputRow.items.add(juce::FlexItem(bypassBtn_).withWidth(70.0f).withMargin(2));
        outputRow.performLayout(outputArea);

        removeLabelSpace(scArea);
        juce::FlexBox scRow;
        scRow.flexDirection = juce::FlexBox::Direction::row;
        scRow.items.add(juce::FlexItem(sidechainToggle_).withWidth(36.0f).withMargin(2));
        scRow.items.add(juce::FlexItem(thresholdKnob_).withWidth(54.0f).withMargin(1));
        scRow.items.add(juce::FlexItem(thresholdLabel_).withFlex(1).withMargin(1));
        scRow.performLayout(scArea);
        return;
    }

    auto area = getLocalBounds().reduced(8, 14);

    // Outer: 4 equal-width columns via FlexBox
    juce::FlexBox outer;
    outer.flexDirection = juce::FlexBox::Direction::row;
    int sectionWidth = area.getWidth() / 4;

    // Section 1: Safety (Sanity + Listen + Link)
    auto safetyArea = juce::Rectangle<int>(area.getX(), area.getY(), sectionWidth, area.getHeight()).reduced(3, 0);
    {
        juce::FlexBox row;
        row.flexDirection = juce::FlexBox::Direction::row;
        row.items.add(juce::FlexItem(sanityToggle_).withFlex(1).withMargin(2));
        row.items.add(juce::FlexItem(listenToggle_).withFlex(1).withMargin(2));
        row.items.add(juce::FlexItem(linkToggle_).withFlex(1).withMargin(2));
        row.performLayout(safetyArea);
    }

    // Section 2: Recall (Mode buttons + Sustain toggle)
    auto recallArea = juce::Rectangle<int>(area.getX() + sectionWidth, area.getY(),
                                            sectionWidth, area.getHeight()).reduced(3, 0);
    {
        auto recallTop = recallArea.removeFromTop(recallArea.getHeight() / 2);
        juce::FlexBox btnPair;
        btnPair.flexDirection = juce::FlexBox::Direction::row;
        btnPair.items.add(juce::FlexItem(recallFastBtn_).withFlex(1).withMargin(2));
        btnPair.items.add(juce::FlexItem(recallFullBtn_).withFlex(1).withMargin(2));
        btnPair.performLayout(recallTop);
        recallToggle_.setBounds(recallArea);
    }

    // Section 3: Output (Gain knob + Bypass button)
    auto outputArea = juce::Rectangle<int>(area.getX() + sectionWidth * 2, area.getY(),
                                            sectionWidth, area.getHeight()).reduced(3, 0);
    {
        juce::FlexBox col;
        col.flexDirection = juce::FlexBox::Direction::column;
        col.items.add(juce::FlexItem(outputGainKnob_).withFlex(1));
        col.items.add(juce::FlexItem(outputGainLabel_).withHeight(12.0f));
        col.items.add(juce::FlexItem(bypassBtn_).withHeight(18.0f).withMargin({ 1, 2, 0, 2 }));
        col.performLayout(outputArea);
    }

    // Section 4: Sidechain
    auto scArea = juce::Rectangle<int>(area.getX() + sectionWidth * 3, area.getY(),
                                        sectionWidth, area.getHeight()).reduced(3, 0);
    {
        juce::FlexBox scRow;
        scRow.flexDirection = juce::FlexBox::Direction::row;
        scRow.items.add(juce::FlexItem(sidechainToggle_).withWidth(32.0f));
        scRow.items.add(juce::FlexItem(thresholdKnob_).withFlex(1));
        scRow.performLayout(scArea);
        thresholdLabel_.setBounds(scArea.getX(), scArea.getBottom() - 12,
                                   scArea.getWidth(), 12);
    }
}

void BottomControlStrip::updateRecallButtons()
{
    auto* param = processor.getAPVTS().getParameter("recallMode");
    bool isFast = (param == nullptr) || param->getValue() < 0.5f;

    auto coralColour = juce::Colour(0xffe5c057);
    auto dimColour = juce::Colour(0xff0d0d10);

    recallFastBtn_.setColour(juce::TextButton::buttonColourId,
                              isFast ? coralColour : dimColour);
    recallFastBtn_.setColour(juce::TextButton::textColourOffId,
                              isFast ? juce::Colours::white : juce::Colour(0xff8e8f95));
    recallFullBtn_.setColour(juce::TextButton::buttonColourId,
                              !isFast ? coralColour : dimColour);
    recallFullBtn_.setColour(juce::TextButton::textColourOffId,
                              !isFast ? juce::Colours::white : juce::Colour(0xff8e8f95));
}

} // namespace more_phi
