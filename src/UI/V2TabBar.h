/*
 * More-Phi — UI/V2TabBar.h
 * Top-level tab bar for the V2 editor layout.
 * Five equal-width tabs: Classic | Engine | Modulation | Presets | AI.
 * Coral accent bottom border marks the active tab; all painting is custom.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace more_phi {

class V2TabBar : public juce::Component
{
public:
    enum Tab { Classic = 0, Engine = 1, Modulation = 2, Presets = 3, AI = 4, NumTabs = 5 };

    V2TabBar();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;

    /** Returns the index of the currently selected tab (0-3). */
    int getSelectedTab() const noexcept;

    /** Programmatically select a tab without triggering onTabChanged. */
    void setSelectedTab(int tab);

    // AUDIT-2026-06-25: standard/expert mode hides Engine, Modulation and AI tabs.
    // Bits correspond to Tab enum values (1 << Classic, 1 << Engine, ...).
    void setVisibleTabs(int visibleMask) noexcept;
    [[nodiscard]] bool isTabVisible(int tab) const noexcept;

    /** Called whenever the active tab changes.  Argument is the new tab index. */
    std::function<void(int)> onTabChanged;

private:
    /** Internal helper — updates state and fires onTabChanged. */
    void selectTab(int index);
    juce::Rectangle<int> getTabBounds(int index) const;
    int getTabIndexAt(juce::Point<int> point) const;

    int selected_ = 0;
    int hovered_ = -1;
    int visibleMask_ = (1 << Classic) | (1 << Engine) | (1 << Modulation) | (1 << Presets) | (1 << AI);

    static constexpr const char* tabNames[] = { "Classic", "Engine", "Modulation", "Presets", "AI" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(V2TabBar)
};

} // namespace more_phi
