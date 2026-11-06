#pragma once

#include "../AudioHost.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>

namespace pf::app {

/// Audio, MIDI and streaming, in one window: the audio device and the MIDI inputs (JUCE's selector), the mirror
/// output (a copy of the sound on another device, for a virtual cable), and the state of the OBS plugin's feed.
/// Created with the application: the mirror opens from the saved setting as soon as the main device runs.
class AudioPanel final : public juce::Component, private juce::Timer {
public:
    AudioPanel(juce::AudioDeviceManager& devices, AudioHost& host, juce::PropertiesFile& settings);
    ~AudioPanel() override;

    juce::String mirrorSummary() const; // for the dashboard
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void fillMirrorDevices();
    void openMirror();
    void applyRateMode(int mode); // 0: below the device's rate, 1: at it; restarts the device

    juce::AudioDeviceManager& devices;
    AudioHost& host;
    juce::PropertiesFile& settings;
    juce::AudioDeviceSelectorComponent selector;
    juce::ComboBox mirrorChoice; // id 1: off, then the devices
    juce::StringArray mirrorDevices;
    juce::Label mirrorStatus, mirrorNote, feedStatus, feedNote;
    juce::TextButton refreshDevices { "Refresh" };
    juce::ComboBox rateChoice; // 1: the engine at 44.1 / 48 kHz under a faster device, 2: at the device's rate
    juce::Label rateStatus, rateNote;
    juce::Rectangle<int> deviceArea, rateArea, mirrorArea, feedArea;
};

} // namespace pf::app
