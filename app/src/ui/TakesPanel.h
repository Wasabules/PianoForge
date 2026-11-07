#pragma once

#include "../TakePlayback.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pf::app {

/// The takes and recordings, in one list: those saved with Ctrl+S (Takes folder) and those recorded with Ctrl+R
/// (Recordings folder), each with its date, length, notes and files. A take is listened to (one, or two in A/B
/// from the same instant), played again through the engine from its MIDI, rendered again with the settings heard
/// now, renamed, shown in its folder, or deleted (to the recycle bin).
class TakesPanel final : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
    struct Hooks {
        std::function<void(const juce::File&)> playMidi;      // the MIDI through the engine (the file player)
        std::function<void(const juce::File&)> renderAgain;   // a new WAV from the MIDI, the settings heard now
    };

    struct Entry {
        juce::String stem, kind;             // the files' common name; "Take" or "Recording"
        juce::File folder;
        juce::Time time;
        double seconds = 0.0;
        int notes = -1;                      // -1: no MIDI
        std::vector<juce::File> files;
        juce::File audio() const;            // the best one to listen to (lossless first)
        juce::File midi() const;
        juce::String extensions() const;
    };

    TakesPanel(TakePlayback& playback, std::vector<juce::File> folders, Hooks hooks);
    ~TakesPanel() override;

    /// Reads the folders again (a take saved, a render done).
    void refresh();

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    // TableListBoxModel
    int getNumRows() override { return (int) entries.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void sortOrderChanged(int column, bool forwards) override;

    void timerCallback() override;
    std::vector<int> selection() const;
    void listen();
    void listenAB();
    void renameSelected();
    void deleteSelected();
    void updateButtons();
    juce::String clock(double seconds) const;

    TakePlayback& playback;
    std::vector<juce::File> folders;
    Hooks hooks;
    std::vector<Entry> entries;
    int sortColumn = 3;
    bool sortForwards = false; // newest first
    juce::TableListBox table { "Takes", this };
    juce::TextButton playButton { "Listen" }, stopButton { "Stop" }, abButton { "A/B" }, sideButton { "Hearing A" },
        midiButton { "Play with the engine" }, renderButton { "Render again" }, renameButton { "Rename..." },
        deleteButton { "Delete" }, showButton { "Show in folder" }, refreshButton { "Refresh" };
    juce::Slider position;
    juce::Label timeLabel, nowLabel, note;
    bool dragging = false;
};

} // namespace pf::app
