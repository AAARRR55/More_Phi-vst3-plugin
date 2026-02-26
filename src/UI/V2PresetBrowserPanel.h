/* MorphSnap — UI/V2PresetBrowserPanel.h
 * V2 preset browser panel for the "Presets" tab.
 * Provides search, filtering, list browsing, and CRUD operations
 * against the PresetLibrary JSON store.
 * MESSAGE THREAD ONLY. */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Preset/PresetLibrary.h"
#include "Preset/PresetEntry.h"
#include <vector>
#include <string>

namespace morphsnap {

class MorphSnapProcessor;

// ── PresetListModel ───────────────────────────────────────────────────────────
// ListBoxModel that renders PresetEntry rows with name, author, stars, and tags.

class PresetListModel final : public juce::ListBoxModel
{
public:
    PresetListModel() = default;

    // Replace the displayed entries and refresh the owning ListBox.
    void setEntries(std::vector<PresetEntry> entries, juce::ListBox* listBox);

    const PresetEntry* getEntry(int row) const noexcept;
    int numEntries() const noexcept { return static_cast<int>(entries_.size()); }

    // juce::ListBoxModel interface
    int  getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int width, int height, bool isSelected) override;

    // Colours shared with the panel (set once after construction).
    juce::Colour colBackground   { 0xff16213e };
    juce::Colour colAltRow       { 0xff1a2742 };
    juce::Colour colSelected     { 0x30ec415d };
    juce::Colour colText         { 0xffe8eaed };
    juce::Colour colDim          { 0xff8b95a5 };
    juce::Colour colTags         { 0xff4a5568 };
    juce::Colour colStarFilled   { 0xfffbbf24 };
    juce::Colour colStarEmpty    { 0xff4a5568 };

private:
    void drawStars(juce::Graphics& g, int rating, juce::Rectangle<float> area) const;

    std::vector<PresetEntry> entries_;
};

// ── V2PresetBrowserPanel ──────────────────────────────────────────────────────

class V2PresetBrowserPanel final : public juce::Component,
                                   private juce::TextEditor::Listener,
                                   private juce::ComboBox::Listener,
                                   private juce::Timer
{
public:
    /** Constructor. Takes a reference to the processor for save/load operations. */
    explicit V2PresetBrowserPanel(MorphSnapProcessor& processor);
    ~V2PresetBrowserPanel() override;

    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Force a full refresh of the preset list (e.g. after external changes). */
    void refresh();

private:
    // ── Listener callbacks ────────────────────────────────────────────────────

    void textEditorTextChanged(juce::TextEditor& editor) override;
    void comboBoxChanged(juce::ComboBox* combo) override;
    void timerCallback() override;   // Deferred search after typing

    // ── Button actions ────────────────────────────────────────────────────────

    void onSaveClicked();
    void onLoadClicked();
    void onDeleteClicked();
    void onImportClicked();
    void onExportClicked();

    // ── Internal helpers ──────────────────────────────────────────────────────

    /** Rebuild the displayed list from the current search query. */
    void runSearch();

    /** Populate the plugin filter combo from library metadata. */
    void repopulatePluginFilter();

    /** Return the currently selected PresetEntry, or nullptr if none. */
    const PresetEntry* selectedEntry() const noexcept;

    /** Update the count label to reflect current result count. */
    void updateCountLabel();

    // ── Data ──────────────────────────────────────────────────────────────────

    MorphSnapProcessor& proc_;
    PresetLibrary       presetLibrary_;

    // Current search results (owned by the list model).
    PresetListModel listModel_;

    // ── Top row widgets ───────────────────────────────────────────────────────

    juce::TextEditor  searchEditor_;
    juce::ComboBox    pluginFilterCombo_;
    juce::ComboBox    sortCombo_;
    juce::Label       countLabel_;

    // ── Center list ───────────────────────────────────────────────────────────

    juce::ListBox presetList_;

    // ── Bottom action buttons ─────────────────────────────────────────────────

    juce::TextButton saveBtn_   { "Save"   };
    juce::TextButton loadBtn_   { "Load"   };
    juce::TextButton deleteBtn_ { "Delete" };
    juce::TextButton importBtn_ { "Import" };
    juce::TextButton exportBtn_ { "Export" };

    // ── Colours ───────────────────────────────────────────────────────────────

    static constexpr juce::uint32 kBackground  = 0xff16213e;
    static constexpr juce::uint32 kSurface     = 0xff1a2742;
    static constexpr juce::uint32 kPadBg       = 0xff0a1628;
    static constexpr juce::uint32 kBorder      = 0xff1e3a5f;
    static constexpr juce::uint32 kTextPrimary = 0xffe8eaed;
    static constexpr juce::uint32 kTextDim     = 0xff8b95a5;
    static constexpr juce::uint32 kCoral       = 0xffec415d;
    static constexpr juce::uint32 kCoralSel    = 0x30ec415d;
    static constexpr juce::uint32 kAmber       = 0xfffbbf24;
    static constexpr juce::uint32 kStarEmpty   = 0xff4a5568;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(V2PresetBrowserPanel)
};

} // namespace morphsnap
