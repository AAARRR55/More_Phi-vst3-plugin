/*
 * MorphSnap — UI/SpectralControlPanel.cpp
 *
 * Implements the spectral engine control strip.
 * Direct engine calls — no APVTS, no parameter automation.
 */
#include "SpectralControlPanel.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

// ── Color constants (panel-local, consistent with dark theme) ─────────────────
namespace {
    constexpr juce::uint32 kBgColour       = 0xff16213e;
    constexpr juce::uint32 kSurfaceColour  = 0xff1a2742;
    constexpr juce::uint32 kAccentColour   = 0xffec415d;
    constexpr juce::uint32 kTextColour     = 0xffe8eaed;
    constexpr juce::uint32 kTextDimColour  = 0xff4a5568;
    constexpr juce::uint32 kBorderColour   = 0xff1e3a5f;
    constexpr juce::uint32 kGreenColour    = 0xff4ade80;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

SpectralControlPanel::SpectralControlPanel(MorphSnapProcessor& proc)
    : proc_(proc)
{
    auto& engine = proc_.getSpectralEngine();

    // ── Active toggle ─────────────────────────────────────────────────────────
    setupToggleButton(activeToggle_, "Spectral");
    activeToggle_.onClick = [this, &engine]()
    {
        engine.setActive(activeToggle_.getToggleState());
    };
    activeToggle_.setToggleState(engine.isActive(), juce::dontSendNotification);

    // ── FFT size combo ────────────────────────────────────────────────────────
    fftSizeCombo_.addItem("512",  1);
    fftSizeCombo_.addItem("1024", 2);
    fftSizeCombo_.addItem("2048", 3);
    fftSizeCombo_.addItem("4096", 4);
    fftSizeCombo_.setSelectedId(3, juce::dontSendNotification); // default 2048
    fftSizeCombo_.onChange = [this, &engine]()
    {
        const int sizes[] = { 512, 1024, 2048, 4096 };
        const int idx = fftSizeCombo_.getSelectedId() - 1;
        if (idx >= 0 && idx < 4)
            engine.setFFTSize(sizes[idx]);
    };
    fftSizeCombo_.setColour(juce::ComboBox::backgroundColourId,
                             juce::Colour(kSurfaceColour));
    fftSizeCombo_.setColour(juce::ComboBox::textColourId,
                             juce::Colour(kTextColour));
    fftSizeCombo_.setColour(juce::ComboBox::outlineColourId,
                             juce::Colour(kBorderColour));

    fftSizeLabel_.setText("FFT Size", juce::dontSendNotification);
    fftSizeLabel_.setFont(juce::Font(juce::FontOptions(8.5f)));
    fftSizeLabel_.setColour(juce::Label::textColourId, juce::Colour(kTextDimColour));
    fftSizeLabel_.setJustificationType(juce::Justification::centredLeft);

    // ── Alpha knob ────────────────────────────────────────────────────────────
    alphaKnob_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    alphaKnob_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
    alphaKnob_.setRange(0.0, 1.0, 0.001);
    alphaKnob_.setValue(static_cast<double>(proc_.getMorphAlpha()),
                         juce::dontSendNotification);
    alphaKnob_.setColour(juce::Slider::rotarySliderFillColourId,
                          juce::Colour(kAccentColour));
    alphaKnob_.setColour(juce::Slider::rotarySliderOutlineColourId,
                          juce::Colour(kSurfaceColour));
    alphaKnob_.setColour(juce::Slider::textBoxTextColourId,
                          juce::Colour(kTextColour));
    alphaKnob_.setColour(juce::Slider::textBoxOutlineColourId,
                          juce::Colours::transparentBlack);
    alphaKnob_.onValueChange = [this]()
    {
        proc_.setMorphAlpha(static_cast<float>(alphaKnob_.getValue()));
    };

    alphaLabel_.setText("Alpha", juce::dontSendNotification);
    alphaLabel_.setFont(juce::Font(juce::FontOptions(8.5f)));
    alphaLabel_.setColour(juce::Label::textColourId, juce::Colour(kTextDimColour));
    alphaLabel_.setJustificationType(juce::Justification::centred);

    // ── Transient toggle ──────────────────────────────────────────────────────
    setupToggleButton(transientToggle_, "Transient");
    transientToggle_.setToggleState(true, juce::dontSendNotification); // default on
    transientToggle_.onClick = [this, &engine]()
    {
        engine.setTransientPreserve(transientToggle_.getToggleState());
    };

    // ── Formant toggle ────────────────────────────────────────────────────────
    setupToggleButton(formantToggle_, "Formant");
    formantToggle_.setToggleState(false, juce::dontSendNotification); // default off
    formantToggle_.onClick = [this, &engine]()
    {
        engine.setFormantPreserve(formantToggle_.getToggleState());
    };

    addAndMakeVisible(activeToggle_);
    addAndMakeVisible(fftSizeCombo_);
    addAndMakeVisible(fftSizeLabel_);
    addAndMakeVisible(alphaKnob_);
    addAndMakeVisible(alphaLabel_);
    addAndMakeVisible(transientToggle_);
    addAndMakeVisible(formantToggle_);
}

// ─────────────────────────────────────────────────────────────────────────────

void SpectralControlPanel::paint(juce::Graphics& g)
{
    // Panel background
    g.setColour(juce::Colour(kBgColour).withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Border
    g.setColour(juce::Colour(kBorderColour));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    const float w  = static_cast<float>(getWidth());
    const float h  = static_cast<float>(getHeight());
    const float sectionW = w / 3.0f;

    // Vertical dividers between sections
    g.setColour(juce::Colour(kBorderColour));
    g.drawLine(sectionW,     6.0f, sectionW,     h - 6.0f, 0.5f);
    g.drawLine(sectionW * 2, 6.0f, sectionW * 2, h - 6.0f, 0.5f);

    // Section labels
    drawSectionLabel(g, "SPECTRAL",
                     juce::Rectangle<int>(0, 0, static_cast<int>(sectionW), 16));
    drawSectionLabel(g, "OPTIONS",
                     juce::Rectangle<int>(static_cast<int>(sectionW * 2), 0,
                                          static_cast<int>(sectionW), 16));
}

void SpectralControlPanel::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    const int sectionW = w / 3;
    const int pad = 8;
    const int topOffset = 18; // space for section label

    // ── Left section: active toggle + FFT size ────────────────────────────────
    {
        auto leftArea = juce::Rectangle<int>(pad, topOffset,
                                              sectionW - pad * 2,
                                              h - topOffset - pad);
        const int btnH = 28;
        activeToggle_.setBounds(leftArea.removeFromTop(btnH));
        leftArea.removeFromTop(6);
        fftSizeLabel_.setBounds(leftArea.removeFromTop(14));
        fftSizeCombo_.setBounds(leftArea.removeFromTop(26));
    }

    // ── Center section: Alpha knob ────────────────────────────────────────────
    {
        const int knobSize = 50;
        const int labelH   = 14;
        const int totalH   = knobSize + labelH + 4;
        const int cx       = sectionW + sectionW / 2;
        const int cy       = topOffset + (h - topOffset) / 2;

        alphaKnob_.setBounds(cx - knobSize / 2,
                              cy - totalH / 2,
                              knobSize, knobSize);
        alphaLabel_.setBounds(cx - knobSize / 2,
                               cy - totalH / 2 + knobSize + 2,
                               knobSize, labelH);
    }

    // ── Right section: Transient + Formant toggles ────────────────────────────
    {
        auto rightArea = juce::Rectangle<int>(sectionW * 2 + pad, topOffset,
                                               sectionW - pad * 2,
                                               h - topOffset - pad);
        const int btnH = 28;
        transientToggle_.setBounds(rightArea.removeFromTop(btnH));
        rightArea.removeFromTop(6);
        formantToggle_.setBounds(rightArea.removeFromTop(btnH));
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void SpectralControlPanel::setupToggleButton(juce::TextButton& btn,
                                               const juce::String& /*label*/)
{
    btn.setClickingTogglesState(true);
    btn.setColour(juce::TextButton::buttonColourId,    juce::Colour(kSurfaceColour));
    btn.setColour(juce::TextButton::buttonOnColourId,  juce::Colour(kAccentColour));
    btn.setColour(juce::TextButton::textColourOffId,   juce::Colour(kTextColour));
    btn.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
}

void SpectralControlPanel::drawSectionLabel(juce::Graphics& g,
                                              const juce::String& text,
                                              juce::Rectangle<int> bounds) const
{
    g.setFont(juce::Font(juce::FontOptions(8.5f)));
    g.setColour(juce::Colour(kTextDimColour));
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace morphsnap
