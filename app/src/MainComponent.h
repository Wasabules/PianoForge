#pragma once

#include "AudioExport.h"
#include "AudioHost.h"
#include "EngineSwitch.h"
#include "KeyboardInput.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "TakeRecorder.h"
#include "VelocityCurveEditor.h"
#include "ui/GestureKeyboard.h"
#include "ui/GesturesPanel.h"
#include "ui/StreamLookPanel.h"
#include "StreamLook.h"
#include "ui/AudioPanel.h"
#include "ui/EnginePanel.h"
#include "ui/TonePanel.h"
#include "ui/SettingsPanel.h"
#include "ui/Meters.h"
#include "ui/PerformancePanel.h"
#include "ui/PlayerBar.h"
#include "ui/RoomPanel.h"
#include "ui/GalleryPanel.h"
#include "ui/NamingDialog.h"
#include "ui/RecordingPanel.h"
#include "UpdateChecker.h"
#include "ui/ToolWindow.h"

#include <pianoforge/modal_engine.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <limits>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <deque>
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
    // Two modal profiles to compare on the same engine and settings: A, the corpus's (modal-profile.bin next to its
    // passes), and B, any profile file (another bank's, a former version). Ctrl+2 / Ctrl+3.
    void selectModalProfile(bool useB);
    void chooseProfileB();
    void loadProfileB(const juce::File& file, bool activate);
    void installActiveProfile();
    static juce::String describeProfile(const pf::ModalProfile& profile, const juce::File& file);
    juce::String bankName() const;
    void toggleRecording(std::int64_t notesFrom = -1);
    void finishRecording(std::int64_t at = -1);
    void saveLastTake(std::int64_t before = std::numeric_limits<std::int64_t>::max());
    void runGesture(int action, double start); // a piano gesture recognized (Keyboard > Piano gestures)
    juce::String gesturesHelp() const;
    void refreshGestureKeys(); // the gestures marked on the on-screen keyboard
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
    /// The instrument as it sounds now saved under `name` (one of the user's presets; an existing one replaced).
    void storeInstrument(const juce::String& name);
    void applyFactory(int index);   // a factory preset (the sound only)
    void revertPreset();            // the current preset applied again
    /// The preset now in use: its name, its kind and the values it stands for (after every panel wrote its own),
    /// to tell when the sound has moved away from it.
    void markPreset(const juce::String& name, bool factory);
    bool presetModified();
    int userPresetIndex(const juce::String& name);
    void showSettings(SettingsPanel::Section s);
    bool settingsShowing(SettingsPanel::Section s) const;
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
    std::shared_ptr<const pf::ModalProfile> profileA, profileB; // the corpus's profile, and the one compared with it
    juce::File profileBFile;
    bool profileBActive = false;        // the modal engine plays profile B
    std::thread profileBLoader;
    juce::String profileASummary = "not loaded", profileBSummary = "none";
    juce::MidiDeviceListConnection midiDevicesConnection;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File corpusFolder;
    LoadTracker loadTracker;

    // Panels (created with the application: their settings apply from the start) and their windows.
    std::unique_ptr<EnginePanel> enginePanel;
    std::unique_ptr<TonePanel> tonePanel;
    std::unique_ptr<AudioPanel> audioPanel;
    PerformancePanel performancePanel;
    std::unique_ptr<RoomPanel> roomPanel;
    std::unique_ptr<ToolWindow> performanceWindow, takesWindow, settingsWindow;
    std::unique_ptr<SettingsPanel> settingsPanel; // every setting: the panels above as its sections
    std::unique_ptr<GesturesPanel> gesturesPanel;
    std::unique_ptr<StreamLook> streamLook;           // the OBS keyboard's look, shared with OBS
    std::unique_ptr<StreamLookPanel> streamLookPanel; // its window's content
    int streamLookTicks = 0;
    std::unique_ptr<GalleryPanel> galleryPanel;
    std::unique_ptr<RecordingPanel> recordingPanel; // Settings > Recording and naming
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
    GestureKeyboard keyboard { host.keyboardState }; // the piano gestures marked on its keys
    juce::TooltipWindow tooltips { nullptr, 500 };    // the tooltips, in every window of the application
    juce::Label hint, statusLeft, statusRight;
    double loadProgress = 0.0;
    juce::ProgressBar progressBar { loadProgress };
    juce::Rectangle<int> statusArea;

    int keyLayout = 1;          // 1 AZERTY, 2 QWERTY
    bool recordMidi = true;
    bool recordAtFirstNote = true; // a recording armed starts with the first note
    bool recordingMarked = false;  // its start marked in the feed (a chapter in OBS's recording)
    std::vector<SoundFormat> recordFormats { SoundFormat::wavFloat }; // the sound's formats (File > Record formats)
    bool recordsFormat(SoundFormat f) const { return std::find(recordFormats.begin(), recordFormats.end(), f) != recordFormats.end(); }
    void toggleRecordFormat(SoundFormat f);
    juce::String recordingExtensions() const;
    RecordingExporter exporter; // the formats made from the live recording once it stops
    juce::String recordingStem, lastSaved;
    juce::Time recordingTime; // when the recording was started
    /// A recording stopped, its name asked for: its files written under the automatic name, its conversions (MP3 and
    /// the others) held until the name is given, so that they are made under it.
    struct PendingRecording {
        juce::String stem;
        juce::File folder;
        juce::Time time;
        double seconds = 0.0;
        int notes = 0;
        juce::File midi;            // written, or none
        RecordingExporter::Job job; // its master and its targets, under the automatic name
        juce::String formats;       // "wav mp3 mid"
    };
    std::deque<PendingRecording> pendingNames;
    bool namingOpen = false;
    bool closing = false; // the application quits: nothing asked any more
    void askNextName();
    void nameRecording(PendingRecording recording, NamingDialog::Action action, const GalleryMeta& meta);
    void submitRecording(PendingRecording& recording);
    std::vector<std::pair<juce::File, bool>> galleryRoots();
    // The OBS plugin's feed: the state for overlays last published, a MIDI file playing at the last look.
    pf::FeedInfo feedInfo {};
    bool filePlayingBefore = false;
    void refreshFeed();
    juce::String feedSummary() const;
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
    static constexpr int kQuickAudioRetries = 8; // then slow ones, in silence (silentForRetry)
    bool wantedWasListed = false;                // the device asked for, in its type's list at the last change
    bool silentForRetry() const;
    double nextAudioRetry = 0.0;
    juce::AudioDeviceManager::AudioDeviceSetup preferredAudio;
    void noteAudioDevice();       // after a device change: the choice kept, the fallback ended, the per-type memory
    void retryWantedAudio();
    bool opensWanted() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace pf::app
