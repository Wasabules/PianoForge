#pragma once

#include "../KeyboardInput.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// What a piano gesture can do (Keyboard > Piano gestures): in PianoForge, and in OBS through the plugin.
enum class PianoAction : int {
    record,           // arm a recording (it starts with the first note), or stop it (before the gesture's notes)
    saveTake,         // save the last take (without the gesture's notes)
    playPause,        // play or pause the MIDI file
    obsRecord,        // OBS: start or stop the recording
    obsStream,        // OBS: start or stop the stream
    obsChapter,       // OBS: a chapter marker in the recording
    obsNextScene,     // OBS: the next scene
    obsPreviousScene, // OBS: the previous scene
    count
};

struct PianoActionInfo {
    const char* key;            // in the settings: "gesture.<key>"
    const char* name;           // in the interface
    const char* defaultGesture; // PianoGesture::text(), "" for none
};

/// The actions, in PianoAction's order. The defaults: clusters at the ends of the keyboard, the end keys struck
/// twice (what no piece asks for); OBS's stream and scenes left to the user (going live by mistake costs too much).
inline const std::array<PianoActionInfo, (std::size_t) PianoAction::count>& pianoActions()
{
    static const std::array<PianoActionInfo, (std::size_t) PianoAction::count> actions { {
        { "record", "Record: arm (it starts with the first note) / stop", "chord:21,22,23" },
        { "saveTake", "Save the last take", "double:108" },
        { "playPause", "Play / pause the MIDI file", "double:21" },
        { "obsRecord", "OBS: start / stop the recording", "chord:106,107,108" },
        { "obsStream", "OBS: start / stop the stream", "" },
        { "obsChapter", "OBS: a chapter marker", "" },
        { "obsNextScene", "OBS: next scene", "" },
        { "obsPreviousScene", "OBS: previous scene", "" },
    } };
    return actions;
}

/// The piano gestures in their own window: on or off, and per action its gesture, learned by playing it (keys struck
/// together, two to five, or one key struck twice quickly) or cleared. A gesture another action had moves here.
class GesturesPanel final : public juce::Component, private juce::Timer {
public:
    GesturesPanel(KeyboardInput& input, juce::PropertiesFile& settings);
    ~GesturesPanel() override;

    /// Reads the gestures from the settings and gives them to the keyboard input (at start, after an import).
    void reload();
    /// A short message for the status bar (a gesture learned, moved...).
    std::function<void(const juce::String&)> onMessage;
    /// The gestures changed (or were turned on or off).
    std::function<void()> onChanged;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Row {
        juce::Label name, gesture;
        juce::TextButton learn { "Learn" }, clear { "Clear" }, reset { "Default" };
    };
    void timerCallback() override;
    void startLearning(int action);
    void stopLearning();
    void assign(int action, const pf::PianoGesture& g);
    void refresh();

    KeyboardInput& input;
    juce::PropertiesFile& settings;
    juce::ToggleButton enabled { "Piano gestures on (from the MIDI keyboard)" };
    juce::Label intro;
    std::array<Row, (std::size_t) PianoAction::count> rows;
    int learning = -1;
};

} // namespace pf::app
