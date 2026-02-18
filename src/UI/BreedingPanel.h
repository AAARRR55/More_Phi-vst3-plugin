/*
 * MorphSnap - UI/BreedingPanel.h
 * Snapshot breeding/mutation utilities.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapProcessor;

class BreedingPanel : public juce::Component
{
public:
    explicit BreedingPanel(MorphSnapProcessor& processor);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void breedSnapshots();
    void mutateSnapshot();
    void randomizeMorphPosition();
    int findNextEmptySlot() const;

    MorphSnapProcessor& proc_;
    juce::TextButton breedButton_ { "Breed" };
    juce::TextButton mutateButton_ { "Mutate" };
    juce::TextButton randomizeButton_ { "Randomize" };
    juce::Label statusLabel_;
    juce::Random random_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BreedingPanel)
};

} // namespace morphsnap
