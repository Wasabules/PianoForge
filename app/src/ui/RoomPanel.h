#pragma once

#include <pianoforge/room.hpp>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// The room around the piano (pf::Room) and where it is heard from, in its own window: on or off, a room preset and
/// its settings (a trim of the reflections against the physics, the volume, the decay, the brightness, the
/// reverberation's width), and the listening position (a preset or a distance, and keep loudness), with what the
/// physics gives there. Created with the application: the settings apply from the start, the window open or not,
/// each saved as it changes. The room is applied at the output, whichever engine plays: what is heard, recorded
/// and streamed has it.
class RoomPanel final : public juce::Component, private juce::Timer {
public:
    RoomPanel(pf::Room& room, juce::PropertiesFile& settings);

    bool isOn() const { return onButton.getToggleState(); }
    void setOn(bool on);
    /// "Concert hall, at the keyboard (1 m)".
    juce::String presetName() const;
    std::function<void()> onChange; // the room switched on or off, another preset or position (the toolbar follows)

    void paint(juce::Graphics&) override;
    void resized() override;

    struct Preset {
        const char* name;
        float size, decay, brightness, width;
    };
    static const std::array<Preset, 5>& presets();
    struct Position {
        const char* name;
        float distance;
    };
    static const std::array<Position, 5>& positions();

private:
    enum Control { level, size, decay, brightness, width, distance, count };

    void timerCallback() override; // the summary, once a drag settles
    void apply();
    void save();
    void loadPreset(int index);
    void edited();         // a room setting moved away from the preset: "Custom"
    void positionEdited(); // the distance moved away from the position: "Custom"
    void describe();

    pf::Room& room;
    juce::PropertiesFile& settings;
    juce::ToggleButton onButton { "Room" }, keepLoudness { "Keep loudness" };
    juce::ComboBox preset, position;
    std::array<juce::Slider, count> sliders;
    std::array<juce::Label, count> labels;
    juce::Label presetLabel, positionLabel, summary, note;
    juce::Rectangle<int> roomCard, positionCard;
    bool loading = false;
};

} // namespace pf::app
