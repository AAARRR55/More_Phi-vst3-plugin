/*
 * More-Phi - UI/BreedingPanel.cpp
 */
#include "BreedingPanel.h"
#include "Plugin/PluginProcessor.h"
#include "Core/SnapshotBank.h"
#include "Core/GeneticEngine.h"
#include "UI/Bindings/ParameterBinding.h"
#include <array>
#include <vector>

namespace more_phi {

BreedingPanel::BreedingPanel(MorePhiProcessor& processor)
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
    std::array<int, SnapshotBank::NUM_SLOTS> occupied{};
    const int occupiedCount = bank.getOccupiedSlots(occupied);
    if (occupiedCount < 2)
    {
        statusLabel_.setText("Need at least 2 snapshots to breed", juce::dontSendNotification);
        return;
    }

    const int parentA = occupied[static_cast<size_t>(random_.nextInt(occupiedCount))];
    int parentB = parentA;
    while (parentB == parentA)
        parentB = occupied[static_cast<size_t>(random_.nextInt(occupiedCount))];

    std::vector<float> a;
    std::vector<float> b;
    if (!bank.getSlotValuesCopy(parentA, a) || !bank.getSlotValuesCopy(parentB, b))
    {
        statusLabel_.setText("Failed to read parent snapshots", juce::dontSendNotification);
        return;
    }

    const int count = juce::jmin(static_cast<int>(a.size()), static_cast<int>(b.size()));
    if (count <= 0)
    {
        statusLabel_.setText("Selected snapshots are empty", juce::dontSendNotification);
        return;
    }

    // FIX C17: Route breeding through GeneticEngine so SanityConfig protects
    // volume/bypass/output parameters from mutation.
    ParameterState stateA, stateB;
    stateA.parameterCount = count;
    stateB.parameterCount = count;
    for (int i = 0; i < count; ++i)
    {
        stateA.values[static_cast<size_t>(i)] = a[static_cast<size_t>(i)];
        stateB.values[static_cast<size_t>(i)] = b[static_cast<size_t>(i)];
    }
    stateA.occupied = true;
    stateB.occupied = true;

    const float alpha = 0.2f + random_.nextFloat() * 0.6f;
    auto offspring = GeneticEngine::breed(stateA, stateB, alpha, 0.0f, random_, proc_.getSanityConfig());

    std::vector<float> blended(static_cast<size_t>(offspring.parameterCount), 0.0f);
    for (int i = 0; i < offspring.parameterCount; ++i)
        blended[static_cast<size_t>(i)] = offspring.values[static_cast<size_t>(i)];

    const int queued = proc_.enqueueParameterState(blended);
    if (queued != count)
    {
        statusLabel_.setText("Queue full: partial blend applied", juce::dontSendNotification);
        return;
    }

    const int targetSlot = findNextEmptySlot();
    bank.captureValues(targetSlot, blended);

    statusLabel_.setText("Bred slot " + juce::String(targetSlot + 1)
                             + " from " + juce::String(parentA + 1)
                             + " + " + juce::String(parentB + 1),
                         juce::dontSendNotification);
}

void BreedingPanel::mutateSnapshot()
{
    auto& bank = proc_.getSnapshotBank();
    std::array<int, SnapshotBank::NUM_SLOTS> occupied{};
    const int occupiedCount = bank.getOccupiedSlots(occupied);
    if (occupiedCount <= 0)
    {
        statusLabel_.setText("Capture a snapshot before mutating", juce::dontSendNotification);
        return;
    }

    const int sourceSlot = occupied[static_cast<size_t>(random_.nextInt(occupiedCount))];
    std::vector<float> mutated;
    if (!bank.getSlotValuesCopy(sourceSlot, mutated) || mutated.empty())
    {
        statusLabel_.setText("Selected snapshot has no parameters", juce::dontSendNotification);
        return;
    }

    ParameterState state;
    state.capture(mutated.data(), static_cast<int>(mutated.size()));

    const float amount = 0.06f;
    const auto sanity = proc_.getSanityConfig();

    // H-6 FIX: Only mutate learned (exposed) parameters; fallback to all if none exposed
    const auto exposed = proc_.getParameterClassifier().getExposedParameterIndices();
    std::unordered_set<int> learnedParams(exposed.begin(), exposed.end());
    if (learnedParams.empty())
    {
        for (int i = 0; i < state.parameterCount; ++i)
            learnedParams.insert(i);
    }

    GeneticEngine::smartRandomize(state, amount, learnedParams, random_, sanity);

    mutated.assign(state.values.begin(), state.values.begin() + state.parameterCount);

    const int queued = proc_.enqueueParameterState(mutated);
    if (queued != static_cast<int>(mutated.size()))
    {
        statusLabel_.setText("Queue full: partial mutation applied", juce::dontSendNotification);
        return;
    }

    const int targetSlot = findNextEmptySlot();
    bank.captureValues(targetSlot, mutated);

    statusLabel_.setText("Mutated slot " + juce::String(sourceSlot + 1)
                             + " into slot " + juce::String(targetSlot + 1),
                         juce::dontSendNotification);
}

void BreedingPanel::randomizeMorphPosition()
{
    const float x = random_.nextFloat();
    const float y = random_.nextFloat();

    proc_.setMorphX(x);
    proc_.setMorphY(y);
    proc_.setMorphSource(0);

    if (auto* px = proc_.getAPVTS().getParameter("morphX"))
        ParameterBinding::setValueWithGesture(*px, x);
    if (auto* py = proc_.getAPVTS().getParameter("morphY"))
        ParameterBinding::setValueWithGesture(*py, y);
    ParameterBinding::setChoiceIndexWithGesture(proc_.getAPVTS(), "morphSource", 0, 2);

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

} // namespace more_phi
