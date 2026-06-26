#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

class OnboardingOverlay : public juce::Component
{
public:
    explicit OnboardingOverlay(MorePhiProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

    bool wasDismissed() const noexcept { return dismissed_; }

private:
    struct Step {
        juce::String title;
        juce::String body;
        juce::Rectangle<int> highlightArea; // area of the editor to highlight
    };

    void showStep(int index);
    void dismiss();

    MorePhiProcessor& proc_;
    std::vector<Step> steps_;
    int currentStep_ = 0;
    bool dismissed_ = false;

    juce::Label titleLabel_;
    juce::TextEditor bodyEditor_;
    juce::TextButton nextBtn_ { "Next \u25B6" };
    juce::TextButton dismissBtn_ { "Dismiss" };
    juce::Label stepCounter_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OnboardingOverlay)
};

} // namespace more_phi
