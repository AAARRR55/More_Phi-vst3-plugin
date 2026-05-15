/*
 * More-Phi — UI/V2TabBar.cpp
 * V2 tab bar: surface-navy background, coral 3 px accent under the active tab,
 * dim text for inactive tabs, bold bright text for the selected tab.
 *
 * Threading: constructed and used on the JUCE message thread only.
 */
#include "V2TabBar.h"
#include "UI/Theme/MorePhiTheme.h"

namespace more_phi {

using namespace Theme::Colours;
using namespace Theme::Metrics;

// Tab underline height
static constexpr int kAccentH = 3;

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
                           textDim());
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           textBright());

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
    juce::FlexBox fb;
    fb.flexDirection = juce::FlexBox::Direction::row;

    for (int i = 0; i < NumTabs; ++i)
        fb.items.add(juce::FlexItem(tabs_[i]).withFlex(1));

    fb.performLayout(getLocalBounds());
}

// ── Painting ──────────────────────────────────────────────────────────────────

void V2TabBar::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    // 1. Background fill
    g.setColour(surface());
    g.fillRect(bounds);

    // 2. 1 px top border
    g.setColour(border());
    g.drawLine(0.0f, 0.0f,
               static_cast<float>(bounds.getWidth()), 0.0f,
               1.0f);

    // 3. Coral accent under the selected tab (subtle rounded bar)
    const juce::Rectangle<int>& selBounds = tabs_[selected_].getBounds();
    g.setColour(accent());
    g.fillRoundedRectangle(static_cast<float>(selBounds.getX() + 4),
                           static_cast<float>(bounds.getHeight() - kAccentH),
                           static_cast<float>(selBounds.getWidth() - 8),
                           static_cast<float>(kAccentH),
                           1.5f);
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
                           juce::Colour(i == selected_ ? textBright() : textDim()));
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           textBright());
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
                           juce::Colour(active ? textBright() : textDim()));
        tabs_[i].setColour(juce::TextButton::textColourOnId,
                           textBright());
    }

    // Move the coral underline.
    repaint();

    if (onTabChanged)
        onTabChanged(selected_);
}

} // namespace more_phi
