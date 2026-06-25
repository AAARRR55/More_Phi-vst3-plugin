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
#include "UI/MorePhiLookAndFeel.h"

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
    activeToggle_.onClick = [this] { updateEnabledState(); };  // H5: disable sub-controls when off

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
    fftSizeLabel_.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    fftSizeLabel_.setColour(juce::Label::textColourId, textDim());
    fftSizeLabel_.setJustificationType(juce::Justification::centredLeft);

    // ── Alpha knob (REMOVED — duplicate; morphAlpha is controlled from
    //      HybridBlendPanel to avoid confusion) ─────────────────────────────────

    // ── Transient toggle ──────────────────────────────────────────────────────
    setupToggleButton(transientToggle_, "Transient");
    transientToggle_.setToggleState(true, juce::dontSendNotification); // default on
    transientToggle_.setTooltip(
        "Transient preservation: retains percussive attack transients when "
        "morphing in the frequency domain, preventing smearing of sharp hits.");
    ParameterBinding::bindToggleButton(transientToggle_, apvts, "spectralTransient");

    // ── Formant toggle ────────────────────────────────────────────────────────
    setupToggleButton(formantToggle_, "Formant");
    formantToggle_.setToggleState(false, juce::dontSendNotification); // default off
    formantToggle_.setTooltip(
        "Formant preservation: retains vocal/synth formant structure when "
        "morphing in the frequency domain, preserving the character of resonant bodies.");
    ParameterBinding::bindToggleButton(formantToggle_, apvts, "spectralFormant");

    addAndMakeVisible(activeToggle_);
    addAndMakeVisible(fftSizeCombo_);
    addAndMakeVisible(fftSizeLabel_);
    addAndMakeVisible(transientToggle_);
    addAndMakeVisible(formantToggle_);

    updateEnabledState();  // H5: sync sub-control enabled state to the active toggle
}

void SpectralControlPanel::updateEnabledState()
{
    const bool on = activeToggle_.getToggleState();
    fftSizeCombo_.setEnabled(on);
    fftSizeLabel_.setEnabled(on);
    transientToggle_.setEnabled(on);
    formantToggle_.setEnabled(on);
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
    const float sectionW = w / 2.0f;

    // Vertical divider
    g.setColour(border());
    g.drawLine(sectionW, 6.0f, sectionW, h - 6.0f, 0.5f);

    // Section labels
    drawSectionLabel(g, "SPECTRAL",
                     juce::Rectangle<int>(0, 0, static_cast<int>(sectionW), 16));
    drawSectionLabel(g, "OPTIONS",
                     juce::Rectangle<int>(static_cast<int>(sectionW), 0,
                                          static_cast<int>(sectionW), 16));
}

void SpectralControlPanel::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    const int sectionW = w / 2;
    const int pad = 8;
    const int topOffset = 18;

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

    // ── Right section: Transient + Formant toggles ────────────────────────────
    {
        auto rightArea = juce::Rectangle<int>(sectionW + pad, topOffset,
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
    g.setFont(MorePhiLookAndFeel::bodyFont(10.0f));
    g.setColour(textDim());
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centredLeft);
}

} // namespace more_phi
