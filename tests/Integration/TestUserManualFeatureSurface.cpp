#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "UI/V2TabBar.h"
#include "Core/SnapshotBank.h"

#include <memory>

namespace more_phi::test {

namespace {

void requireParameterExists(MorePhiProcessor& processor, const char* parameterId)
{
    INFO("parameterId=" << parameterId);
    REQUIRE(processor.getAPVTS().getParameter(parameterId) != nullptr);
}

void requireParameterMissing(MorePhiProcessor& processor, const char* parameterId)
{
    INFO("parameterId=" << parameterId);
    REQUIRE(processor.getAPVTS().getParameter(parameterId) == nullptr);
}

float requireRawParameterValue(MorePhiProcessor& processor, const char* parameterId)
{
    auto* raw = processor.getAPVTS().getRawParameterValue(parameterId);
    INFO("parameterId=" << parameterId);
    REQUIRE(raw != nullptr);
    return raw->load(std::memory_order_relaxed);
}

void setNormalizedWithGesture(juce::RangedAudioParameter& parameter, float normalizedValue)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(normalizedValue);
    parameter.endChangeGesture();
}

juce::TextButton* findTextButton(juce::Component& root, const juce::String& text)
{
    if (auto* button = dynamic_cast<juce::TextButton*>(&root))
        if (button->getButtonText() == text)
            return button;

    for (int i = 0; i < root.getNumChildComponents(); ++i)
        if (auto* found = findTextButton(*root.getChildComponent(i), text))
            return found;

    return nullptr;
}

void processOneBlock(MorePhiProcessor& processor)
{
    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

bool isAllowedEditorOverlap(juce::Component* a, juce::Component* b)
{
    if (dynamic_cast<juce::ResizableCornerComponent*>(a) != nullptr
        || dynamic_cast<juce::ResizableCornerComponent*>(b) != nullptr)
        return true;

    const bool aIsPad = dynamic_cast<MorphPad*>(a) != nullptr;
    const bool bIsPad = dynamic_cast<MorphPad*>(b) != nullptr;
    const bool aIsRing = dynamic_cast<SnapshotRing*>(a) != nullptr;
    const bool bIsRing = dynamic_cast<SnapshotRing*>(b) != nullptr;
    return (aIsPad && bIsRing) || (aIsRing && bIsPad);
}

void selectSegmentButton(juce::TextButton& button)
{
    button.setToggleState(true, juce::sendNotificationSync);
}

void requireVisibleTopLevelLayoutSane(juce::Component& editor)
{
    const auto local = editor.getLocalBounds();

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* child = editor.getChildComponent(i);
        if (child == nullptr || ! child->isVisible())
            continue;

        INFO("child index=" << i << " bounds=" << child->getBounds().toString());
        REQUIRE(! child->getBounds().isEmpty());
        REQUIRE(local.contains(child->getBounds()));
    }

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* a = editor.getChildComponent(i);
        if (a == nullptr || ! a->isVisible())
            continue;

        for (int j = i + 1; j < editor.getNumChildComponents(); ++j)
        {
            auto* b = editor.getChildComponent(j);
            if (b == nullptr || ! b->isVisible() || isAllowedEditorOverlap(a, b))
                continue;

            const auto overlap = a->getBounds().getIntersection(b->getBounds());
            INFO("overlap i=" << i << " j=" << j << " intersection=" << overlap.toString());
            REQUIRE(overlap.isEmpty());
        }
    }
}

} // namespace

TEST_CASE("User manual feature surface exposes implementation-backed automation parameters", "[integration][user-manual][feature-surface]")
{
    MorePhiProcessor processor;

    const char* validatedParameterIds[] = {
        "morphX",
        "morphY",
        "faderPos",
        "morphSource",
        "physicsMode",
        "smoothing",
        "driftSpeed",
        "driftDistance",
        "driftChaos",
        "outputGain",
        "bypass",
        "sanityEnabled",
        "recallMode",
        "sidechainEnabled",
        "sidechainThreshold",
        "listenMode",
        "recallToggle",
        "driftOutputX",
        "driftOutputY",
        "smartRandomize",
        "linkMode",
    };

    for (const auto* parameterId : validatedParameterIds)
        requireParameterExists(processor, parameterId);
}

TEST_CASE("User guide feature surface exposes implementation-backed workflow controls", "[integration][user-guide][feature-surface]")
{
    MorePhiProcessor processor;

    const char* guideWorkflowParameterIds[] = {
        "morphX",
        "morphY",
        "faderPos",
        "physicsMode",
        "smoothing",
        "driftSpeed",
        "driftDistance",
        "driftChaos",
        "outputGain",
        "bypass",
        "sanityEnabled",
        "recallMode",
        "recallToggle",
        "sidechainEnabled",
        "sidechainThreshold",
        "listenMode",
        "linkMode",
    };

    for (const auto* parameterId : guideWorkflowParameterIds)
        requireParameterExists(processor, parameterId);

    STATIC_REQUIRE(SnapshotBank::NUM_SLOTS == 12);
    REQUIRE(processor.getBus(true, 1) != nullptr);
}

TEST_CASE("User manual feature surface records documented automation ID drift", "[integration][user-manual][feature-surface][documented-mismatch]")
{
    MorePhiProcessor processor;

    requireParameterExists(processor, "faderPos");
    requireParameterMissing(processor, "morphFader");
    requireParameterExists(processor, "morphSource");
}

TEST_CASE("User manual feature surface exposes Engine tab APVTS parameters", "[integration][user-manual][feature-surface]")
{
    MorePhiProcessor processor;

    const char* engineParameterIds[] = {
        "spectralActive",
        "spectralFFTSize",
        "spectralTransient",
        "spectralFormant",
        "granularActive",
        "grainSize",
        "grainDensity",
        "grainPitch",
        "grainScatter",
        "audioDomainEnabled",
        "oversampling",
        "blendParamWeight",
        "blendSpectralWeight",
        "blendGranularWeight",
        "morphAlpha",
    };

    for (const auto* parameterId : engineParameterIds)
        requireParameterExists(processor, parameterId);
}

TEST_CASE("User manual feature surface includes the documented five editor tabs", "[integration][user-manual][feature-surface][gui]")
{
    STATIC_REQUIRE(V2TabBar::Classic == 0);
    STATIC_REQUIRE(V2TabBar::Engine == 1);
    STATIC_REQUIRE(V2TabBar::Modulation == 2);
    STATIC_REQUIRE(V2TabBar::Presets == 3);
    STATIC_REQUIRE(V2TabBar::AI == 4);
    STATIC_REQUIRE(V2TabBar::NumTabs == 5);

    V2TabBar tabBar;
    REQUIRE(tabBar.getSelectedTab() == V2TabBar::Classic);

    tabBar.setSelectedTab(V2TabBar::Engine);
    REQUIRE(tabBar.getSelectedTab() == V2TabBar::Engine);

    tabBar.setSelectedTab(V2TabBar::Modulation);
    REQUIRE(tabBar.getSelectedTab() == V2TabBar::Modulation);

    tabBar.setSelectedTab(V2TabBar::Presets);
    REQUIRE(tabBar.getSelectedTab() == V2TabBar::Presets);

    tabBar.setSelectedTab(V2TabBar::AI);
    REQUIRE(tabBar.getSelectedTab() == V2TabBar::AI);
}

TEST_CASE("User manual feature surface can construct and resize the editor headlessly", "[integration][user-manual][feature-surface][gui]")
{
    MorePhiProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());

    REQUIRE(editor != nullptr);

    const juce::Rectangle<int> sizes[] = {
        {0, 0, 720, 600},
        {0, 0, 920, 760},
        {0, 0, 1600, 1120},
    };

    for (const auto& size : sizes)
    {
        editor->setBounds(size);
        editor->resized();

        REQUIRE(editor->getWidth() == size.getWidth());
        REQUIRE(editor->getHeight() == size.getHeight());
        requireVisibleTopLevelLayoutSane(*editor);
    }
}

TEST_CASE("User manual feature surface mode selector updates APVTS and processor state", "[integration][user-manual][feature-surface][gui][mode]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    REQUIRE(editor != nullptr);
    editor->setBounds(0, 0, 720, 600);
    editor->resized();

    auto* fader = findTextButton(*editor, "Fader");
    auto* pad = findTextButton(*editor, "2D Pad");
    auto* elastic = findTextButton(*editor, "Elastic");
    auto* drift = findTextButton(*editor, "Drift");
    REQUIRE(fader != nullptr);
    REQUIRE(pad != nullptr);
    REQUIRE(elastic != nullptr);
    REQUIRE(drift != nullptr);

    selectSegmentButton(*fader);
    REQUIRE(requireRawParameterValue(processor, "morphSource") == Catch::Approx(1.0f));
    processOneBlock(processor);
    REQUIRE(processor.getMorphSource() == 1);

    selectSegmentButton(*pad);
    REQUIRE(requireRawParameterValue(processor, "morphSource") == Catch::Approx(0.0f));
    processOneBlock(processor);
    REQUIRE(processor.getMorphSource() == 0);

    selectSegmentButton(*elastic);
    REQUIRE(requireRawParameterValue(processor, "physicsMode") == Catch::Approx(1.0f));
    processOneBlock(processor);
    REQUIRE(processor.getPhysicsMode() == 1);

    selectSegmentButton(*drift);
    REQUIRE(requireRawParameterValue(processor, "physicsMode") == Catch::Approx(2.0f));
    processOneBlock(processor);
    REQUIRE(processor.getPhysicsMode() == 2);

    processor.releaseResources();
}

TEST_CASE("User manual feature surface mode selector syncs from automation", "[integration][user-manual][feature-surface][gui][mode]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    REQUIRE(editor != nullptr);
    editor->setBounds(0, 0, 920, 760);
    editor->resized();

    auto* fader = findTextButton(*editor, "Fader");
    auto* drift = findTextButton(*editor, "Drift");
    REQUIRE(fader != nullptr);
    REQUIRE(drift != nullptr);

    auto* sourceParam = processor.getAPVTS().getParameter("morphSource");
    auto* physicsParam = processor.getAPVTS().getParameter("physicsMode");
    REQUIRE(sourceParam != nullptr);
    REQUIRE(physicsParam != nullptr);

    setNormalizedWithGesture(*sourceParam, 1.0f);
    setNormalizedWithGesture(*physicsParam, 1.0f);
    processOneBlock(processor);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(160);

    REQUIRE(fader->getToggleState());
    REQUIRE(drift->getToggleState());

    processor.releaseResources();
}

} // namespace more_phi::test
