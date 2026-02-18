/*
 * MorphSnap - UI/BreedingPanel.cpp
 */
#include "BreedingPanel.h"
#include "Plugin/PluginProcessor.h"
#include "Core/SnapshotBank.h"
#include "Host/ParameterBridge.h"
#include <vector>

namespace morphsnap {

BreedingPanel::BreedingPanel(MorphSnapProcessor& processor)
    : proc_(processor)
{
    breedButton_.onClick = [this]() { breedSnapshots(); };
    mutateButton_.onClick = [this]() { mutateSnapshot(); };
    randomizeButton_.onClick = [this]() { randomizeMorphPosition(); };

    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0a0b0));
    statusLabel_.setText("Snapshot genetics ready", juce::dontSendNotification);

    addAndMakeVisible(breedButton_);
    addAndMakeVisible(mutateButton_);
    addAndMakeVisible(randomizeButton_);
    addAndMakeVisible(statusLabel_);
}

void BreedingPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff16213e));
    g.fillRect(getLocalBounds());
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawLine(0.0f, 0.0f, static_cast<float>(getWidth()), 0.0f, 1.0f);
}

void BreedingPanel::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    breedButton_.setBounds(area.removeFromLeft(72));
    area.removeFromLeft(6);
    mutateButton_.setBounds(area.removeFromLeft(76));
    area.removeFromLeft(6);
    randomizeButton_.setBounds(area.removeFromLeft(92));
    area.removeFromLeft(10);
    statusLabel_.setBounds(area);
}

void BreedingPanel::breedSnapshots()
{
    auto& bank = proc_.getSnapshotBank();
    auto& bridge = proc_.getParameterBridge();

    std::vector<int> occupied;
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (bank.isOccupied(i))
            occupied.push_back(i);
    }

    if (occupied.size() < 2)
    {
        statusLabel_.setText("Need at least 2 snapshots to breed", juce::dontSendNotification);
        return;
    }

    const int parentA = occupied[static_cast<size_t>(random_.nextInt(static_cast<int>(occupied.size())))];
    int parentB = parentA;
    while (parentB == parentA)
        parentB = occupied[static_cast<size_t>(random_.nextInt(static_cast<int>(occupied.size())))];

    const auto& a = bank.getSlot(parentA).values;
    const auto& b = bank.getSlot(parentB).values;
    const int count = juce::jmin(static_cast<int>(a.size()), static_cast<int>(b.size()));
    if (count <= 0)
    {
        statusLabel_.setText("Selected snapshots are empty", juce::dontSendNotification);
        return;
    }

    const float alpha = 0.2f + random_.nextFloat() * 0.6f;
    std::vector<float> blended(static_cast<size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
        blended[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] * (1.0f - alpha)
                                        + b[static_cast<size_t>(i)] * alpha;

    bridge.applyParameterState(blended);

    const int targetSlot = findNextEmptySlot();
    bank.getSlot(targetSlot).capture(blended.data(), count);

    statusLabel_.setText("Bred slot " + juce::String(targetSlot + 1)
                             + " from " + juce::String(parentA + 1)
                             + " + " + juce::String(parentB + 1),
                         juce::dontSendNotification);
}

void BreedingPanel::mutateSnapshot()
{
    auto& bank = proc_.getSnapshotBank();
    auto& bridge = proc_.getParameterBridge();

    std::vector<int> occupied;
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (bank.isOccupied(i))
            occupied.push_back(i);
    }

    if (occupied.empty())
    {
        statusLabel_.setText("Capture a snapshot before mutating", juce::dontSendNotification);
        return;
    }

    const int sourceSlot = occupied[static_cast<size_t>(random_.nextInt(static_cast<int>(occupied.size())))];
    const auto& sourceValues = bank.getSlot(sourceSlot).values;
    if (sourceValues.empty())
    {
        statusLabel_.setText("Selected snapshot has no parameters", juce::dontSendNotification);
        return;
    }

    std::vector<float> mutated = sourceValues;
    for (auto& value : mutated)
    {
        const float delta = (random_.nextFloat() - 0.5f) * 0.12f;
        value = juce::jlimit(0.0f, 1.0f, value + delta);
    }

    bridge.applyParameterState(mutated);

    const int targetSlot = findNextEmptySlot();
    bank.getSlot(targetSlot).capture(mutated.data(), static_cast<int>(mutated.size()));

    statusLabel_.setText("Mutated slot " + juce::String(sourceSlot + 1)
                             + " into slot " + juce::String(targetSlot + 1),
                         juce::dontSendNotification);
}

void BreedingPanel::randomizeMorphPosition()
{
    const float x = random_.nextFloat();
    const float y = random_.nextFloat();

    proc_.morphX.store(x, std::memory_order_relaxed);
    proc_.morphY.store(y, std::memory_order_relaxed);
    proc_.morphSource.store(0, std::memory_order_relaxed);

    if (auto* px = proc_.getAPVTS().getParameter("morphX"))
        px->setValueNotifyingHost(x);
    if (auto* py = proc_.getAPVTS().getParameter("morphY"))
        py->setValueNotifyingHost(y);

    statusLabel_.setText("Randomized position: X " + juce::String(x, 2)
                             + " / Y " + juce::String(y, 2),
                         juce::dontSendNotification);
}

int BreedingPanel::findNextEmptySlot() const
{
    auto& bank = proc_.getSnapshotBank();
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (!bank.isOccupied(i))
            return i;
    }

    // If all slots are full, recycle slot 0.
    return 0;
}

} // namespace morphsnap
