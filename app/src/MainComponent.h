#pragma once

#include "AudioHost.h"
#include "EngineSwitch.h"
#include "KeyboardInput.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "TakeRecorder.h"
#include "VelocityCurveEditor.h"
#include "ui/AudioPanel.h"
#include "ui/EnginePanel.h"
#include "ui/Meters.h"
#include "ui/PerformancePanel.h"
#include "ui/PlayerBar.h"
#include "ui/RoomPanel.h"
#include "ui/ToolWindow.h"

#include <pianoforge/modal_engine.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <thread>

namespace pf::app {

/// The main window: the menus, a toolbar (engine, recording, volume, velocity, pedal, audio load, level), a
/// dashboard of four cards (engine, last note, audio, session), the MIDI file player, the 88-key keyboard and a
/// status bar. A MIDI file opens from the menus or by dropping it on the window. The
/// settings of the engines, the audio and streaming, the velocity curve and the performance details have their
/// own windows (menus Engine, Audio and Keyboard).
class MainComponent final : public juce::Component,
                            public juce::MenuBarModel,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer,
                            private juce::KeyListener,
                            private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int id, int) override;

    /// Opens a MIDI file in the player (menus, drag and drop, or the command line: "Open with" PianoForge).
    void openMidiFile(const juce::File& file);

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { playerBar->setDropHighlight(true); }
    void fileDragExit(const juce::StringArray&) override { playerBar->setDropHighlight(false); }

private:
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged(bool isKeyDown, juce::Component*) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // Actions.
    void selectEngine(bool modal);
    bool modalSelected() const { return engines.selected.load() == 1; }
    void toggleRecording();
    void finishRecording();
    void saveLastTake();
    void chooseCorpus();
    void loadCorpus(const juce::File& folder);
    void loadModalProfile();
    void chooseMidiFile();
    void closeMidiFile();
    void openVelocityCurve();
    void updateRenderedLayers();
    void showWindow(std::unique_ptr<ToolWindow>& window, const juce::String& title, juce::Component& content);
    void showShortcuts();
    void refreshRoomButton();
    void showAbout();
    void setKeyLayout(int layout);
    void shiftOctave(int delta);
    void sendSustain(bool down);
    void sendSostenuto(bool down);
    void enableMidiInputs();
    void saveSettings();
    void showMessage(const juce::String& text, juce::Colour colour = theme::text);
    static juce::File takesFolder();
    static juce::File recordingsFolder();

    // The dashboard, refreshed by the timer.
    void refreshDashboard();
    juce::StringArray engineLines() const;
    juce::StringArray deviceLines() const;
    juce::StringArray adviceLines() const;
    juce::String midiInputs() const;

    juce::ApplicationProperties properties;
    juce::AudioDeviceManager deviceManager;
    ReferencePlayer player;
    pf::ModalEngine modal;
    EngineSwitch engines { player, modal };
    AudioHost host { engines };
    pf::VelocityCurve velocityCurve;
    pf::VelocityCurve::Settings curveSettings;
    KeyboardInput keyboardInput { host.collector, velocityCurve }; // hardware keyboards go through the curve
    TakeRecorder takes;                 // every event that reached the engine (message thread)
    std::vector<RecordedEvent> drained; // reused buffer
    std::thread takeRender;             // renders a saved take to WAV in the background
    std::atomic<bool> renderingTake { false };
    BankLoader loader;
    std::thread modalLoader;            // loads modal-profile.bin in the background
    juce::String corpusSummary = "Corpus: none", profileSummary = "Modal profile: not loaded";
    juce::MidiDeviceListConnection midiDevicesConnection;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File corpusFolder;
    LoadTracker loadTracker;

    // Panels (created with the application: their settings apply from the start) and their windows.
    std::unique_ptr<EnginePanel> enginePanel;
    std::unique_ptr<AudioPanel> audioPanel;
    PerformancePanel performancePanel;
    std::unique_ptr<RoomPanel> roomPanel;
    std::unique_ptr<ToolWindow> engineWindow, audioWindow, performanceWindow, roomWindow;
    std::unique_ptr<PlayerBar> playerBar;
    std::unique_ptr<VelocityCurveWindow> curveWindow;

    // Toolbar.
    juce::TextButton referenceButton { "Reference" }, modalButton { "Modal" }, recordButton, pedalButton { "Pedal" },
        roomButton { "Room" };
    juce::Slider volume, velocity;
    juce::Label volumeLabel, velocityLabel;
    LoadMeter loadMeter;
    LevelMeter levelMeter;
    juce::Rectangle<int> toolbarArea;

    // Dashboard, keyboard, status bar.
    InfoCard engineCard { "Engine" }, noteCard { "Last note" }, audioCard { "Audio" }, sessionCard { "Session" };
    juce::MidiKeyboardComponent keyboard { host.keyboardState, juce::KeyboardComponentBase::horizontalKeyboard };
    juce::Label hint, statusLeft, statusRight;
    double loadProgress = 0.0;
    juce::ProgressBar progressBar { loadProgress };
    juce::Rectangle<int> statusArea;

    int keyLayout = 1;          // 1 AZERTY, 2 QWERTY
    bool recordMidi = true, recordWav = true;
    juce::String recordingStem, lastSaved;
    double messageUntil = 0.0;
    int octave = 4;             // scientific octave of the first computer-keyboard note (C4 = middle C)
    bool spaceDown = false, pedalFromSpace = false, sostenutoKeyDown = false, focusGiven = false, bankInstalled = false;
    bool constructed = false;   // no saving while the settings are being read back

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace pf::app
