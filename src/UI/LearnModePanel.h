/*
 * MorphSnap — UI/LearnModePanel.h
 * Status panel for Learn Mode, token usage, and parameter exposure.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapProcessor;

class LearnModePanel : public juce::Component,
                       private juce::Timer
{
public:
    explicit LearnModePanel(MorphSnapProcessor& processor);
    ~LearnModePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Update display data
    void refresh();

private:
    void timerCallback() override;
    void updateTokenDisplay();
    void updateLearnModeDisplay();
    void updateCompatibilityDisplay();

    MorphSnapProcessor& processor_;

    // Token usage labels
    juce::Label tokenUsageLabel_{"Token Usage", "Token Usage"};
    juce::Label sessionCostLabel_{"Session Cost", "$0.000"};
    juce::Label budgetStatusLabel_{"Budget", "OK"};
    juce::Label paramsExposedLabel_{"Params Exposed", "0 / 0"};

    // Learn Mode labels
    juce::Label learnModeLabel_{"Learn Mode", "Learn Mode"};
    juce::Label classificationLabel_{"Classification", "Not analyzed"};
    juce::Label importanceLabel_{"Avg Importance", "0.00"};

    // Buttons
    juce::TextButton refreshButton_{"Refresh"};
    juce::TextButton exposeAllButton_{"Expose All"};
    juce::TextButton resetLearnButton_{"Reset Learning"};
    juce::TextButton analyzeButton_{"Analyze Plugin"};

    // Callbacks
    void onRefreshClicked();
    void onExposeAllClicked();
    void onResetLearnClicked();
    void onAnalyzeClicked();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnModePanel)
};

} // namespace morphsnap
