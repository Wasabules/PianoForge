#pragma once

#include "AudioExport.h"
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
#include "ui/TakesPanel.h"
#include "UpdateChecker.h"
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
    void openPedalCurve();
    // Instrument presets (the whole setup, named) and the settings' backup.
    static const juce::StringArray& instrumentKeys();
    juce::Array<juce::var> instrumentPresets();
    void reloadSettings();          // every live object from the settings file
    void askSaveInstrument();
    void applyInstrument(int index);
    void askDeleteInstrument(int index);
    void exportSettings();
    void importSettings();
    std::unique_ptr<juce::FileChooser> settingsChooser;
    void updateRenderedLayers();
    void showWindow(std::unique_ptr<ToolWindow>& window, const juce::String& title, juce::Component& content);
    void showShortcuts();
    void refreshRoomButton();
    void showAbout();
    void setKeyLayout(int layout);
    void shiftOctave(int delta);
    void sendSustain(bool down);
    void sendSostenuto(bool down);
    void sendSoftPedal(bool down);
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
    pf::PedalCurve pedalCurve;                                   // the MIDI keyboard's sustain pedal, through it
    pf::PedalCurve::Settings pedalSettings = pf::PedalCurve::defaults();
    KeyboardInput keyboardInput { host.collector, velocityCurve, &pedalCurve }; // hardware keyboards go through the curve
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
    std::unique_ptr<ToolWindow> engineWindow, audioWindow, performanceWindow, roomWindow, takesWindow;
    std::unique_ptr<TakesPanel> takesPanel;
    void openTakes();
    void toggleHeadphones();
    // Updates (Help menu): where to look and whether to look at start are settings; without an address, nothing is
    // checked. A newer version is offered, downloaded and checked in the background, then installed once closed.
    void checkForUpdates(bool manual);
    void askUpdateSettings();
    void offerUpdate(const UpdateChecker::Release& release);
    void installUpdate(const UpdateChecker::Release& release);
    std::thread updateThread;
    std::atomic<bool> updateBusy { false }, updateCancel { false };
    /// Renders `sequence` (seconds) with the engine heard, its settings, the room and the limiter as they are now, to
    /// `wavFile`, in the background; then `done(ok)` on the message thread.
    void renderInBackground(juce::MidiMessageSequence sequence, double rate, juce::File wavFile, juce::String summary,
                            std::function<void(bool)> done = {});
    std::unique_ptr<PlayerBar> playerBar;
    std::unique_ptr<VelocityCurveWindow> curveWindow, pedalWindow;

    // Toolbar. The pedal button fills with the pedal's travel, as the engine receives it (a half pedal shows).
    class PedalButton final : public juce::TextButton {
    public:
        PedalButton() : juce::TextButton("Pedal") {}
        void setDepth(float d)
        {
            if (std::abs(d - depth) > 0.004f) {
                depth = d;
                repaint();
            }
        }
        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), over, down);
            const auto r = getLocalBounds().toFloat().reduced(1.5f);
            g.setColour(findColour(juce::TextButton::buttonOnColourId));
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * juce::jlimit(0.0f, 1.0f, depth)), 4.0f);
            g.setColour(findColour(depth > 0.55f ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId));
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(depth > 0.0f && depth < 1.0f ? "Pedal " + juce::String(juce::roundToInt(depth * 100.0f)) + " %" : juce::String("Pedal"),
                       getLocalBounds(), juce::Justification::centred);
        }
    private:
        float depth = 0.0f;
    };
    juce::TextButton referenceButton { "Reference" }, modalButton { "Modal" }, recordButton, roomButton { "Room" };
    PedalButton pedalButton;
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
    bool recordMidi = true;
    std::vector<SoundFormat> recordFormats { SoundFormat::wavFloat }; // the sound's formats (File > Record formats)
    bool recordsFormat(SoundFormat f) const { return std::find(recordFormats.begin(), recordFormats.end(), f) != recordFormats.end(); }
    void toggleRecordFormat(SoundFormat f);
    juce::String recordingExtensions() const;
    RecordingExporter exporter; // the formats made from the live recording once it stops
    juce::String recordingStem, lastSaved;
    double messageUntil = 0.0;
    int octave = 4;             // scientific octave of the first computer-keyboard note (C4 = middle C)
    bool spaceDown = false, pedalFromSpace = false, sostenutoKeyDown = false, softKeyDown = false, focusGiven = false, bankInstalled = false;
    bool constructed = false;   // no saving while the settings are being read back
    // The audio device asked for (the saved one, or the last one chosen) and, when it could not be opened at start
    // (a driver still held by a closing application, another program using it), the one used meanwhile: the
    // choice is then neither saved over nor forgotten, and tried again for a while.
    std::unique_ptr<juce::XmlElement> wantedAudio;
    juce::String fallbackType, fallbackName, lastDeviceType;
    bool audioFallback = false;
    int audioRetries = 0;
    double nextAudioRetry = 0.0;
    juce::AudioDeviceManager::AudioDeviceSetup preferredAudio;
    void noteAudioDevice();       // after a device change: the choice kept, the fallback ended, the per-type memory
    void retryWantedAudio();
    bool opensWanted() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace pf::app
