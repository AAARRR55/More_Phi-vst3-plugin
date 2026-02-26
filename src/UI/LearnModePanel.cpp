/*
 * MorphSnap — UI/LearnModePanel.cpp
 * Implementation of LearnModePanel matching the header definition.
 */
#include "LearnModePanel.h"
#include "../Plugin/PluginProcessor.h"
#include <iomanip>
#include <sstream>

namespace morphsnap {

LearnModePanel::LearnModePanel(MorphSnapProcessor& processor)
    : processor_(processor)
{
    // Add components
    addAndMakeVisible(tokenUsageLabel_);
    addAndMakeVisible(sessionCostLabel_);
    addAndMakeVisible(budgetStatusLabel_);
    addAndMakeVisible(paramsExposedLabel_);

    addAndMakeVisible(learnModeLabel_);
    addAndMakeVisible(classificationLabel_);
    addAndMakeVisible(importanceLabel_);

    addAndMakeVisible(refreshButton_);
    addAndMakeVisible(exposeAllButton_);
    addAndMakeVisible(resetLearnButton_);
    addAndMakeVisible(analyzeButton_);

    // Setup button listeners
    refreshButton_.onClick = [this] { onRefreshClicked(); };
    exposeAllButton_.onClick = [this] { onExposeAllClicked(); };
    resetLearnButton_.onClick = [this] { onResetLearnClicked(); };
    analyzeButton_.onClick = [this] { onAnalyzeClicked(); };

    // Format labels
    auto formatLabel = [](juce::Label& label, const juce::String& text, juce::Colour bg) {
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::backgroundColourId, bg);
        label.setText(text, juce::dontSendNotification);
    };

    const juce::Colour headerBg = juce::Colours::darkgrey.withAlpha(0.5f);
    formatLabel(tokenUsageLabel_, "Token Usage", headerBg);
    formatLabel(sessionCostLabel_, "Session Cost", headerBg);
    formatLabel(budgetStatusLabel_, "Budget", headerBg);
    formatLabel(paramsExposedLabel_, "Params Exposed", headerBg);

    formatLabel(learnModeLabel_, "Learn Mode", headerBg);
    formatLabel(classificationLabel_, "Classification", headerBg);
    formatLabel(importanceLabel_, "Avg Importance", headerBg);

    // Initial refresh
    refresh();

    // Start timer for periodic updates
    startTimer(1000);
}

LearnModePanel::~LearnModePanel()
{
    stopTimer();
}

void LearnModePanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.3f));
    
    g.setColour(juce::Colours::white);
    g.setFont(14.0f);
    
    auto bounds = getLocalBounds().reduced(10);
    g.drawText("AI & Learn Mode Status", bounds.removeFromTop(20), juce::Justification::centred);
    
    g.setColour(juce::Colours::grey);
    g.drawLine(float(bounds.getX()), float(bounds.getY()), float(bounds.getRight()), float(bounds.getY()));
}

void LearnModePanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);
    bounds.removeFromTop(30); // header area

    auto leftCol = bounds.removeFromLeft(bounds.getWidth() / 2).reduced(5);
    auto rightCol = bounds.reduced(5);

    // Left Column: Token Usage & API Config
    tokenUsageLabel_.setBounds(leftCol.removeFromTop(20).withTrimmedBottom(2));
    sessionCostLabel_.setBounds(leftCol.removeFromTop(20).withTrimmedBottom(2));
    budgetStatusLabel_.setBounds(leftCol.removeFromTop(20).withTrimmedBottom(2));
    paramsExposedLabel_.setBounds(leftCol.removeFromTop(20).withTrimmedBottom(2));

    leftCol.removeFromTop(10); // spacing
    analyzeButton_.setBounds(leftCol.removeFromTop(24).withTrimmedBottom(4));
    exposeAllButton_.setBounds(leftCol.removeFromTop(24).withTrimmedBottom(4));

    // Right Column: Learn Mode Diagnostics
    learnModeLabel_.setBounds(rightCol.removeFromTop(20).withTrimmedBottom(2));
    classificationLabel_.setBounds(rightCol.removeFromTop(20).withTrimmedBottom(2));
    importanceLabel_.setBounds(rightCol.removeFromTop(20).withTrimmedBottom(2));

    rightCol.removeFromTop(10); // spacing
    refreshButton_.setBounds(rightCol.removeFromTop(24).withTrimmedBottom(4));
    resetLearnButton_.setBounds(rightCol.removeFromTop(24).withTrimmedBottom(4));
}

void LearnModePanel::refresh()
{
    updateTokenDisplay();
    updateLearnModeDisplay();
    updateCompatibilityDisplay();
}

void LearnModePanel::timerCallback()
{
    refresh();
}

void LearnModePanel::updateTokenDisplay()
{
    auto& optimizer = processor_.getTokenOptimizer();
    auto stats = optimizer.getSessionStats();

    tokenUsageLabel_.setText("Tokens: " + juce::String(stats.totalTokens()), juce::dontSendNotification);

    std::stringstream costStream;
    costStream << std::fixed << std::setprecision(4) << stats.totalCostUsd;
    sessionCostLabel_.setText("Session Cost: $" + juce::String(costStream.str()), juce::dontSendNotification);

    budgetStatusLabel_.setText("Budget: OK", juce::dontSendNotification);
    budgetStatusLabel_.setColour(juce::Label::backgroundColourId, juce::Colours::green.withAlpha(0.3f));
}

void LearnModePanel::updateLearnModeDisplay()
{
    auto& classifier = processor_.getParameterClassifier();
    
    int exposedCount = 0;
    float totalImportance = 0.0f;
    int knownParams = 0;
    
    int totalParams = processor_.getParameterBridge().getParameterCount();
    
    for (int i = 0; i < totalParams; ++i) {
        auto meta = classifier.getMetadata(i);
        if (meta.isExposed) exposedCount++;
        if (meta.type != ParameterType::Unknown) {
            knownParams++;
            totalImportance += meta.importanceScore;
        }
    }
    
    paramsExposedLabel_.setText("Params Exposed: " + juce::String(exposedCount) + " / " + juce::String(totalParams), juce::dontSendNotification);
    
    if (knownParams > 0) {
        classificationLabel_.setText("Classified: " + juce::String(knownParams), juce::dontSendNotification);
        
        float avgImp = totalImportance / knownParams;
        std::stringstream impStream;
        impStream << std::fixed << std::setprecision(2) << avgImp;
        importanceLabel_.setText("Avg Importance: " + juce::String(impStream.str()), juce::dontSendNotification);
    } else {
        classificationLabel_.setText("Classified: None", juce::dontSendNotification);
        importanceLabel_.setText("Avg Importance: 0.00", juce::dontSendNotification);
    }
}

void LearnModePanel::updateCompatibilityDisplay()
{
    // Implementation placeholder for future compatibility tracking updates
}

void LearnModePanel::onRefreshClicked()
{
    refresh();
}

void LearnModePanel::onExposeAllClicked()
{
    auto& classifier = processor_.getParameterClassifier();
    classifier.exposeAll();
    refresh();
}

void LearnModePanel::onResetLearnClicked()
{
    auto& classifier = processor_.getParameterClassifier();
    classifier.hideAll();
    classifier.exposeAll();
    refresh();
}

void LearnModePanel::onAnalyzeClicked()
{
    auto& classifier = processor_.getParameterClassifier();
    classifier.analyzeParameters(processor_.getParameterBridge());
    refresh();
}

} // namespace morphsnap
