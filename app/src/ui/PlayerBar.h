#pragma once

#include "../MidiFilePlayer.h"
#include "LevelStrip.h"
#include "Theme.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pf::app {

/// A transport button drawn by hand (the system fonts do not all have the symbols).
class TransportButton final : public juce::Button {
public:
    enum class Icon { play, pause, stop, previous, next, back, forward };
    TransportButton(const juce::String& name, Icon i) : juce::Button(name), icon(i) {}
    void setIcon(Icon i)
    {
        if (i != icon) {
            icon = i;
            repaint();
        }
    }
    void paintButton(juce::Graphics&, bool over, bool down) override;

private:
    Icon icon;
};

/// The mini MIDI player, under the dashboard, on three rows: open, the previous file, 5 s back, play / pause, stop,
/// 5 s on, the next file, the file (its place in the list), its parts (the instruments to play) and the
/// transposition; then the position (click or drag to move), the time, the speed, auto-play and repeat; then the
/// level along the piece (LevelStrip: from its recording, else its notes). The list: the MIDI files of the folder
/// opened from, or those the gallery shows. Auto-play starts a file as it is opened and goes on to the next one at
/// the end; repeat: the list (from its start again) or the file. The file plays through the audio host, to the
/// sample, with what the keyboards play (and is recorded with it). Speed, auto-play and repeat are kept in the
/// settings; the parts and the transposition go with the file.
class PlayerBar final : public juce::Component {
public:
    PlayerBar(MidiFilePlayer& p, juce::PropertiesFile& s);

    std::function<void()> onOpen;    // the Open button
    std::function<void()> onClicked; // after any click on the bar (the keyboard takes the focus back)
    /// Loads a file of the list (the previous, the next one); false when it cannot be read.
    std::function<bool(const juce::File&)> onLoadFile;

    enum class Repeat { off, all, one };
    /// The files to go through with previous / next, and the one opened among them.
    void setPlaylist(std::vector<juce::File> files, const juce::File& current);
    void next(bool fromEnd = false);
    void previous();
    bool autoPlays() const { return autoButton.getToggleState(); }
    /// Whether next() and previous() lead somewhere (the desktop's media controls show it); the file loaded.
    bool hasNext() const { return playlistIndex >= 0 && playlistIndex + 1 < (int) playlist.size(); }
    bool hasPrevious() const { return playlistIndex >= 0; } // (the previous file, or the start of this one)
    juce::File file() const { return currentFile(); }

    void togglePlay();
    void stop();
    void nudge(double seconds);      // moves the position by `seconds` (negative: back)
    void setSpeed(double factor);    // also updates the slider and saves
    void setLoop(bool on);           // repeat the file, or not
    void setRepeat(Repeat r);
    void setDropHighlight(bool on);
    void transposeBy(int semitones); // relative to the current transposition
    /// The timer: position, time, state.
    void refresh();
    /// After a file is loaded or closed (newFile), or its parts changed.
    void fileChanged(bool newFile = true);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void save();
    void showParts();
    void showTranspose();
    bool openAt(int index, bool play);
    juce::File currentFile() const;

    MidiFilePlayer& player;
    juce::PropertiesFile& settings;
    juce::TextButton openButton { "Open MIDI..." };
    TransportButton playButton { "Play", TransportButton::Icon::play }, stopButton { "Stop", TransportButton::Icon::stop },
        previousButton { "Previous", TransportButton::Icon::previous }, nextButton { "Next", TransportButton::Icon::next },
        backButton { "Back", TransportButton::Icon::back }, forwardButton { "Forward", TransportButton::Icon::forward };
    juce::Label nameLabel, timeLabel, speedLabel;
    juce::Slider position, speed;
    juce::ToggleButton autoButton { "Auto-play" };
    juce::TextButton repeatButton { "Repeat: off" }, slower { "-" }, faster { "+" };
    Repeat repeat = Repeat::off;
    LevelStrip strip;
    std::vector<juce::File> playlist;
    int playlistIndex = -1;
    bool wasPlaying = false;
    juce::TextButton partsButton { "Tracks..." };
    juce::Label transposeLabel, transposeValue;
    juce::TextButton octaveDown { "-oct" }, semitoneDown { "-" }, semitoneUp { "+" }, octaveUp { "+oct" };
    bool dragging = false, dropHighlight = false, ready = false; // ready: the settings are read back
};

} // namespace pf::app
