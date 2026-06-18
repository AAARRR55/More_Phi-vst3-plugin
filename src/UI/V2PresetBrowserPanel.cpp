/* More-Phi — UI/V2PresetBrowserPanel.cpp
 * Implementation of V2PresetBrowserPanel and its PresetListModel.
 * MESSAGE THREAD ONLY — all public methods must be called on the JUCE
 * message thread. PresetLibrary is non-reentrant. */

#include "V2PresetBrowserPanel.h"
#include "Plugin/PluginProcessor.h"
#include "Preset/PresetSerializer.h"
#include "MorePhiLookAndFeel.h"

#include <algorithm>
#include <numeric>

namespace more_phi {

// ═══════════════════════════════════════════════════════════════════════════════
// PresetListModel
// ═══════════════════════════════════════════════════════════════════════════════

void PresetListModel::setEntries(std::vector<PresetEntry> entries,
                                 juce::ListBox* listBox)
{
    entries_ = std::move(entries);
    if (listBox != nullptr)
        listBox->updateContent();
}

const PresetEntry* PresetListModel::getEntry(int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(entries_.size()))
        return nullptr;
    return &entries_[static_cast<size_t>(row)];
}

int PresetListModel::getNumRows()
{
    return static_cast<int>(entries_.size());
}

void PresetListModel::paintListBoxItem(int row, juce::Graphics& g,
                                       int width, int height, bool isSelected)
{
    const PresetEntry* entry = getEntry(row);
    if (entry == nullptr)
        return;

    // ── Background ────────────────────────────────────────────────────────────
    if (isSelected)
    {
        g.setColour(juce::Colour(colSelected));
        g.fillRect(0, 0, width, height);
        // Subtle left accent bar for selected row
        g.setColour(juce::Colour(0xffe5c057));
        g.fillRect(0, 0, 3, height);
    }
    else
    {
        const juce::Colour rowBg = (row % 2 == 0) ? colBackground : colAltRow;
        g.setColour(rowBg);
        g.fillRect(0, 0, width, height);
    }

    // ── Layout constants ──────────────────────────────────────────────────────
    constexpr int kPadLeft   = 10;
    constexpr int kPadTop    = 3;
    constexpr int kStarWidth = 64;

    const int starX     = width - kStarWidth - 6;
    const int textRight = starX - 4;

    // ── Name (bold) + author (dim), top half ──────────────────────────────────
    const int topY    = kPadTop;
    const int topH    = height / 2 - kPadTop;

    {
        const juce::String nameStr  = juce::String(entry->name);
        const juce::String authorStr = entry->author.empty()
                                           ? juce::String{}
                                           : juce::String("  " + entry->author);

        // Measure name width to position author inline
        juce::Font boldFont(juce::FontOptions(MorePhiLookAndFeel::bodyTypefaceName(), 12.5f, juce::Font::bold));
        juce::Font dimFont (juce::FontOptions(MorePhiLookAndFeel::bodyTypefaceName(), 11.0f, juce::Font::plain));

        juce::GlyphArrangement ga;
        ga.addLineOfText(boldFont, nameStr, 0.0f, 0.0f);
        const int nameW = ga.getNumGlyphs() > 0
            ? static_cast<int>(ga.getBoundingBox(0, ga.getNumGlyphs(), true).getWidth()) + 1
            : 0;
        const int availW = textRight - kPadLeft;

        g.setFont(boldFont);
        g.setColour(colText);
        g.drawText(nameStr,
                   kPadLeft, topY, juce::jmin(nameW, availW), topH,
                   juce::Justification::centredLeft, true);

        if (!authorStr.isEmpty())
        {
            const int authorX = kPadLeft + juce::jmin(nameW, availW);
            const int authorW = textRight - authorX;
            if (authorW > 20)
            {
                g.setFont(dimFont);
                g.setColour(juce::Colour(colDim));
                g.drawText(authorStr,
                           authorX, topY, authorW, topH,
                           juce::Justification::centredLeft, true);
            }
        }
    }

    // ── Tags (small, bottom half) ─────────────────────────────────────────────
    const int botY = topY + topH;
    const int botH = height - botY - kPadTop;

    if (!entry->tags.empty() && botH >= 10)
    {
        // Join tags with ", "
        std::string joined;
        for (size_t i = 0; i < entry->tags.size(); ++i)
        {
            if (i != 0) joined += ", ";
            joined += entry->tags[i];
        }

        juce::Font tagFont(juce::FontOptions(MorePhiLookAndFeel::bodyTypefaceName(), 10.0f, juce::Font::plain));
        g.setFont(tagFont);
        g.setColour(juce::Colour(colTags));
        g.drawText(juce::String(joined),
                   kPadLeft, botY, textRight - kPadLeft, botH,
                   juce::Justification::centredLeft, true);
    }

    // ── Star rating (right side, vertically centred) ──────────────────────────
    const juce::Rectangle<float> starArea(
        static_cast<float>(starX),
        static_cast<float>(topY),
        static_cast<float>(kStarWidth),
        static_cast<float>(height - kPadTop * 2));

    drawStars(g, entry->rating, starArea);

    // ── Bottom separator line ─────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff323237).withAlpha(0.6f));
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
}

void PresetListModel::drawStars(juce::Graphics& g, int rating,
                                 juce::Rectangle<float> area) const
{
    // Draw 5 stars as small filled/empty circles
    constexpr int   kNumStars  = 5;
    constexpr float kDiameter  = 8.0f;
    constexpr float kGap       = 3.0f;
    constexpr float kTotalW    = kNumStars * kDiameter + (kNumStars - 1) * kGap;

    const float startX = area.getCentreX() - kTotalW * 0.5f;
    const float cy     = area.getCentreY() - kDiameter * 0.5f;

    for (int i = 0; i < kNumStars; ++i)
    {
        const float sx = startX + i * (kDiameter + kGap);
        if (i < rating)
        {
            g.setColour(juce::Colour(colStarFilled));
            g.fillEllipse(sx, cy, kDiameter, kDiameter);
        }
        else
        {
            g.setColour(juce::Colour(colStarEmpty));
            g.drawEllipse(sx + 0.5f, cy + 0.5f, kDiameter - 1.0f, kDiameter - 1.0f, 1.0f);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// V2PresetBrowserPanel — Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════════

V2PresetBrowserPanel::V2PresetBrowserPanel(MorePhiProcessor& processor)
    : proc_(processor)
{
    // ── Initialise library ────────────────────────────────────────────────────
    presetLibrary_.initialize(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("More-Phi").getChildFile("Presets"));

    // ── Pass colours to the list model ────────────────────────────────────────
    listModel_.colBackground = juce::Colour(kBackground);
    listModel_.colAltRow     = juce::Colour(kSurface);
    listModel_.colSelected   = juce::Colour(kCoralSel);
    listModel_.colText       = juce::Colour(kTextPrimary);
    listModel_.colDim        = juce::Colour(kTextDim);
    listModel_.colStarFilled = juce::Colour(kAmber);
    listModel_.colStarEmpty  = juce::Colour(kStarEmpty);

    // ── Search editor ─────────────────────────────────────────────────────────
    searchEditor_.setTextToShowWhenEmpty("Search presets...",
                                         juce::Colour(kTextDim));
    searchEditor_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(kPadBg));
    searchEditor_.setColour(juce::TextEditor::textColourId,       juce::Colour(kTextPrimary));
    searchEditor_.setColour(juce::TextEditor::outlineColourId,    juce::Colour(kBorder));
    searchEditor_.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colour(kCoral).withAlpha(0.6f));
    searchEditor_.addListener(this);
    addAndMakeVisible(searchEditor_);

    // ── Plugin filter combo ───────────────────────────────────────────────────
    pluginFilterCombo_.addItem("All Plugins", 1);
    pluginFilterCombo_.setSelectedId(1, juce::dontSendNotification);
    pluginFilterCombo_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(kSurface));
    pluginFilterCombo_.setColour(juce::ComboBox::textColourId,        juce::Colour(kTextPrimary));
    pluginFilterCombo_.setColour(juce::ComboBox::outlineColourId,     juce::Colour(kBorder));
    pluginFilterCombo_.addListener(this);
    addAndMakeVisible(pluginFilterCombo_);

    // ── Sort combo ────────────────────────────────────────────────────────────
    sortCombo_.addItem("Newest",  1);
    sortCombo_.addItem("Name",    2);
    sortCombo_.addItem("Oldest",  3);
    sortCombo_.addItem("Rating",  4);
    sortCombo_.setSelectedId(1, juce::dontSendNotification);
    sortCombo_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(kSurface));
    sortCombo_.setColour(juce::ComboBox::textColourId,        juce::Colour(kTextPrimary));
    sortCombo_.setColour(juce::ComboBox::outlineColourId,     juce::Colour(kBorder));
    sortCombo_.addListener(this);
    addAndMakeVisible(sortCombo_);

    // ── Preset count label ────────────────────────────────────────────────────
    countLabel_.setJustificationType(juce::Justification::centredRight);
    countLabel_.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    addAndMakeVisible(countLabel_);

    // ── Preset list ───────────────────────────────────────────────────────────
    presetList_.setModel(&listModel_);
    presetList_.setRowHeight(22);
    presetList_.setColour(juce::ListBox::backgroundColourId,   juce::Colour(kBackground));
    presetList_.setColour(juce::ListBox::outlineColourId,      juce::Colour(kBorder));
    presetList_.setOutlineThickness(1);
    addAndMakeVisible(presetList_);

    // ── Action buttons ────────────────────────────────────────────────────────
    auto styleBtn = [&](juce::TextButton& btn)
    {
        btn.setColour(juce::TextButton::buttonColourId,      juce::Colour(kSurface));
        btn.setColour(juce::TextButton::buttonOnColourId,    juce::Colour(kCoral));
        btn.setColour(juce::TextButton::textColourOffId,     juce::Colour(kTextPrimary));
        btn.setColour(juce::TextButton::textColourOnId,      juce::Colours::white);
        addAndMakeVisible(btn);
    };

    styleBtn(saveBtn_);
    styleBtn(loadBtn_);
    styleBtn(deleteBtn_);
    styleBtn(importBtn_);
    styleBtn(exportBtn_);

    saveBtn_.onClick   = [this] { onSaveClicked();   };
    loadBtn_.onClick   = [this] { onLoadClicked();   };
    deleteBtn_.onClick = [this] { onDeleteClicked(); };
    importBtn_.onClick = [this] { onImportClicked(); };
    exportBtn_.onClick = [this] { onExportClicked(); };

    // ── Initial population ────────────────────────────────────────────────────
    repopulatePluginFilter();
    refresh();
}

V2PresetBrowserPanel::~V2PresetBrowserPanel()
{
    stopTimer();
    presetList_.setModel(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// paint / resized
// ═══════════════════════════════════════════════════════════════════════════════

void V2PresetBrowserPanel::paint(juce::Graphics& g)
{
    // Background fill
    g.setColour(juce::Colour(kBackground));
    g.fillRect(getLocalBounds());

    // Hairline border around the entire panel
    g.setColour(juce::Colour(kBorder));
    g.drawRect(getLocalBounds().toFloat(), 0.5f);
}

void V2PresetBrowserPanel::resized()
{
    auto area = getLocalBounds().reduced(4, 4);

    // ── Top row (30px) ────────────────────────────────────────────────────────
    auto topRow = area.removeFromTop(30);
    area.removeFromTop(2); // gap

    searchEditor_.setBounds(topRow.removeFromLeft(200));
    topRow.removeFromLeft(4);
    pluginFilterCombo_.setBounds(topRow.removeFromLeft(120));
    topRow.removeFromLeft(4);
    sortCombo_.setBounds(topRow.removeFromLeft(100));
    topRow.removeFromLeft(4);
    countLabel_.setBounds(topRow); // takes the remainder, right-aligned text

    // ── Bottom row (28px) ─────────────────────────────────────────────────────
    auto bottomRow = area.removeFromBottom(28);
    area.removeFromBottom(2); // gap above buttons

    constexpr int kBtnW = 60;
    constexpr int kBtnGap = 4;

    saveBtn_.setBounds  (bottomRow.removeFromLeft(kBtnW)); bottomRow.removeFromLeft(kBtnGap);
    loadBtn_.setBounds  (bottomRow.removeFromLeft(kBtnW)); bottomRow.removeFromLeft(kBtnGap);
    deleteBtn_.setBounds(bottomRow.removeFromLeft(kBtnW)); bottomRow.removeFromLeft(kBtnGap);
    importBtn_.setBounds(bottomRow.removeFromLeft(kBtnW)); bottomRow.removeFromLeft(kBtnGap);
    exportBtn_.setBounds(bottomRow.removeFromLeft(kBtnW));

    // ── Preset list (remaining height) ────────────────────────────────────────
    presetList_.setBounds(area);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TextEditor / ComboBox listeners
// ═══════════════════════════════════════════════════════════════════════════════

void V2PresetBrowserPanel::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    // Debounce: wait 250 ms after typing stops before searching.
    startTimer(250);
}

void V2PresetBrowserPanel::comboBoxChanged(juce::ComboBox* /*combo*/)
{
    runSearch();
}

void V2PresetBrowserPanel::timerCallback()
{
    stopTimer();
    runSearch();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Search / refresh
// ═══════════════════════════════════════════════════════════════════════════════

void V2PresetBrowserPanel::runSearch()
{
    PresetSearchQuery query;

    // Text query
    query.textQuery = searchEditor_.getText().toStdString();

    // Plugin filter (item 1 = "All Plugins", items 2+ = plugin names)
    const int pluginId = pluginFilterCombo_.getSelectedId();
    if (pluginId > 1)
        query.pluginFilter = pluginFilterCombo_.getText().toStdString();

    // Sort order
    switch (sortCombo_.getSelectedId())
    {
        case 1: query.sortBy = PresetSearchQuery::SortBy::DateNewest; break;
        case 2: query.sortBy = PresetSearchQuery::SortBy::Name;       break;
        case 3: query.sortBy = PresetSearchQuery::SortBy::DateOldest; break;
        case 4: query.sortBy = PresetSearchQuery::SortBy::Rating;     break;
        default: query.sortBy = PresetSearchQuery::SortBy::DateNewest; break;
    }

    query.maxResults = 200; // Show up to 200 results in the panel

    auto results = presetLibrary_.search(query);
    listModel_.setEntries(std::move(results), &presetList_);
    updateCountLabel();
}

void V2PresetBrowserPanel::refresh()
{
    repopulatePluginFilter();
    runSearch();
}

void V2PresetBrowserPanel::repopulatePluginFilter()
{
    const int previousId = pluginFilterCombo_.getSelectedId();

    pluginFilterCombo_.clear(juce::dontSendNotification);
    pluginFilterCombo_.addItem("All Plugins", 1);

    const auto pluginNames = presetLibrary_.getAllPluginNames();
    int itemId = 2;
    for (const auto& name : pluginNames)
        pluginFilterCombo_.addItem(juce::String(name), itemId++);

    // Restore previous selection if it still exists, else default to "All"
    if (previousId > 1 && previousId < itemId)
        pluginFilterCombo_.setSelectedId(previousId, juce::dontSendNotification);
    else
        pluginFilterCombo_.setSelectedId(1, juce::dontSendNotification);
}

void V2PresetBrowserPanel::updateCountLabel()
{
    const int total   = presetLibrary_.getPresetCount();
    const int showing = listModel_.numEntries();
    if (showing == total)
        countLabel_.setText(juce::String(total) + " preset" + (total == 1 ? "" : "s"),
                            juce::dontSendNotification);
    else
        countLabel_.setText(juce::String(showing) + " / " + juce::String(total)
                                + " preset" + (total == 1 ? "" : "s"),
                            juce::dontSendNotification);
}

const PresetEntry* V2PresetBrowserPanel::selectedEntry() const noexcept
{
    return listModel_.getEntry(presetList_.getSelectedRow());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Button actions
// ═══════════════════════════════════════════════════════════════════════════════

void V2PresetBrowserPanel::onSaveClicked()
{
    // Build a simple dialog with name, author, and tags fields.
    auto dialog = std::make_unique<juce::AlertWindow>(
        "Save Preset",
        "Enter preset details:",
        juce::MessageBoxIconType::NoIcon);

    dialog->addTextEditor("name",   "", "Name:");
    dialog->addTextEditor("author", "", "Author:");
    dialog->addTextEditor("tags",   "", "Tags (comma-separated):");
    dialog->addButton("Save",   1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    // Capture raw pointer before moving into lambda.
    juce::AlertWindow* dlgPtr = dialog.get();

    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, dlgPtr](int result)
            {
                if (result != 1)
                    return;

                const juce::String name   = dlgPtr->getTextEditorContents("name").trim();
                const juce::String author = dlgPtr->getTextEditorContents("author").trim();
                const juce::String tagsRaw = dlgPtr->getTextEditorContents("tags").trim();

                if (name.isEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Invalid Name",
                        "Please enter a preset name.");
                    return;
                }

                // Parse comma-separated tags
                std::vector<std::string> tags;
                juce::StringArray tagArr;
                tagArr.addTokens(tagsRaw, ",", "\"");
                for (const auto& t : tagArr)
                {
                    const juce::String trimmed = t.trim();
                    if (trimmed.isNotEmpty())
                        tags.push_back(trimmed.toStdString());
                }

                PresetEntry entry;
                entry.id     = PresetLibrary::generateUUID();
                entry.name   = name.toStdString();
                entry.author = author.toStdString();
                entry.tags   = std::move(tags);
                entry.rating = 0;
                entry.createdTimestamp  = juce::Time::currentTimeMillis() / 1000LL;
                entry.modifiedTimestamp = entry.createdTimestamp;

                // Serialize current processor state (snapshots + morph config) to JSON
                juce::var stateJson = PresetSerializer::serialize(
                    proc_.getSnapshotBank(),
                    proc_.getAPVTS());
                entry.jsonData = juce::JSON::toString(stateJson).toStdString();

                if (presetLibrary_.save(entry))
                {
                    refresh();
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Save Failed",
                        "Could not save the preset. Check the storage directory.");
                }
            }),
        true /* deleteWhenDismissed */);

    // Transfer ownership to modal state (JUCE manages lifetime).
    dialog.release();
}

void V2PresetBrowserPanel::onLoadClicked()
{
    const PresetEntry* sel = selectedEntry();
    if (sel == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "No Selection",
            "Please select a preset to load.");
        return;
    }

    PresetEntry loaded;
    if (!presetLibrary_.load(sel->id, loaded))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Failed",
            "Could not load preset \"" + juce::String(sel->name) + "\".");
        return;
    }

    // Deserialize and apply the preset state
    if (!loaded.jsonData.empty())
    {
        juce::var stateJson;
        if (juce::JSON::parse(juce::String(loaded.jsonData), stateJson).wasOk())
        {
            if (!PresetSerializer::deserialize(stateJson,
                                               proc_.getSnapshotBank(),
                                               proc_.getAPVTS()))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Load Failed",
                    "Could not parse preset data for \"" + juce::String(sel->name) + "\".");
                return;
            }
        }
    }
}

void V2PresetBrowserPanel::onDeleteClicked()
{
    const PresetEntry* sel = selectedEntry();
    if (sel == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "No Selection",
            "Please select a preset to delete.");
        return;
    }

    const juce::String presetName(sel->name);
    const std::string  presetId  = sel->id;

    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon,
        "Delete Preset",
        "Delete \"" + presetName + "\"? This cannot be undone.",
        "Delete",
        "Cancel",
        nullptr,
        juce::ModalCallbackFunction::create(
            [this, presetId, presetName](int result)
            {
                if (result != 1)
                    return;

                if (!presetLibrary_.remove(presetId))
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Delete Failed",
                        "Could not delete \"" + presetName + "\".");
                    return;
                }

                refresh();
            }));
}

void V2PresetBrowserPanel::onImportClicked()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import Preset",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.json");

    chooser->launchAsync(
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (!result.existsAsFile())
                return;

            if (!presetLibrary_.importFromFile(result))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Import Failed",
                    "Could not import \"" + result.getFileName() + "\".\n"
                    "The file may be malformed or missing required fields.");
                return;
            }

            refresh();
        });
}

void V2PresetBrowserPanel::onExportClicked()
{
    const PresetEntry* sel = selectedEntry();
    if (sel == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "No Selection",
            "Please select a preset to export.");
        return;
    }

    const std::string exportId   = sel->id;
    const juce::String safeName  = juce::String(sel->name)
                                       .replaceCharacter('/', '_')
                                       .replaceCharacter('\\', '_');

    auto chooser = std::make_shared<juce::FileChooser>(
        "Export Preset",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeName + ".json"),
        "*.json");

    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::warnAboutOverwriting,
        [this, exportId, chooser](const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (result == juce::File{})
                return;

            if (!presetLibrary_.exportToFile(exportId, result))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Export Failed",
                    "Could not write to \"" + result.getFullPathName() + "\".");
            }
        });
}

} // namespace more_phi
