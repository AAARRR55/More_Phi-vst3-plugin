/*
 * More-Phi — UI/HybridBlendPanel.cpp
 *
 * Implements the hybrid blend weight control strip.
 *
 * Blend weight normalization:
 *   Raw slider values are stored as [0, 1] each. When any slider changes,
 *   the three raw values are summed and each is divided by the total to
 *   produce normalized weights that always sum to 1.0. If all three are
 *   zero the Direct weight is forced to 1.0 as a safe fallback.
 *
 * All controls route through APVTS for DAW automation support.
 * syncStateFromAPVTS() bridges APVTS → engine setters on the audio thread.
 */
#include "HybridBlendPanel.h"
#include "MorePhiLookAndFeel.h"
#include "Plugin/PluginProcessor.h"
#include "Core/OversamplingWrapper.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

using namespace Theme::Colours;

    // Pixel widths for the three main sections
    constexpr int kLeftSectionW   = 140;
    constexpr int kRightSectionW  = 80;
    // Center section fills the remainder

// ─────────────────────────────────────────────────────────────────────────────

HybridBlendPanel::HybridBlendPanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    auto& apvts = proc_.getAPVTS();

    // ── Audio Domain toggle ───────────────────────────────────────────────────
    setupToggleButton(audioDomainToggle_, "Audio Domain");
    audioDomainToggle_.setToggleState(proc_.getAudioDomainEnabled(),
                                       juce::dontSendNotification);
    ParameterBinding::bindToggleButton(audioDomainToggle_, apvts, "audioDomainEnabled");

    // ── Oversampling combo ────────────────────────────────────────────────────
    oversamplingCombo_.addItem("x1", 1);
    oversamplingCombo_.addItem("x2", 2);
    oversamplingCombo_.addItem("x4", 3);
    oversamplingCombo_.addItem("x8", 4);
    oversamplingCombo_.setSelectedId(1, juce::dontSendNotification); // default x1
    ParameterBinding::bindComboBox(oversamplingCombo_, apvts, "oversampling");
    oversamplingCombo_.setColour(juce::ComboBox::backgroundColourId,
                                  surfaceLit());
    oversamplingCombo_.setColour(juce::ComboBox::textColourId,
                                  textBright());
    oversamplingCombo_.setColour(juce::ComboBox::outlineColourId,
                                  border());

    oversamplingLabel_.setText("Oversampling", juce::dontSendNotification);
    oversamplingLabel_.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    oversamplingLabel_.setColour(juce::Label::textColourId,
                                  textDim());
    oversamplingLabel_.setJustificationType(juce::Justification::centredLeft);

    // ── Vertical blend sliders ────────────────────────────────────────────────
    setupVerticalSlider(paramSlider_,    1.0); // Direct = 1.0 default
    setupVerticalSlider(spectralSlider_, 0.0); // Spectral = 0.0
    setupVerticalSlider(granularSlider_, 0.0); // Granular = 0.0

    paramSlider_.onValueChange    = [this]() { onBlendWeightChanged(); };
    spectralSlider_.onValueChange = [this]() { onBlendWeightChanged(); };
    granularSlider_.onValueChange = [this]() { onBlendWeightChanged(); };
    paramSlider_.onDragStart = spectralSlider_.onDragStart = granularSlider_.onDragStart =
        [this]() { beginBlendGesture(); };
    paramSlider_.onDragEnd = spectralSlider_.onDragEnd = granularSlider_.onDragEnd =
        [this]() { endBlendGesture(); };

    // Slider axis labels
    const auto setupSliderLabel = [this](juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
        lbl.setColour(juce::Label::textColourId, textDim());
        lbl.setJustificationType(juce::Justification::centred);
    };

    setupSliderLabel(paramLabel_,    "Direct");
    setupSliderLabel(spectralLabel_, "Spectral");
    setupSliderLabel(granularLabel_, "Granular");

    // Blend summary label
    blendSummaryLabel_.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    blendSummaryLabel_.setColour(juce::Label::textColourId,
                                  green());
    blendSummaryLabel_.setJustificationType(juce::Justification::centred);
    updateBlendLabel(); // initialise text

    // ── Alpha knob (shares morphAlpha APVTS param with SpectralControlPanel) ──
    alphaKnob_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    alphaKnob_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
    alphaKnob_.setRange(0.0, 1.0, 0.001);
    alphaKnob_.setValue(static_cast<double>(proc_.getMorphAlpha()),
                         juce::dontSendNotification);
    alphaKnob_.setColour(juce::Slider::rotarySliderFillColourId,
                          accent());
    alphaKnob_.setColour(juce::Slider::rotarySliderOutlineColourId,
                          surfaceLit());
    alphaKnob_.setColour(juce::Slider::textBoxTextColourId,
                          textBright());
    alphaKnob_.setColour(juce::Slider::textBoxOutlineColourId,
                          juce::Colours::transparentBlack);
    ParameterBinding::bindSlider(alphaKnob_, apvts, "morphAlpha");

    alphaLabel_.setText("Alpha", juce::dontSendNotification);
    alphaLabel_.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    alphaLabel_.setColour(juce::Label::textColourId, textDim());
    alphaLabel_.setJustificationType(juce::Justification::centred);

    // ── Register children ─────────────────────────────────────────────────────
    addAndMakeVisible(audioDomainToggle_);
    addAndMakeVisible(oversamplingCombo_);
    addAndMakeVisible(oversamplingLabel_);
    addAndMakeVisible(paramSlider_);
    addAndMakeVisible(spectralSlider_);
    addAndMakeVisible(granularSlider_);
    addAndMakeVisible(paramLabel_);
    addAndMakeVisible(spectralLabel_);
    addAndMakeVisible(granularLabel_);
    addAndMakeVisible(blendSummaryLabel_);
    addAndMakeVisible(alphaKnob_);
    addAndMakeVisible(alphaLabel_);
}

// ─────────────────────────────────────────────────────────────────────────────

void HybridBlendPanel::paint(juce::Graphics& g)
{
    // Panel background
    g.setColour(surface().withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Border
    g.setColour(border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    const float h = static_cast<float>(getHeight());

    // Section dividers
    g.setColour(border());
    g.drawLine(static_cast<float>(kLeftSectionW),
               6.0f, static_cast<float>(kLeftSectionW), h - 6.0f, 0.5f);
    g.drawLine(static_cast<float>(getWidth() - kRightSectionW),
               6.0f, static_cast<float>(getWidth() - kRightSectionW), h - 6.0f, 0.5f);

    // Section labels
    drawSectionLabel(g, "BLEND MODE",
                     juce::Rectangle<int>(0, 0, kLeftSectionW, 16));
    drawSectionLabel(g, "BLEND WEIGHTS",
                     juce::Rectangle<int>(kLeftSectionW + 4, 0,
                                          getWidth() - kLeftSectionW - kRightSectionW - 8,
                                          16));
    drawSectionLabel(g, "MORPH",
                     juce::Rectangle<int>(getWidth() - kRightSectionW, 0,
                                          kRightSectionW, 16));
}

void HybridBlendPanel::resized()
{
    const int totalW    = getWidth();
    const int totalH    = getHeight();
    const int pad       = 8;
    const int topOffset = 18;
    const int innerH    = totalH - topOffset - pad;

    // ── Left section: BLEND MODE — vertical FlexBox ──────────────────────────
    {
        auto leftArea = juce::Rectangle<int>(pad, topOffset,
                                              kLeftSectionW - pad * 2, innerH);
        juce::FlexBox leftCol;
        leftCol.flexDirection = juce::FlexBox::Direction::column;
        leftCol.items.add(juce::FlexItem(audioDomainToggle_).withHeight(28.0f));
        leftCol.items.add(juce::FlexItem().withHeight(6.0f));
        leftCol.items.add(juce::FlexItem(oversamplingLabel_).withHeight(14.0f));
        leftCol.items.add(juce::FlexItem(oversamplingCombo_).withHeight(26.0f));
        leftCol.performLayout(leftArea);
    }

    // ── Center section: BLEND WEIGHTS — FlexBox slider row ────────────────────
    {
        const int centerX = kLeftSectionW + pad;
        const int centerW = totalW - kLeftSectionW - kRightSectionW - pad * 2;

        // Summary label at bottom of center section
        const int summaryH = 16;
        blendSummaryLabel_.setBounds(centerX,
                                      topOffset + innerH - summaryH,
                                      centerW, summaryH);

        // Three vertical sliders — FlexBox row with equal columns
        const int sliderH  = 120;
        const int labelH   = 14;
        const int sliderW  = 40;
        const int totalSliderH = sliderH + labelH + 4;
        const int sliderY  = topOffset + (innerH - summaryH - 4 - totalSliderH) / 2;

        juce::Slider* sliders[3] = { &paramSlider_, &spectralSlider_, &granularSlider_ };
        juce::Label*  labels[3]  = { &paramLabel_,  &spectralLabel_,  &granularLabel_  };

        const float colW = static_cast<float>(centerW) / 3.0f;
        for (int i = 0; i < 3; ++i)
        {
            const int cx = centerX + static_cast<int>(colW * i + colW / 2.0f);
            sliders[i]->setBounds(cx - sliderW / 2, sliderY, sliderW, sliderH);
            labels[i]->setBounds(cx - sliderW / 2, sliderY + sliderH + 2,
                                  sliderW, labelH);
        }
    }

    // ── Right section: Alpha knob ─────────────────────────────────────────────
    {
        const int knobSize = 50;
        const int labelH   = 14;
        const int totalH_r = knobSize + labelH + 4;
        const int rx       = totalW - kRightSectionW;
        const int cx       = rx + kRightSectionW / 2;
        const int cy       = topOffset + innerH / 2;

        alphaKnob_.setBounds(cx - knobSize / 2,
                              cy - totalH_r / 2,
                              knobSize, knobSize);
        alphaLabel_.setBounds(cx - knobSize / 2,
                               cy - totalH_r / 2 + knobSize + 2,
                               knobSize, labelH);
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void HybridBlendPanel::setupToggleButton(juce::TextButton& btn,
                                          const juce::String& /*label*/)
{
    btn.setClickingTogglesState(true);
    btn.setColour(juce::TextButton::buttonColourId,   surfaceLit());
    btn.setColour(juce::TextButton::buttonOnColourId, accent());
    btn.setColour(juce::TextButton::textColourOffId,  textBright());
    btn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
}

void HybridBlendPanel::setupVerticalSlider(juce::Slider& slider, double defaultVal)
{
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange(0.0, 1.0, 0.001);
    slider.setValue(defaultVal, juce::dontSendNotification);
    slider.setColour(juce::Slider::thumbColourId,   accent());
    slider.setColour(juce::Slider::trackColourId,   border());
}

void HybridBlendPanel::beginBlendGesture()
{
    if (blendGestureActive_)
        return;

    auto& apvts = proc_.getAPVTS();
    if (auto* p = apvts.getParameter("blendParamWeight")) p->beginChangeGesture();
    if (auto* p = apvts.getParameter("blendSpectralWeight")) p->beginChangeGesture();
    if (auto* p = apvts.getParameter("blendGranularWeight")) p->beginChangeGesture();
    blendGestureActive_ = true;
}

void HybridBlendPanel::endBlendGesture()
{
    if (!blendGestureActive_)
        return;

    auto& apvts = proc_.getAPVTS();
    if (auto* p = apvts.getParameter("blendParamWeight")) p->endChangeGesture();
    if (auto* p = apvts.getParameter("blendSpectralWeight")) p->endChangeGesture();
    if (auto* p = apvts.getParameter("blendGranularWeight")) p->endChangeGesture();
    blendGestureActive_ = false;
}

void HybridBlendPanel::onBlendWeightChanged()
{
    const float rawParam    = static_cast<float>(paramSlider_.getValue());
    const float rawSpectral = static_cast<float>(spectralSlider_.getValue());
    const float rawGranular = static_cast<float>(granularSlider_.getValue());

    float total = rawParam + rawSpectral + rawGranular;

    float normParam, normSpectral, normGranular;
    if (total < 1e-5f)
    {
        // All zero: fall back to 100% Direct
        normParam    = 1.0f;
        normSpectral = 0.0f;
        normGranular = 0.0f;
    }
    else
    {
        normParam    = rawParam    / total;
        normSpectral = rawSpectral / total;
        normGranular = rawGranular / total;
    }

    // Route normalized weights through APVTS for DAW automation support.
    auto& apvts = proc_.getAPVTS();
    if (auto* p = apvts.getParameter("blendParamWeight"))
    {
        if (blendGestureActive_)
            p->setValueNotifyingHost(normParam);
        else
            ParameterBinding::setValueWithGesture(*p, normParam);
    }
    if (auto* p = apvts.getParameter("blendSpectralWeight"))
    {
        if (blendGestureActive_)
            p->setValueNotifyingHost(normSpectral);
        else
            ParameterBinding::setValueWithGesture(*p, normSpectral);
    }
    if (auto* p = apvts.getParameter("blendGranularWeight"))
    {
        if (blendGestureActive_)
            p->setValueNotifyingHost(normGranular);
        else
            ParameterBinding::setValueWithGesture(*p, normGranular);
    }

    updateBlendLabel();
}

void HybridBlendPanel::updateBlendLabel()
{
    const float rawParam    = static_cast<float>(paramSlider_.getValue());
    const float rawSpectral = static_cast<float>(spectralSlider_.getValue());
    const float rawGranular = static_cast<float>(granularSlider_.getValue());

    const float total = rawParam + rawSpectral + rawGranular;

    int pct_d, pct_s, pct_g;
    if (total < 1e-5f)
    {
        pct_d = 100;
        pct_s = 0;
        pct_g = 0;
    }
    else
    {
        pct_d = static_cast<int>(std::round(rawParam    / total * 100.0f));
        pct_s = static_cast<int>(std::round(rawSpectral / total * 100.0f));
        pct_g = static_cast<int>(std::round(rawGranular / total * 100.0f));

        // Clamp rounding error so they always display as summing to 100
        pct_g = 100 - pct_d - pct_s;
        pct_g = juce::jmax(0, pct_g);
    }

    const juce::String text =
        "D:" + juce::String(pct_d) + "%"
        "  S:" + juce::String(pct_s) + "%"
        "  G:" + juce::String(pct_g) + "%";

    blendSummaryLabel_.setText(text, juce::dontSendNotification);
}

void HybridBlendPanel::drawSectionLabel(juce::Graphics& g,
                                         const juce::String& text,
                                         juce::Rectangle<int> bounds) const
{
    g.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
