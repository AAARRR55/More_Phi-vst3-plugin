/*
 * More-Phi — UI/ModulationMatrixPanel.cpp
 *
 * Dark-theme modulation routing panel:
 *   Left  (60 %) — scrollable route list; each row shows enable/source/dest/depth.
 *   Right (40 %) — four LFO mini-panels (shape + rate knob).
 *
 * A 5 Hz juce::Timer keeps the route list in sync with the engine (routes may
 * arrive via MCP or automation at any time).
 *
 * Threading: All widget callbacks and timer ticks fire on the message thread.
 * Engine mutation methods (addRoute, removeRoute, etc.) are documented as
 * message-thread-safe in ModulationEngine.h.
 */
#include "ModulationMatrixPanel.h"
#include "Plugin/PluginProcessor.h"
#include "Core/ModulationTypes.h"

namespace more_phi {

// ============================================================================
// Colour constants (dark theme — matches MorePhiLookAndFeel palette)
// ============================================================================

namespace colours {

static const juce::Colour background    { 0xff0d0d10 };
static const juce::Colour headerText    { 0xff3a3a40 };
static const juce::Colour activeAccent  { 0xffe5c057 };  // coral knob / left border
static const juce::Colour comboFill     { 0xff17181c };
static const juce::Colour comboText     { 0xffeeeef2 };
static const juce::Colour border        { 0xff323237 };
static const juce::Colour textSecondary { 0xff8e8f95 };

} // namespace colours

// ============================================================================
// Source name helpers
// ============================================================================

static juce::StringArray buildSourceNames()
{
    // Show first 8 Macros only (Macro_1 .. Macro_8) to fit space.
    return {
        "LFO 1", "LFO 2", "LFO 3", "LFO 4",
        "Env 1", "Env 2",
        "Macro 1", "Macro 2", "Macro 3", "Macro 4",
        "Macro 5", "Macro 6", "Macro 7", "Macro 8",
        "StepSeq 1", "StepSeq 2",
        "MorphX", "MorphY", "Fader",
        "Velocity", "Aftertouch", "ModWheel"
    };
}

// Maps combo index → ModSourceId (parallel to buildSourceNames()).
// Macros 9-16 are omitted from the UI but still exist in the engine.
static ModSourceId sourceIdFromComboIndex(int idx)
{
    // Matches the order in buildSourceNames():
    //  0-3  → LFO_1..LFO_4
    //  4-5  → Envelope_1..Envelope_2
    //  6-13 → Macro_1..Macro_8
    // 14-15 → StepSeq_1..StepSeq_2
    // 16    → MorphX
    // 17    → MorphY
    // 18    → FaderPos
    // 19    → MIDIVelocity
    // 20    → MIDIAftertouch
    // 21    → MIDIModWheel

    static const ModSourceId map[] = {
        ModSourceId::LFO_1,       ModSourceId::LFO_2,
        ModSourceId::LFO_3,       ModSourceId::LFO_4,
        ModSourceId::Envelope_1,  ModSourceId::Envelope_2,
        ModSourceId::Macro_1,     ModSourceId::Macro_2,
        ModSourceId::Macro_3,     ModSourceId::Macro_4,
        ModSourceId::Macro_5,     ModSourceId::Macro_6,
        ModSourceId::Macro_7,     ModSourceId::Macro_8,
        ModSourceId::StepSeq_1,   ModSourceId::StepSeq_2,
        ModSourceId::MorphX,      ModSourceId::MorphY,
        ModSourceId::FaderPos,
        ModSourceId::MIDIVelocity, ModSourceId::MIDIAftertouch,
        ModSourceId::MIDIModWheel
    };

    if (idx >= 0 && idx < static_cast<int>(std::size(map)))
        return map[idx];
    return ModSourceId::LFO_1;
}

static int comboIndexFromSourceId(ModSourceId id)
{
    static const ModSourceId map[] = {
        ModSourceId::LFO_1,       ModSourceId::LFO_2,
        ModSourceId::LFO_3,       ModSourceId::LFO_4,
        ModSourceId::Envelope_1,  ModSourceId::Envelope_2,
        ModSourceId::Macro_1,     ModSourceId::Macro_2,
        ModSourceId::Macro_3,     ModSourceId::Macro_4,
        ModSourceId::Macro_5,     ModSourceId::Macro_6,
        ModSourceId::Macro_7,     ModSourceId::Macro_8,
        ModSourceId::StepSeq_1,   ModSourceId::StepSeq_2,
        ModSourceId::MorphX,      ModSourceId::MorphY,
        ModSourceId::FaderPos,
        ModSourceId::MIDIVelocity, ModSourceId::MIDIAftertouch,
        ModSourceId::MIDIModWheel
    };

    for (int i = 0; i < static_cast<int>(std::size(map)); ++i)
        if (map[i] == id)
            return i;
    return 0;
}

// ============================================================================
// RouteRow
// ============================================================================

RouteRow::RouteRow(int routeId, MorePhiProcessor& proc)
    : routeId_(routeId), proc_(proc)
{
    // --- Enable toggle ---
    enabledToggle_.setButtonText({});
    enabledToggle_.onClick = [this] { onEnabledToggled(); };
    addAndMakeVisible(enabledToggle_);

    // --- Source combo ---
    const auto sourceNames = buildSourceNames();
    int itemId = 1;
    for (const auto& name : sourceNames)
        sourceCombo_.addItem(name, itemId++);
    sourceCombo_.setSelectedId(1, juce::dontSendNotification);
    sourceCombo_.setColour(juce::ComboBox::backgroundColourId, colours::comboFill);
    sourceCombo_.setColour(juce::ComboBox::textColourId,       colours::comboText);
    sourceCombo_.setColour(juce::ComboBox::outlineColourId,    colours::border);
    sourceCombo_.onChange = [this] { onSourceChanged(); };
    addAndMakeVisible(sourceCombo_);

    // --- Dest combo — styled here, items populated by rebuildDestCombo() ---
    destCombo_.setColour(juce::ComboBox::backgroundColourId, colours::comboFill);
    destCombo_.setColour(juce::ComboBox::textColourId,       colours::comboText);
    destCombo_.setColour(juce::ComboBox::outlineColourId,    colours::border);
    destCombo_.onChange = [this] { onDestChanged(); };
    addAndMakeVisible(destCombo_);

    // --- Depth knob (bipolar rotary) ---
    depthKnob_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    depthKnob_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    depthKnob_.setRange(-1.0, 1.0, 0.001);
    depthKnob_.setValue(0.0, juce::dontSendNotification);
    depthKnob_.setColour(juce::Slider::rotarySliderFillColourId,    colours::activeAccent);
    depthKnob_.setColour(juce::Slider::rotarySliderOutlineColourId, colours::border);
    depthKnob_.setColour(juce::Slider::thumbColourId,               colours::activeAccent);
    depthKnob_.setTooltip("Modulation depth [-1.0, +1.0]");
    depthKnob_.onValueChange = [this] { onDepthChanged(); };
    addAndMakeVisible(depthKnob_);

    refresh();
}

void RouteRow::rebuildDestCombo()
{
    // Suppress onChange while repopulating.
    syncing_ = true;

    const int prevSelId = destCombo_.getSelectedId();

    destCombo_.clear(juce::dontSendNotification);
    destCombo_.addItem("(none)", 1);

    const auto& bridge     = proc_.getParameterBridge();
    const int   paramCount = bridge.getParameterCount();

    for (int i = 0; i < paramCount; ++i)
    {
        const juce::String name = bridge.getParameterName(i);
        // Item IDs start at 2; ID 1 is reserved for "(none)".
        destCombo_.addItem(juce::String(i) + ": " + name, i + 2);
    }

    // Restore previous selection if still valid, otherwise fall back to "(none)".
    const int maxId = paramCount + 1;  // highest valid item id
    destCombo_.setSelectedId((prevSelId > 1 && prevSelId <= maxId) ? prevSelId : 1,
                              juce::dontSendNotification);

    syncing_ = false;
}

void RouteRow::resized()
{
    // Layout adapts for compact editors with the parameter drawer open.
    auto area = getLocalBounds().reduced(0, 2);

    enabledToggle_.setBounds(area.removeFromLeft(20));
    area.removeFromLeft(4);
    const int knobW = juce::jlimit(34, 42, area.getWidth() / 8);
    const int sourceW = juce::jlimit(76, 120, area.getWidth() / 3);
    sourceCombo_.setBounds(area.removeFromLeft(sourceW));
    area.removeFromLeft(4);
    destCombo_.setBounds(area.removeFromLeft(juce::jmax(70, area.getWidth() - knobW - 4)));
    area.removeFromLeft(4);
    depthKnob_.setBounds(area.removeFromLeft(knobW));
}

void RouteRow::refresh()
{
    auto& engine = proc_.getModulationEngine();
    const int routeCount = engine.getAssignedRouteCount();

    // Guard: route may have been removed since this row was created.
    if (routeId_ < 0 || routeId_ >= routeCount)
        return;

    syncing_ = true;

    const ModRoute& route = engine.getRoute(routeId_);

    enabledToggle_.setToggleState(route.enabled, juce::dontSendNotification);

    const int srcIdx = comboIndexFromSourceId(route.source);
    sourceCombo_.setSelectedId(srcIdx + 1, juce::dontSendNotification);

    // destParamIndex == -1 means unassigned; combo item 0 is "(none)".
    if (route.destParamIndex < 0)
        destCombo_.setSelectedId(1, juce::dontSendNotification);  // "(none)"
    else
        destCombo_.setSelectedId(route.destParamIndex + 2, juce::dontSendNotification); // +2: item 1 is "(none)"

    depthKnob_.setValue(static_cast<double>(route.depth), juce::dontSendNotification);

    syncing_ = false;
}

void RouteRow::onEnabledToggled()
{
    if (syncing_) return;
    proc_.getModulationEngine().setRouteEnabled(routeId_, enabledToggle_.getToggleState());
}

void RouteRow::onSourceChanged()
{
    if (syncing_) return;

    // Changing the source on an existing route requires remove + re-add preserving dest/depth.
    auto& engine = proc_.getModulationEngine();
    const int routeCount = engine.getAssignedRouteCount();
    if (routeId_ < 0 || routeId_ >= routeCount) return;

    const ModRoute& existing = engine.getRoute(routeId_);
    const ModSourceId newSrc = sourceIdFromComboIndex(sourceCombo_.getSelectedId() - 1);

    engine.removeRoute(routeId_);
    // Note: routeId_ is now stale after removal. The timer will rebuild rows.
    engine.addRoute(newSrc, existing.destParamIndex, existing.depth);
}

void RouteRow::onDestChanged()
{
    if (syncing_) return;

    auto& engine = proc_.getModulationEngine();
    const int routeCount = engine.getAssignedRouteCount();
    if (routeId_ < 0 || routeId_ >= routeCount) return;

    const ModRoute& existing = engine.getRoute(routeId_);

    // Item 1 == "(none)" → destParamIndex -1; items 2+ map to index (id - 2).
    const int selId = destCombo_.getSelectedId();
    const int destIdx = (selId <= 1) ? -1 : (selId - 2);

    engine.removeRoute(routeId_);
    engine.addRoute(existing.source, destIdx, existing.depth);
}

void RouteRow::onDepthChanged()
{
    if (syncing_) return;
    proc_.getModulationEngine().setRouteDepth(routeId_, static_cast<float>(depthKnob_.getValue()));
}

// ============================================================================
// LFOPanel
// ============================================================================

LFOPanel::LFOPanel(int lfoIndex, MorePhiProcessor& proc)
    : lfoIndex_(lfoIndex), proc_(proc)
{
    // Label "LFO N"
    label_.setText("LFO " + juce::String(lfoIndex_ + 1), juce::dontSendNotification);
    label_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::bold)));
    label_.setColour(juce::Label::textColourId, colours::headerText);
    label_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(label_);

    // Shape combo
    shapeCombo_.addItem("Sine",      1);
    shapeCombo_.addItem("Triangle",  2);
    shapeCombo_.addItem("Saw",       3);
    shapeCombo_.addItem("Square",    4);
    shapeCombo_.addItem("S&H",       5);
    shapeCombo_.addItem("Random",    6);
    shapeCombo_.setSelectedId(1, juce::dontSendNotification);
    shapeCombo_.setColour(juce::ComboBox::backgroundColourId, colours::comboFill);
    shapeCombo_.setColour(juce::ComboBox::textColourId,       colours::comboText);
    shapeCombo_.setColour(juce::ComboBox::outlineColourId,    colours::border);
    shapeCombo_.onChange = [this] { onShapeChanged(); };
    addAndMakeVisible(shapeCombo_);

    // Rate knob
    rateKnob_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    rateKnob_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    rateKnob_.setRange(0.01, 50.0, 0.01);
    rateKnob_.setSkewFactorFromMidPoint(4.0);  // log-ish feel, comfortable for 0-50 Hz
    rateKnob_.setValue(1.0, juce::dontSendNotification);
    rateKnob_.setColour(juce::Slider::rotarySliderFillColourId,    colours::activeAccent);
    rateKnob_.setColour(juce::Slider::rotarySliderOutlineColourId, colours::border);
    rateKnob_.setColour(juce::Slider::thumbColourId,               colours::activeAccent);
    rateKnob_.setTooltip("LFO rate [0.01, 50.0] Hz");
    rateKnob_.onValueChange = [this] { onRateChanged(); };
    addAndMakeVisible(rateKnob_);

    // "Hz" unit label
    rateLabel_.setText("Hz", juce::dontSendNotification);
    rateLabel_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
    rateLabel_.setColour(juce::Label::textColourId, colours::textSecondary);
    rateLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(rateLabel_);
}

void LFOPanel::paint(juce::Graphics& g)
{
    // Subtle separator line at top
    g.setColour(colours::border);
    g.drawLine(0.0f, 0.0f, static_cast<float>(getWidth()), 0.0f, 0.5f);
}

void LFOPanel::resized()
{
    auto area = getLocalBounds().reduced(4, 2);

    const int labelW = juce::jlimit(34, 46, area.getWidth() / 5);
    const int knobW = juce::jlimit(36, 44, area.getWidth() / 5);
    const int rateW = 24;
    const int shapeW = juce::jlimit(62, 86, area.getWidth() - labelW - knobW - rateW - 14);

    label_.setBounds(area.removeFromLeft(labelW));
    area.removeFromLeft(4);
    shapeCombo_.setBounds(area.removeFromLeft(shapeW));
    area.removeFromLeft(4);
    rateKnob_.setBounds(area.removeFromLeft(knobW));
    area.removeFromLeft(2);
    rateLabel_.setBounds(area.removeFromLeft(rateW));
}

void LFOPanel::onShapeChanged()
{
    const auto shape = static_cast<LFOShape>(shapeCombo_.getSelectedId() - 1);
    proc_.getModulationEngine().setLFOShape(lfoIndex_, shape);
}

void LFOPanel::onRateChanged()
{
    proc_.getModulationEngine().setLFORate(lfoIndex_, static_cast<float>(rateKnob_.getValue()));
}

// ============================================================================
// ModulationMatrixPanel
// ============================================================================

ModulationMatrixPanel::ModulationMatrixPanel(MorePhiProcessor& proc)
    : proc_(proc)
{
    // ---- Section header: route list ----
    routeListHeader_.setText("Routes", juce::dontSendNotification);
    routeListHeader_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::bold)));
    routeListHeader_.setColour(juce::Label::textColourId, colours::headerText);
    routeListHeader_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(routeListHeader_);

    // ---- Viewport + container ----
    routeViewport_.setViewedComponent(&routeContainer_, false);
    routeViewport_.setScrollBarsShown(true, false);
    routeViewport_.setScrollBarThickness(6);
    addAndMakeVisible(routeViewport_);

    // ---- Bottom buttons ----
    addRouteBtn_.onClick = [this] { onAddRoute(); };
    removeRouteBtn_.onClick = [this] { onRemoveRoute(); };
    clearAllBtn_.onClick   = [this] { onClearAll(); };

    addAndMakeVisible(addRouteBtn_);
    addAndMakeVisible(removeRouteBtn_);
    addAndMakeVisible(clearAllBtn_);

    // ---- Route count label ----
    routeCountLabel_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::plain)));
    routeCountLabel_.setColour(juce::Label::textColourId, colours::textSecondary);
    routeCountLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(routeCountLabel_);

    // ---- Section header: LFOs ----
    lfoHeader_.setText("LFOs", juce::dontSendNotification);
    lfoHeader_.setFont(juce::Font(juce::FontOptions("Inter", 10.0f, juce::Font::bold)));
    lfoHeader_.setColour(juce::Label::textColourId, colours::headerText);
    lfoHeader_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(lfoHeader_);

    // ---- LFO panels ----
    for (int i = 0; i < 4; ++i)
    {
        lfoPanels_[i] = std::make_unique<LFOPanel>(i, proc_);
        addAndMakeVisible(*lfoPanels_[i]);
    }

    // ---- Initial sync ----
    rebuildRouteRows();
    updateRouteCountLabel();

    // 5 Hz timer (200 ms) for engine-state sync
    startTimer(200);
}

ModulationMatrixPanel::~ModulationMatrixPanel()
{
    stopTimer();
}

// ----------------------------------------------------------------------------
// paint
// ----------------------------------------------------------------------------

void ModulationMatrixPanel::paint(juce::Graphics& g)
{
    // Background
    g.setColour(colours::background);
    g.fillRect(getLocalBounds());

    // Divider between left (routes) and right (LFOs)
    const int dividerX = juce::roundToInt(getWidth() * 0.60f);
    g.setColour(colours::border);
    g.drawLine(static_cast<float>(dividerX), 0.0f,
               static_cast<float>(dividerX), static_cast<float>(getHeight()), 0.5f);

    // Alternate-row tinting inside the route container is handled per-row in
    // RouteRow::paint; we draw it here via the container background instead.
    // (See rebuildRouteRows for per-row colouring.)
}

// ----------------------------------------------------------------------------
// resized
// ----------------------------------------------------------------------------

void ModulationMatrixPanel::resized()
{
    auto bounds = getLocalBounds();

    // Split horizontally: 60% left, 40% right
    const int leftWidth = juce::roundToInt(bounds.getWidth() * 0.60f);

    auto leftArea  = bounds.removeFromLeft(leftWidth).reduced(4, 2);
    auto rightArea = bounds.reduced(4, 2);  // remaining 40%

    // ---- Left: route list ----

    // Header label
    routeListHeader_.setBounds(leftArea.removeFromTop(16));
    leftArea.removeFromTop(2);

    // Bottom strip: [Add Route 70] [4] [Remove 60] [4] [Clear All 60] [4] [count label rest]
    auto bottomStrip = leftArea.removeFromBottom(24);
    leftArea.removeFromBottom(2);  // gap between viewport and buttons

    addRouteBtn_.setBounds(bottomStrip.removeFromLeft(70));
    bottomStrip.removeFromLeft(4);
    removeRouteBtn_.setBounds(bottomStrip.removeFromLeft(60));
    bottomStrip.removeFromLeft(4);
    clearAllBtn_.setBounds(bottomStrip.removeFromLeft(60));
    bottomStrip.removeFromLeft(4);
    routeCountLabel_.setBounds(bottomStrip);

    // Viewport fills remaining space
    routeViewport_.setBounds(leftArea);

    // Keep the routeContainer_ as wide as the viewport content area,
    // and tall enough to hold all rows (24px each).
    const int containerWidth  = leftArea.getWidth() - routeViewport_.getScrollBarThickness() - 2;
    const int rowHeight       = 28;
    const int containerHeight = juce::jmax(leftArea.getHeight(),
                                            static_cast<int>(routeRows_.size()) * rowHeight);
    routeContainer_.setBounds(0, 0, containerWidth, containerHeight);

    // Position individual rows
    for (int i = 0; i < static_cast<int>(routeRows_.size()); ++i)
        routeRows_[static_cast<size_t>(i)]->setBounds(0, i * rowHeight, containerWidth, rowHeight);

    // ---- Right: LFO panels ----

    lfoHeader_.setBounds(rightArea.removeFromTop(16));
    rightArea.removeFromTop(2);

    const int lfoHeight = rightArea.getHeight() / 4;
    for (auto& lfo : lfoPanels_)
        lfo->setBounds(rightArea.removeFromTop(lfoHeight));
}

// ----------------------------------------------------------------------------
// Timer — 5 Hz sync
// ----------------------------------------------------------------------------

void ModulationMatrixPanel::timerCallback()
{
    const int engineCount = proc_.getModulationEngine().getAssignedRouteCount();

    if (engineCount != cachedRouteCount_)
    {
        // Route count changed externally (MCP / automation) — full rebuild.
        rebuildRouteRows();
        cachedRouteCount_ = engineCount;
        updateRouteCountLabel();
        resized();
    }
    else
    {
        // Same count — just refresh widget values in case depths/enables changed.
        refreshRouteRows();
    }
}

// ----------------------------------------------------------------------------
// Route management helpers
// ----------------------------------------------------------------------------

void ModulationMatrixPanel::rebuildRouteRows()
{
    // Remove all existing rows from container
    for (auto& row : routeRows_)
        routeContainer_.removeChildComponent(row.get());
    routeRows_.clear();

    auto& engine    = proc_.getModulationEngine();
    const int count = engine.getAssignedRouteCount();

    routeRows_.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        auto row = std::make_unique<RouteRow>(i, proc_);

        // Populate dest combo with hosted plugin parameters.
        row->rebuildDestCombo();

        // Apply alternate-row background tint
        // (RouteRow itself does not override paint; we set a component-level
        //  background colour via the Component opaque/background mechanism.)
        row->setOpaque(true);

        routeContainer_.addAndMakeVisible(*row);
        routeRows_.push_back(std::move(row));
    }

    cachedRouteCount_ = count;
}

void ModulationMatrixPanel::refreshRouteRows()
{
    for (auto& row : routeRows_)
        row->refresh();
}

void ModulationMatrixPanel::updateRouteCountLabel()
{
    const int count = proc_.getModulationEngine().getAssignedRouteCount();
    routeCountLabel_.setText("Routes: " + juce::String(count) + "/128",
                             juce::dontSendNotification);
}

// ----------------------------------------------------------------------------
// Button callbacks
// ----------------------------------------------------------------------------

void ModulationMatrixPanel::onAddRoute()
{
    auto& engine = proc_.getModulationEngine();

    if (engine.getAssignedRouteCount() >= ModulationState::MAX_ROUTES)
        return;  // Full — no-op

    // Default: LFO_1 → no destination, depth 0
    const int newId = engine.addRoute(ModSourceId::LFO_1, -1, 0.0f);
    if (newId < 0)
        return;  // Engine rejected (full)

    rebuildRouteRows();
    updateRouteCountLabel();
    resized();

    // Scroll to the new row
    routeViewport_.setViewPosition(0, newId * 28);
}

void ModulationMatrixPanel::onRemoveRoute()
{
    if (selectedRouteId_ < 0)
    {
        // No explicit selection — remove the last route as a convenience.
        const int count = proc_.getModulationEngine().getAssignedRouteCount();
        if (count <= 0) return;
        selectedRouteId_ = count - 1;
    }

    proc_.getModulationEngine().removeRoute(selectedRouteId_);
    selectedRouteId_ = -1;

    rebuildRouteRows();
    updateRouteCountLabel();
    resized();
}

void ModulationMatrixPanel::onClearAll()
{
    proc_.getModulationEngine().clearAllRoutes();
    selectedRouteId_ = -1;

    rebuildRouteRows();
    updateRouteCountLabel();
    resized();
}

} // namespace more_phi
