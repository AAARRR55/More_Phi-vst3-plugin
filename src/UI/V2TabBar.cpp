/*
 * More-Phi — UI/V2TabBar.cpp
 * V2 tab bar: surface-navy background, coral 3 px accent under the active tab,
 * dim text for inactive tabs, bold bright text for the selected tab.
 *
 * Threading: constructed and used on the JUCE message thread only.
 */
#include "V2TabBar.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/MorePhiLookAndFeel.h"

namespace more_phi {

using namespace Theme::Colours;
using namespace Theme::Metrics;

// Tab underline height
static constexpr int kAccentH = 3;

// ── Constructor ───────────────────────────────────────────────────────────────

V2TabBar::V2TabBar()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void V2TabBar::resized()
{
    repaint();
}

// ── Painting ──────────────────────────────────────────────────────────────────

namespace {

int countVisibleTabs(int mask) noexcept
{
    int n = 0;
    for (int i = 0; i < more_phi::V2TabBar::NumTabs; ++i)
        if (mask & (1 << i))
            ++n;
    return n;
}

int visibleIndexOfTab(int mask, int tab) noexcept
{
    int idx = 0;
    for (int i = 0; i < tab; ++i)
        if (mask & (1 << i))
            ++idx;
    return idx;
}

int tabAtVisibleIndex(int mask, int visIdx) noexcept
{
    int seen = -1;
    for (int i = 0; i < more_phi::V2TabBar::NumTabs; ++i)
    {
        if (mask & (1 << i))
            ++seen;
        if (seen == visIdx)
            return i;
    }
    return -1;
}

} // namespace

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

    auto tabFont = MorePhiLookAndFeel::bodyFont(11.0f);
    auto selectedFont = MorePhiLookAndFeel::bodyFont(11.0f, juce::Font::bold);

    const int visibleCount = countVisibleTabs(visibleMask_);
    if (visibleCount <= 0)
        return;

    int drawn = 0;
    for (int i = 0; i < NumTabs; ++i)
    {
        if (!isTabVisible(i))
            continue;

        const auto tabBounds = getTabBounds(i);
        const bool selected = i == selected_;
        const bool hovered = i == hovered_;

        if (selected)
        {
            g.setColour(surfaceLit().withAlpha(0.82f));
            g.fillRect(tabBounds.reduced(1, 0));
        }
        else if (hovered)
        {
            g.setColour(surfaceLit().withAlpha(0.38f));
            g.fillRect(tabBounds.reduced(1, 0));
        }

        if (drawn > 0)
        {
            g.setColour(border().withAlpha(0.65f));
            g.drawLine(static_cast<float>(tabBounds.getX()), 5.0f,
                       static_cast<float>(tabBounds.getX()), static_cast<float>(bounds.getHeight() - 5),
                       1.0f);
        }

        g.setFont(selected ? selectedFont : tabFont);
        g.setColour(selected ? textBright() : textDim());
        g.drawFittedText(tabNames[i], tabBounds.reduced(6, 0),
                         juce::Justification::centred, 1, 0.86f);
        ++drawn;
    }

    // 3. Coral accent under the selected tab (subtle rounded bar)
    if (isTabVisible(selected_))
    {
        const auto selBounds = getTabBounds(selected_);
        g.setColour(accent());
        g.fillRoundedRectangle(static_cast<float>(selBounds.getX() + 8),
                               static_cast<float>(bounds.getHeight() - kAccentH),
                               static_cast<float>(selBounds.getWidth() - 16),
                               static_cast<float>(kAccentH),
                               1.5f);
    }
}

void V2TabBar::mouseDown(const juce::MouseEvent& event)
{
    selectTab(getTabIndexAt(event.getPosition()));
}

void V2TabBar::mouseMove(const juce::MouseEvent& event)
{
    const int nextHovered = getTabIndexAt(event.getPosition());
    if (nextHovered == hovered_)
        return;

    hovered_ = nextHovered;
    repaint();
}

void V2TabBar::mouseExit(const juce::MouseEvent&)
{
    if (hovered_ == -1)
        return;

    hovered_ = -1;
    repaint();
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
    repaint();
}

void V2TabBar::setVisibleTabs(int visibleMask) noexcept
{
    if (visibleMask_ == visibleMask)
        return;

    visibleMask_ = visibleMask;
    if (!isTabVisible(selected_))
        selected_ = Classic;
    repaint();
}

bool V2TabBar::isTabVisible(int tab) const noexcept
{
    return tab >= 0 && tab < NumTabs && (visibleMask_ & (1 << tab)) != 0;
}

// ── Private helpers ───────────────────────────────────────────────────────────

void V2TabBar::selectTab(int index)
{
    if (index < 0 || index >= NumTabs || index == selected_ || !isTabVisible(index))
        return;

    selected_ = index;

    // Move the coral underline.
    repaint();

    if (onTabChanged)
        onTabChanged(selected_);
}

juce::Rectangle<int> V2TabBar::getTabBounds(int index) const
{
    const auto bounds = getLocalBounds();
    if (index < 0 || index >= NumTabs || bounds.isEmpty() || !isTabVisible(index))
        return {};

    const int visibleCount = countVisibleTabs(visibleMask_);
    if (visibleCount <= 0)
        return {};

    const int visIdx = visibleIndexOfTab(visibleMask_, index);
    const int left  = bounds.getX() + bounds.getWidth() * visIdx / visibleCount;
    const int right = bounds.getX() + bounds.getWidth() * (visIdx + 1) / visibleCount;
    return { left, bounds.getY(), right - left, bounds.getHeight() };
}

int V2TabBar::getTabIndexAt(juce::Point<int> point) const
{
    const auto bounds = getLocalBounds();
    if (! bounds.contains(point) || bounds.getWidth() <= 0)
        return -1;

    const int visibleCount = countVisibleTabs(visibleMask_);
    if (visibleCount <= 0)
        return -1;

    const int visIdx = juce::jlimit(0, visibleCount - 1, point.x * visibleCount / bounds.getWidth());
    return tabAtVisibleIndex(visibleMask_, visIdx);
}

} // namespace more_phi
