#pragma once

#include <pianoforge/room.hpp>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// The room around the piano (pf::Room), in its own window: on or off, a preset, and its six settings (level,
/// size, decay, brightness, pre-delay, width). Created with the application: the settings apply from the start,
/// the window open or not, each saved as it changes. The room is applied at the output, whichever engine plays:
/// what is heard, recorded and streamed has it.
class RoomPanel final : public juce::Component {
public:
    RoomPanel(pf::Room& room, juce::PropertiesFile& settings);

    bool isOn() const { return onButton.getToggleState(); }
    void setOn(bool on);
    juce::String presetName() const;
    std::function<void()> onChange; // the room switched on or off, or another preset (the toolbar follows)

    void paint(juce::Graphics&) override;
    void resized() override;

    struct Preset {
        const char* name;
        float size, decay, brightness, predelayMs, levelDb, width;
    };
    static const std::array<Preset, 5>& presets();

private:
    enum Control { level, size, decay, brightness, predelay, width, count };

    void apply();
    void save();
    void loadPreset(int index);
    void edited(); // a setting moved away from the preset: "Custom"

    pf::Room& room;
    juce::PropertiesFile& settings;
    juce::ToggleButton onButton { "Room" };
    juce::ComboBox preset;
    std::array<juce::Slider, count> sliders;
    std::array<juce::Label, count> labels;
    juce::Label presetLabel, note;
    juce::Rectangle<int> card;
    bool loading = false;
};

} // namespace pf::app
