/*
 * More-Phi — UI/SpectralControlPanel.cpp
 *
 * Implements the spectral engine control strip.
 * All controls route through APVTS for DAW automation support.
 * syncStateFromAPVTS() bridges APVTS → engine setters on the audio thread.
 */
#include "SpectralControlPanel.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

using namespace Theme::Colours;

// ─────────────────────────────────────────────────────────────────────────────

SpectralControlPanel::SpectralControlPanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    auto& apvts = proc_.getAPVTS();
    auto& engine = proc_.getSpectralEngine();

    // ── Active toggle ─────────────────────────────────────────────────────────
    setupToggleButton(activeToggle_, "Spectral");
    activeToggle_.setToggleState(engine.isActive(), juce::dontSendNotification);
    ParameterBinding::bindToggleButton(activeToggle_, apvts, "spectralActive");

    // ── FFT size combo ────────────────────────────────────────────────────────
    fftSizeCombo_.addItem("512",  1);
    fftSizeCombo_.addItem("1024", 2);
    fftSizeCombo_.addItem("2048", 3);
    fftSizeCombo_.addItem("4096", 4);
    fftSizeCombo_.setSelectedId(3, juce::dontSendNotification); // default 2048
    ParameterBinding::bindComboBox(fftSizeCombo_, apvts, "spectralFFTSize");
    fftSizeCombo_.setColour(juce::ComboBox::backgroundColourId,
                             surfaceLit());
    fftSizeCombo_.setColour(juce::ComboBox::textColourId,
                             textBright());
    fftSizeCombo_.setColour(juce::ComboBox::outlineColourId,
                             border());

    fftSizeLabel_.setText("FFT Size", juce::dontSendNotification);
    fftSizeLabel_.setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
    fftSizeLabel_.setColour(juce::Label::textColourId, textDim());
    fftSizeLabel_.setJustificationType(juce::Justification::centredLeft);

    // ── Alpha knob ────────────────────────────────────────────────────────────
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
    alphaLabel_.setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
    alphaLabel_.setColour(juce::Label::textColourId, textDim());
    alphaLabel_.setJustificationType(juce::Justification::centred);

    // ── Transient toggle ──────────────────────────────────────────────────────
    setupToggleButton(transientToggle_, "Transient");
    transientToggle_.setToggleState(true, juce::dontSendNotification); // default on
    ParameterBinding::bindToggleButton(transientToggle_, apvts, "spectralTransient");

    // ── Formant toggle ────────────────────────────────────────────────────────
    setupToggleButton(formantToggle_, "Formant");
    formantToggle_.setToggleState(false, juce::dontSendNotification); // default off
    ParameterBinding::bindToggleButton(formantToggle_, apvts, "spectralFormant");

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
    g.setColour(surface().withAlpha(0.85f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Border
    g.setColour(border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.25f), 4.0f, 0.5f);

    const float w  = static_cast<float>(getWidth());
    const float h  = static_cast<float>(getHeight());
    const float sectionW = w / 3.0f;

    // Vertical dividers between sections
    g.setColour(border());
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
        juce::FlexBox leftCol;
        leftCol.flexDirection = juce::FlexBox::Direction::column;
        leftCol.items.add(juce::FlexItem(activeToggle_).withHeight(28.0f));
        leftCol.items.add(juce::FlexItem().withHeight(6.0f));
        leftCol.items.add(juce::FlexItem(fftSizeLabel_).withHeight(14.0f));
        leftCol.items.add(juce::FlexItem(fftSizeCombo_).withHeight(26.0f));
        leftCol.performLayout(leftArea);
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
        juce::FlexBox rightCol;
        rightCol.flexDirection = juce::FlexBox::Direction::column;
        rightCol.items.add(juce::FlexItem(transientToggle_).withHeight(28.0f));
        rightCol.items.add(juce::FlexItem().withHeight(6.0f));
        rightCol.items.add(juce::FlexItem(formantToggle_).withHeight(28.0f));
        rightCol.performLayout(rightArea);
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void SpectralControlPanel::setupToggleButton(juce::TextButton& btn,
                                               const juce::String& /*label*/)
{
    btn.setClickingTogglesState(true);
    btn.setColour(juce::TextButton::buttonColourId,    surfaceLit());
    btn.setColour(juce::TextButton::buttonOnColourId,  accent());
    btn.setColour(juce::TextButton::textColourOffId,   textBright());
    btn.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
}

void SpectralControlPanel::drawSectionLabel(juce::Graphics& g,
                                              const juce::String& text,
                                              juce::Rectangle<int> bounds) const
{
    g.setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
