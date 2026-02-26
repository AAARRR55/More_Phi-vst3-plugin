/*
 * MorphSnap — UI/V2TabBar.cpp
 * V2 tab bar: surface-navy background, coral 3 px accent under the active tab,
 * dim text for inactive tabs, bold bright text for the selected tab.
 *
 * Threading: constructed and used on the JUCE message thread only.
 */
#include "V2TabBar.h"

namespace morphsnap {

// ── Palette constants (mirrors MorphSnapLookAndFeel) ─────────────────────────
namespace {
    constexpr juce::uint32 kBackground  = 0xff16213e; // Surface navy
    constexpr juce::uint32 kTopBorder   = 0xff1e3a5f; // 1 px top line
    constexpr juce::uint32 kAccentCoral = 0xffec415d; // Active tab underline
    constexpr juce::uint32 kTextActive  = 0xffe8eaed; // Active tab label
    constexpr juce::uint32 kTextDim     = 0xff8b95a5; // Inactive tab label
    constexpr int           kAccentH    = 3;           // Coral underline height (px)
} // namespace

// ── Constructor ───────────────────────────────────────────────────────────────

V2TabBar::V2TabBar()
{
    for (int i = 0; i < NumTabs; ++i)
    {
        tabs_[i].setButtonText(tabNames[i]);
        tabs_[i].setRadioGroupId(9001);
        tabs_[i].setClickingTogglesState(true);

        // Suppress all built-in button painting — V2TabBar::paint() handles
        // backgrounds and underlines; drawButtonText() still fires for labels
        // but we override colours per-button in selectTab().
        tabs_[i].setColour(juce::TextButton::buttonColourId,
                           juce::Colours::transparentBlack);
        tabs_[i].setColour(juce::TextButton::buttonOnColourId,
                           juce::Colours::transparentBlack);
        tabs_[i].setColour(juce::TextButton::textColourOffId,
                           juce::Colour(kTextDim));
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           juce::Colour(kTextActive));

        const int tabIndex = i; // capture by value
        tabs_[i].onClick = [this, tabIndex]()
        {
            selectTab(tabIndex);
        };

        addAndMakeVisible(tabs_[i]);
    }

    // Initialise the first tab as selected without firing the callback.
    tabs_[0].setToggleState(true, juce::dontSendNotification);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void V2TabBar::resized()
{
    const int totalWidth = getWidth();
    const int h          = getHeight();

    // Distribute width as evenly as possible; give any remainder pixel to the
    // rightmost tab so the combined set always reaches the component edge.
    const int baseW      = totalWidth / NumTabs;
    int       xOffset    = 0;

    for (int i = 0; i < NumTabs; ++i)
    {
        const int w = (i == NumTabs - 1) ? (totalWidth - xOffset) : baseW;
        tabs_[i].setBounds(xOffset, 0, w, h);
        xOffset += w;
    }
}

// ── Painting ──────────────────────────────────────────────────────────────────

void V2TabBar::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    // 1. Background fill
    g.setColour(juce::Colour(kBackground));
    g.fillRect(bounds);

    // 2. 1 px top border
    g.setColour(juce::Colour(kTopBorder));
    g.drawLine(0.0f, 0.0f,
               static_cast<float>(bounds.getWidth()), 0.0f,
               1.0f);

    // 3. Coral accent under the selected tab
    const juce::Rectangle<int>& selBounds = tabs_[selected_].getBounds();
    g.setColour(juce::Colour(kAccentCoral));
    g.fillRect(selBounds.getX(),
               bounds.getHeight() - kAccentH,
               selBounds.getWidth(),
               kAccentH);
}

// ── Public API ────────────────────────────────────────────────────────────────

int V2TabBar::getSelectedTab() const noexcept
{
    return selected_;
}

void V2TabBar::setSelectedTab(int tab)
{
    if (tab < 0 || tab >= NumTabs || tab == selected_)
        return;

    selected_ = tab;

    // Keep button toggle state in sync without triggering onClick.
    for (int i = 0; i < NumTabs; ++i)
        tabs_[i].setToggleState(i == selected_, juce::dontSendNotification);

    // Update text colours to reflect the new selection.
    for (int i = 0; i < NumTabs; ++i)
    {
        tabs_[i].setColour(juce::TextButton::textColourOffId,
                           juce::Colour(i == selected_ ? kTextActive : kTextDim));
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           juce::Colour(kTextActive));
    }

    repaint();
}

// ── Private helpers ───────────────────────────────────────────────────────────

void V2TabBar::selectTab(int index)
{
    if (index < 0 || index >= NumTabs || index == selected_)
        return;

    selected_ = index;

    // Apply bold font + bright colour to selected tab; dim all others.
    // JUCE TextButton re-renders on setColour, so no explicit repaint needed
    // for the button faces — but we still call repaint() for the coral line.
    for (int i = 0; i < NumTabs; ++i)
    {
        const bool active = (i == selected_);
        tabs_[i].setColour(juce::TextButton::textColourOffId,
                           juce::Colour(active ? kTextActive : kTextDim));
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           juce::Colour(kTextActive));
    }

    // Move the coral underline.
    repaint();

    if (onTabChanged)
        onTabChanged(selected_);
}

} // namespace morphsnap
