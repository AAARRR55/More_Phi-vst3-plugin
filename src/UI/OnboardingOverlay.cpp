#include "OnboardingOverlay.h"
#include "Plugin/PluginProcessor.h"
#include "UI/MorePhiLookAndFeel.h"

namespace more_phi {

OnboardingOverlay::OnboardingOverlay(MorePhiProcessor& proc) : proc_(proc)
{
    setAlwaysOnTop(true);
    setInterceptsMouseClicks(true, false);
    setWantsKeyboardFocus(false);

    steps_ = {
        {
            "Welcome to More-Phi",
            "More-Phi is a VST3/AU plugin that hosts other plugins and morphs "
            "between parameter snapshots in real time.\n\n"
            "Use the Morph Pad (the large circular area) to move between snapshot "
            "positions. Each dot around the ring is a snapshot slot.",
            {}
        },
        {
            "Capturing Snapshots",
            "Right-click any numbered dot to capture the current plugin state into "
            "that slot. Left-click an occupied dot to recall that snapshot.\n\n"
            "Breeding, mutation, and randomization tools are in the Breeding panel "
            "below the Morph Pad.",
            {}
        },
        {
            "Physics Modes & Smoothing",
            juce::CharPointer_UTF8(
                "Choose a physics mode in the Mode bar:\n"
                "\u2022 Direct \u2014 cursor drives morph instantly\n"
                "\u2022 Elastic \u2014 spring physics with momentum\n"
                "\u2022 Drift \u2014 Perlin-noise wandering\n\n"
                "The Smoothing slider blends between morph positions for gliding transitions."),
            {}
        },
        {
            "A/B Compare & Undo",
            "Use the A/B button in the title bar to capture a reference state and "
            "toggle between live tweaks and your reference.\n\n"
            "Snapshot operations (capture, breed, clear) can be undone with Ctrl+Z "
            "via the engine tab.",
            {}
        }
    };

    titleLabel_.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel_);

    bodyEditor_.setMultiLine(true);
    bodyEditor_.setReadOnly(true);
    bodyEditor_.setScrollbarsShown(true);
    bodyEditor_.setFont(juce::Font(juce::FontOptions(13.0f)));
    bodyEditor_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0x00000000));
    bodyEditor_.setColour(juce::TextEditor::textColourId, juce::Colour(0xddddddff));
    bodyEditor_.setColour(juce::TextEditor::outlineColourId, juce::Colour(0x00000000));
    addAndMakeVisible(bodyEditor_);

    nextBtn_.onClick = [this] { showStep(currentStep_ + 1); };
    addAndMakeVisible(nextBtn_);

    dismissBtn_.onClick = [this] { dismiss(); };
    addAndMakeVisible(dismissBtn_);

    stepCounter_.setColour(juce::Label::textColourId, juce::Colour(0xff8e8f95));
    stepCounter_.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(stepCounter_);

    showStep(0);

    // Auto-focus the Next button when the onboarding overlay appears
    nextBtn_.grabKeyboardFocus();
}

void OnboardingOverlay::paint(juce::Graphics& g)
{
    // Semi-transparent backdrop
    g.fillAll(juce::Colour(0xcc0a0a12));

    // Card background
    auto card = getLocalBounds().toFloat().reduced(getWidth() * 0.18f,
                                                     getHeight() * 0.22f);
    g.setColour(juce::Colour(0xf0161820));
    g.fillRoundedRectangle(card, 12.0f);
    g.setColour(juce::Colour(0x40ec415d));
    g.drawRoundedRectangle(card, 12.0f, 1.5f);
}

void OnboardingOverlay::resized()
{
    auto cardArea = getLocalBounds().toFloat();
    auto card = cardArea.reduced(cardArea.getWidth() * 0.18f,
                                  cardArea.getHeight() * 0.22f).toNearestInt();
    card.removeFromTop(20);

    auto headerArea = card.removeFromTop(40).reduced(20, 0);
    titleLabel_.setBounds(headerArea);

    auto bodyArea = card.removeFromTop(card.getHeight() - 50).reduced(20, 8);
    bodyEditor_.setBounds(bodyArea);

    auto footerArea = card.removeFromBottom(40).reduced(20, 4);
    stepCounter_.setBounds(footerArea.removeFromLeft(100));
    dismissBtn_.setBounds(footerArea.removeFromRight(90));
    nextBtn_.setBounds(footerArea.removeFromRight(90));
}

void OnboardingOverlay::mouseDown(const juce::MouseEvent&)
{
    // Click-through to editor — intentionally empty
}

void OnboardingOverlay::showStep(int index)
{
    if (index < 0 || index >= static_cast<int>(steps_.size()))
    {
        dismiss();
        return;
    }

    currentStep_ = index;
    const auto& s = steps_[static_cast<size_t>(index)];
    titleLabel_.setText(s.title, juce::dontSendNotification);
    bodyEditor_.setText(s.body, false);
    stepCounter_.setText(juce::String(index + 1) + " / "
                         + juce::String(steps_.size()), juce::dontSendNotification);
    nextBtn_.setVisible(index < static_cast<int>(steps_.size()) - 1);
    dismissBtn_.setButtonText(index < static_cast<int>(steps_.size()) - 1
        ? "Skip" : "Got it!");
}

void OnboardingOverlay::dismiss()
{
    dismissed_ = true;
    setVisible(false);
}

} // namespace more_phi
