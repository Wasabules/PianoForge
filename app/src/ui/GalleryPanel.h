#pragma once

#include "../Gallery.h"
#include "../TakePlayback.h"
#include "StarRating.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace pf::app {

/// The gallery of everything recorded (Ctrl+R) and saved (Ctrl+S): on the left the collections (all, recordings,
/// takes, not named yet, then each piece); on top the search and the filters (formats, date, rating, length, size);
/// in the middle the list, sorted by any column; on the right what is known of the selection (piece, composer, tags,
/// rating, notes), applied to every item selected, its files renamed and filed by the name pattern; below, listening
/// (one, or two in A/B), playing through the engine, rendering again, renaming, deleting (to the recycle bin).
class GalleryPanel final : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
    struct Hooks {
        std::function<void(const juce::File&)> playMidi;    // the MIDI through the engine (the file player)
        std::function<void(const juce::File&)> renderAgain; // a new WAV from the MIDI, the settings heard now
        std::function<GalleryNaming()> naming;              // the name pattern (Settings > Recording)
        std::function<void()> openNamingSettings;
    };

    /// `roots`: the folders and whether each holds recordings (else takes).
    GalleryPanel(TakePlayback& playback, std::vector<std::pair<juce::File, bool>> roots, Hooks hooks);
    ~GalleryPanel() override;

    /// Reads the folders again (a recording named, a conversion or a render done); the selection kept.
    void refresh();

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    class Sidebar;

    // TableListBoxModel
    int getNumRows() override { return (int) shown.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void sortOrderChanged(int column, bool forwards) override;
    juce::String getCellTooltip(int row, int column) override;

    void timerCallback() override;
    void applyFilters();
    void sortShown();
    void readFilters();
    std::vector<GalleryItem*> selected();
    void showDetails();
    void applyDetails();
    void listen();
    void listenAB();
    void renameSelected();
    void deleteSelected();
    void updateButtons();
    juce::File rootOf(const GalleryItem& item) const;
    int numberOf(const GalleryItem& item, const juce::String& piece) const; // its rank among the piece's (by date)
    /// A value already in the gallery, from a menu; `then` once one is chosen.
    void chooseFrom(juce::TextEditor& field, const juce::StringArray& values, bool& edited, std::function<void()> then);

    TakePlayback& playback;
    std::vector<std::pair<juce::File, bool>> roots;
    Hooks hooks;
    std::vector<GalleryItem> items;
    std::vector<int> shown; // indices into items: filtered, sorted
    GalleryFilter filter;
    int sortColumn = 6;
    bool sortForwards = false; // newest first

    // The collections.
    std::unique_ptr<Sidebar> sidebar;
    // The filters.
    juce::TextEditor search;
    std::array<juce::ToggleButton, 5> types; // MIDI, WAV, MP3, FLAC, Opus
    juce::ComboBox dateFilter, ratingFilter;
    juce::Slider lengthFilter, sizeFilter;
    juce::Label lengthLabel, sizeLabel, countLabel;
    juce::TextButton resetFilters { "Reset" };
    // The list.
    juce::TableListBox table { "Gallery", this };
    // The details of the selection.
    juce::Label detailsHeading, pieceLabel, composerLabel, tagsLabel, ratingLabel, notesLabel, filesInfo;
    juce::TextEditor piece, composer, tags, notes;
    juce::TextButton pieceMenu { "..." }, composerMenu { "..." };
    StarRating rating;
    juce::ToggleButton renameByPattern { "Name and file by the pattern" };
    juce::TextButton applyButton { "Apply" }, revertButton { "Revert" }, patternButton { "Pattern..." };
    bool pieceEdited = false, composerEdited = false, tagsEdited = false, ratingEdited = false, notesEdited = false;
    bool loadingDetails = false;
    // Listening and actions.
    juce::TextButton playButton { "Listen" }, stopButton { "Stop" }, abButton { "A/B" }, sideButton { "Hearing A" },
        midiButton { "Play with the engine" }, renderButton { "Render again" }, renameButton { "Rename..." }, deleteButton { "Delete" },
        showButton { "Show in folder" }, refreshButton { "Refresh" };
    juce::Slider position;
    juce::Label timeLabel, nowLabel;
    bool dragging = false;
    juce::Rectangle<int> filterArea, detailsArea;
};

} // namespace pf::app
