/*
 * More-Phi - UI/BreedingPanel.h
 * Snapshot breeding/mutation utilities.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiProcessor;

class BreedingPanel : public juce::Component
{
public:
    explicit BreedingPanel(MorePhiProcessor& processor);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void breedSnapshots();
    void mutateSnapshot();
    void randomizeMorphPosition();
    int findNextEmptySlot() const;

    MorePhiProcessor& proc_;
    juce::TextButton breedButton_ { "Breed" };
    juce::TextButton mutateButton_ { "Mutate" };
    juce::TextButton randomizeButton_ { "Randomize" };
    juce::TextButton waypointStartStop_ { "Waypoints" };
    juce::TextButton clearWaypoints_ { "Clear WP" };
    juce::Label statusLabel_;
    juce::Random random_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BreedingPanel)
};

} // namespace more_phi
