#pragma once

#include "DesktopBus.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>

namespace pf::app {

/// The desktop's side of the application, each part a setting of Settings > Desktop, on by default:
/// - keepAwake: the screen on and no suspend while playing (all systems);
/// - performanceProfile (Linux): the "performance" power profile while playing (steadier small blocks);
/// - mediaControls (Linux): the MIDI player in the desktop's media controls (MPRIS: media keys, the shell's widget,
///   a headset's buttons);
/// - notifyRecordings (Linux): a notification when a recording is saved while PianoForge is in the background;
/// - virtualMidiInput (Linux, macOS): a MIDI input named "PianoForge" that other programs play (a DAW, a score editor,
///   a sequencer): their notes reach the engine as they are, without the keyboard's curves;
/// - singleInstance (Linux): read at start by the application (SingleInstance.h); shown here.
/// "Playing": a note sounding in the last ten minutes, a MIDI file playing, or a recording. The interface's size
/// (interfaceScale) is applied by the application, through juce::Desktop's scale.
class DesktopIntegration {
public:
    struct Hooks {
        std::function<bool()> sounding;               // notes sounding, a file playing, or a recording, now
        std::function<desktop::PlayerState()> player; // the MIDI player's state
        desktop::MediaControls::Commands commands;    // what the media controls do
        juce::MidiInputCallback* virtualInput = nullptr; // where the virtual input's messages go
    };

    DesktopIntegration(juce::PropertiesFile& settings, Hooks hooks);
    ~DesktopIntegration();

    static constexpr double kPlayingSeconds = 600.0;

    /// Message thread, a few times a second: what is held, what the media controls show.
    void tick();
    /// The settings changed: each part started or stopped accordingly.
    void apply();
    /// Something was saved (`what`: as the status bar says it): a notification when PianoForge is in the background.
    void saved(const juce::String& what);
    /// Set by the application: whether this instance answers later starts.
    void setInstanceAnswering(bool answering) { instanceAnswering = answering; }

    struct Status {
        juce::String keepAwake, performance, media, notifications, instance, midi;
    };
    Status status() const;

private:
    juce::PropertiesFile& settings;
    Hooks hooks;
    desktop::KeepAwake awake;
    desktop::PerformanceProfile performance;
    std::unique_ptr<desktop::MediaControls> media;
    std::unique_ptr<juce::MidiInput> virtualInput;
    double lastSound = -1.0e12; // ms
    bool instanceAnswering = false;
    juce::String notificationResult;

    bool playing() const;
};

} // namespace pf::app
