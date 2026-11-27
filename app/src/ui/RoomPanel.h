// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../RoomPresets.h"

#include <pianoforge/room.hpp>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// The room around the piano (pf::Room) and where it is heard from, in its own window: on or off, a room preset and
/// its settings (a trim of the reflections against the physics, the volume, the decay, the brightness, the
/// reverberation's width), and the listening position (a preset or a distance, keep loudness, and speakers or
/// headphones), with what the physics gives there. Created with the application: the settings apply from the start,
/// the window open or not, each saved as it changes. The room is applied at the output, whichever engine plays: what
/// is heard, recorded and streamed has it.
class RoomPanel final : public juce::Component, private juce::Timer {
public:
    RoomPanel(pf::Room& room, juce::PropertiesFile& settings);

    bool isOn() const { return onButton.getToggleState(); }
    void setOn(bool on);
    /// "Concert hall, 1.0 m".
    juce::String presetName() const;
    /// Every preset, for a menu: the built-in rooms, then the user's (a room and a listening position, named).
    juce::StringArray presetNames() const;
    int presetIndex() const;       // the selected one in presetNames(), -1: custom
    void selectPreset(int index);  // as if chosen in the list
    /// Heard on headphones (binaural: a spherical head), the room on or off; or on speakers.
    bool onHeadphones() const { return heardOn.getSelectedId() == kHeadphonesId; }
    void setHeadphones(bool headphones);
    /// Reads every setting back from the settings file and applies it (an instrument preset, an import).
    void reload();
    /// Writes every setting of the panel to the settings file now (a preset's snapshot).
    void saveNow() { save(); }
    std::function<void()> onChange; // the room switched on or off, another preset or position (the toolbar follows)

    void paint(juce::Graphics&) override;
    void resized() override;

    using Preset = RoomPreset; // the tables are shared with the plugin (RoomPresets.h)
    static const std::array<Preset, 5>& presets() { return kRoomPresets; }
    using Position = ListeningPosition;
    static const std::array<Position, 5>& positions() { return kListeningPositions; }

private:
    enum Control { level, size, decay, brightness, width, distance, count };
    static constexpr int kSpeakersId = 1, kHeadphonesId = 2;
    struct UserPreset {
        juce::String name;
        std::array<double, count> values {}; // as the sliders show them
        bool keepLoudness = true;
    };

    void timerCallback() override; // the summary, once a drag settles
    void apply();
    void save();
    void loadPreset(int index);
    void edited();         // a room setting moved away from the preset: "Custom"
    void positionEdited(); // the distance moved away from the position: "Custom"
    void describe();
    void loadUserPresets();
    void storeUserPresets();
    void rebuildPresetList(int selectId);
    void loadUserPreset(int index);
    void askSavePreset();
    void askDeletePreset();

    pf::Room& room;
    juce::PropertiesFile& settings;
    juce::ToggleButton onButton { "Room" }, keepLoudness { "Keep loudness" };
    juce::ComboBox preset, position, heardOn;
    juce::TextButton savePreset { "Save..." }, deletePreset { "Delete" };
    std::vector<UserPreset> userPresets;
    std::array<juce::Slider, count> sliders;
    std::array<juce::Label, count> labels;
    juce::Label presetLabel, positionLabel, heardOnLabel, summary, note;
    juce::Rectangle<int> roomCard, positionCard;
    bool loading = false;
};

} // namespace pf::app
