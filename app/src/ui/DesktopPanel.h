#pragma once

#include "../DesktopIntegration.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace pf::app {

/// Settings > Desktop: the interface's size, and what PianoForge does with the desktop (DesktopIntegration): kept
/// awake and on the performance power profile while playing, the MIDI player in the media controls, notifications,
/// one PianoForge for the files opened, a MIDI input for other programs. Each with what it does now.
class DesktopPanel final : public juce::Component, private juce::Timer {
public:
    DesktopPanel(juce::PropertiesFile& settings, DesktopIntegration& integration);

    /// The interface's size saved (0: automatic), as a factor.
    static double savedScale(const juce::PropertiesFile& settings);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Row {
        const char* key;
        juce::ToggleButton toggle;
        juce::Label status;
        bool shown = true;
    };

    juce::PropertiesFile& settings;
    DesktopIntegration& integration;
    juce::Label scaleLabel, scaleNote, playingNote;
    juce::ComboBox scale;
    std::array<Row, 6> rows; // keepAwake, performanceProfile, mediaControls, notifyRecordings, singleInstance, virtualMidiInput
    juce::Rectangle<int> interfaceArea, playingArea, desktopArea;
};

} // namespace pf::app
