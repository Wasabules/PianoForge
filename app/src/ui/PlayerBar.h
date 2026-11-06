#pragma once

#include "../MidiFilePlayer.h"
#include "Theme.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pf::app {

/// A transport button drawn by hand (the system fonts do not all have the symbols).
class TransportButton final : public juce::Button {
public:
    enum class Icon { play, pause, stop };
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

/// The mini MIDI player, under the dashboard: open, play / pause, stop, the position (click or drag to move),
/// the time, the speed and the loop. The file plays through the audio host, to the sample, with what the
/// keyboards play (and is recorded with it). Speed and loop are kept in the settings.
class PlayerBar final : public juce::Component {
public:
    PlayerBar(MidiFilePlayer& p, juce::PropertiesFile& s);

    std::function<void()> onOpen;    // the Open button
    std::function<void()> onClicked; // after any click on the bar (the keyboard takes the focus back)

    void togglePlay();
    void stop();
    void nudge(double seconds);      // moves the position by `seconds` (negative: back)
    void setSpeed(double factor);    // also updates the slider and saves
    void setLoop(bool on);
    void setDropHighlight(bool on);
    /// The timer: position, time, state.
    void refresh();
    /// After a file is loaded (or closed).
    void fileChanged();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void save();

    MidiFilePlayer& player;
    juce::PropertiesFile& settings;
    juce::TextButton openButton { "Open MIDI..." };
    TransportButton playButton { "Play", TransportButton::Icon::play }, stopButton { "Stop", TransportButton::Icon::stop };
    juce::Label nameLabel, timeLabel, speedLabel;
    juce::Slider position, speed;
    juce::ToggleButton loopButton { "Loop" };
    bool dragging = false, dropHighlight = false, ready = false; // ready: the settings are read back
};

} // namespace pf::app
