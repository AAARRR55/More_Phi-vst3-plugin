/*
 * More-Phi — UI/V2TabBar.h
 * Top-level tab bar for the V2 editor layout.
 * Five equal-width tabs: Classic | Engine | Modulation | Presets | AI.
 * Coral accent bottom border marks the active tab; all painting is custom.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>

namespace more_phi {

class V2TabBar : public juce::Component
{
public:
    enum Tab { Classic = 0, Engine = 1, Modulation = 2, Presets = 3, AI = 4, NumTabs = 5 };

    V2TabBar();

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Returns the index of the currently selected tab (0-3). */
    int getSelectedTab() const noexcept;

    /** Programmatically select a tab without triggering onTabChanged. */
    void setSelectedTab(int tab);

    /** Called whenever the active tab changes.  Argument is the new tab index. */
    std::function<void(int)> onTabChanged;

private:
    /** Internal helper — updates state and fires onTabChanged. */
    void selectTab(int index);

    std::array<juce::TextButton, NumTabs> tabs_;
    int selected_ = 0;

    static constexpr const char* tabNames[] = { "Classic", "Engine", "Modulation", "Presets", "AI" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(V2TabBar)
};

} // namespace more_phi
