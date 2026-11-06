#pragma once

#include "AudioHost.h"
#include "EngineSwitch.h"
#include "KeyboardInput.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "TakeRecorder.h"
#include "Tuning.h"
#include "VelocityCurveEditor.h"

#include <pianoforge/modal_engine.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <thread>

namespace pf::app {

/// Listening lab: play the reference corpus or the modal engine (switchable while playing) with the mouse, the
/// computer keyboard or a MIDI keyboard.
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::KeyListener,
                            private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged(bool isKeyDown, juce::Component*) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void openAudioSettings();
    void openVelocityCurve();
    void saveLastTake();
    static juce::File takesFolder();
    void toggleRecording();
    void finishRecording();
    static juce::File recordingsFolder();
    void updateRenderedLayers();
    void chooseCorpus();
    void loadCorpus(const juce::File& folder);
    void loadModalProfile();
    bool modalSelected() const { return engineChoice.getSelectedId() == kEngineModal; }
    void enableMidiInputs();
    void applyKeyLayout();
    void sendSustain(bool down);
    void saveSettings();
    void applyTuning();
    juce::String audioDescription() const;

    juce::ApplicationProperties properties;
    juce::AudioDeviceManager deviceManager;
    static constexpr int kEngineReference = 1, kEngineModal = 2;
    ReferencePlayer player;
    pf::ModalEngine modal;
    EngineSwitch engines { player, modal };
    AudioHost host { engines };
    pf::VelocityCurve velocityCurve;
    pf::VelocityCurve::Settings curveSettings;
    KeyboardInput keyboardInput { host.collector, velocityCurve }; // hardware keyboards go through the curve
    std::unique_ptr<VelocityCurveWindow> curveWindow;
    TakeRecorder takes;                  // every event that reached the engine (message thread)
    std::vector<RecordedEvent> drained;  // reused buffer
    std::thread takeRender;              // renders a saved take to WAV in the background
    std::atomic<bool> renderingTake { false };
    BankLoader loader;
    std::thread modalLoader;             // loads modal-profile.bin in the background
    juce::String modalProfileText = "Modal profile: not loaded";
    juce::MidiDeviceListConnection midiDevicesConnection;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File corpusFolder;

    juce::MidiKeyboardComponent keyboard { host.keyboardState, juce::KeyboardComponentBase::horizontalKeyboard };
    juce::Label title, corpusStatus, modalStatus, audioStatus, noteStatus, engineStatus, help, takeStatus;
    juce::ComboBox engineChoice;
    juce::ToggleButton bodyButton, residualButton;
    juce::ToggleButton sympatheticButton, heldKeysButton, pedalCatchButton, releaseNoiseButton;
    juce::Label tuningLabel;
    juce::Slider referenceA4;
    juce::ComboBox temperamentChoice, temperamentRoot;
    juce::TextButton audioButton, corpusButton, curveButton, saveTakeButton, takesFolderButton;
    juce::TextButton recordButton, recordingsFolderButton;
    juce::ToggleButton recordMidiButton, recordWavButton;
    juce::Label recordStatus;
    juce::String recordingStem;
    juce::ToggleButton sustainButton, wholeTakeButton;
    juce::Slider volume, velocity, restrikeFade;
    juce::Label volumeLabel, velocityLabel, restrikeLabel;
    juce::ComboBox keyLayout;
    double loadProgress = 0.0;
    juce::ProgressBar progressBar { loadProgress };
    juce::Rectangle<int> peakArea;

    int octave = 4;              // scientific octave of the first computer-keyboard note (C4 = middle C)
    bool spaceDown = false;
    bool focusGiven = false;
    float displayedPeak = 0.0f;
    bool bankInstalled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace pf::app
