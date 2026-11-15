#include "MainComponent.h"

#include <cstring>

#include "CurveText.h"
#include "OfflineRender.h"

namespace pf::app {

namespace {

constexpr int kLayoutAzerty = 1, kLayoutQwerty = 2;
// A gesture's notes still on their way to the engine when it is recognized: an armed recording ignores them.
constexpr double kGestureTailSeconds = 0.1;

enum MenuId {
    // File
    kRecord = 1, kRecordMidi, kSaveTake, kTakes, kOpenRecordings, kOpenTakes, kChangeCorpus, kQuit, kOpenMidi, kRecordAtFirstNote,
    // Player
    kPlayPause = 80, kStopPlayer, kBack, kForward, kLoop, kCloseMidi, kSpeed = 100,
    // Engine
    kEngineReference = 20, kEngineModal, kEngineModalB, kChooseProfileB, kEngineOption = 30, kEngineSettings = 50, kRoomToggle, kRoomSettings, kHeadphones,
    // Audio
    kAudioSettings = 60, kPerformance, kResetDropouts, kStreamLook,
    // Keyboard
    kVelocityCurve = 70, kAzerty, kQwerty, kOctaveDown, kOctaveUp, kVelocityDown, kVelocityUp, kPedalCurve, kGestures,
    // Help
    kShortcuts = 90, kAbout, kCheckUpdates, kUpdateSettings,
    // Engine > Room presets (one per preset, built-in then the user's)
    kRoomPresetBase = 300,
    // Presets: the instrument presets (apply, then delete), save, export, import
    kInstrumentBase = 400, kInstrumentDeleteBase = 500, kInstrumentSave = 390, kExportSettings, kImportSettings,
    // File > Record formats (one per sound format)
    kRecordFormat = 200,
};

constexpr int kSpeedPresets[] = { 50, 75, 100, 125, 150, 200 }; // percent, menu Player > Speed

bool isMidiFile(const juce::File& f)
{
    return f.hasFileExtension("mid;midi;smf;kar");
}

/// Finds the corpus: saved setting, then $PIANOFORGE_CORPUS, then walking up from the executable.
juce::File findCorpus(const juce::String& saved)
{
    if (saved.isNotEmpty() && juce::File(saved).isDirectory())
        return juce::File(saved);
    const auto pass = "reference/noire-dry/single-notes";
    if (const auto env = juce::SystemStats::getEnvironmentVariable("PIANOFORGE_CORPUS", {}); env.isNotEmpty())
        if (const auto d = juce::File(env).getChildFile(pass); d.isDirectory())
            return d;
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 10 && dir.exists(); ++i, dir = dir.getParentDirectory())
        if (const auto d = dir.getChildFile("corpus").getChildFile(pass); d.isDirectory())
            return d;
    return {};
}

/// Saved as "inputMin;inputMax;p0,p1,p2,p3,p4" (CurveText.h, shared with the plugin).
pf::VelocityCurve::Settings parseCurve(const juce::String& text)
{
    return parseCurveText(text);
}

/// The pedal curve, saved like the velocity curve; its input range may start at 0.
pf::PedalCurve::Settings parsePedalCurve(const juce::String& text)
{
    return parseCurveText(text, 0, pf::PedalCurve::defaults());
}

juce::String formatCurve(const pf::VelocityCurve::Settings& s)
{
    return formatCurveText(s);
}

juce::String noteName(int note)
{
    return juce::MidiMessage::getMidiNoteName(note, true, true, 4);
}

juce::String thousands(int n)
{
    auto s = juce::String(n);
    for (int i = s.length() - 3; i > 0; i -= 3)
        s = s.substring(0, i) + juce::String::charToString((juce::juce_wchar) 0x202f) + s.substring(i);
    return s;
}

} // namespace

MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PianoForge";
    options.folderName = "PianoForge";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters(options);
    auto* settings = properties.getUserSettings();

    // Audio: ask for 44.1 kHz (the corpus rate) and a small buffer; the saved setting takes precedence, and the
    // device is switched to the corpus rate once the corpus is loaded, when the device offers it.
    preferredAudio.sampleRate = 44100.0;
    preferredAudio.bufferSize = 128;
    const auto savedAudio = settings->getXmlValue("audio");
    const auto audioError = deviceManager.initialise(0, 2, savedAudio.get(), true, {}, &preferredAudio);
    if (savedAudio != nullptr)
        wantedAudio = std::make_unique<juce::XmlElement>(*savedAudio);
    lastDeviceType = deviceManager.getCurrentAudioDeviceType();
    // The device last used with each type, from the start: a switch of type in Audio opens it, not the type's first.
    if (wantedAudio != nullptr && wantedAudio->getStringAttribute("deviceType").isNotEmpty())
        settings->setValue("audioDevice." + wantedAudio->getStringAttribute("deviceType"), wantedAudio->getStringAttribute("audioOutputDeviceName"));
    if (wantedAudio != nullptr && !opensWanted()) { // the saved device could not be opened: kept, tried again
        audioFallback = true;
        fallbackType = deviceManager.getCurrentAudioDeviceType();
        fallbackName = deviceManager.getCurrentAudioDevice() != nullptr ? deviceManager.getCurrentAudioDevice()->getName() : juce::String();
        nextAudioRetry = juce::Time::getMillisecondCounterHiRes() + 1500.0;
        showMessage("Audio: " + wantedAudio->getStringAttribute("audioOutputDeviceName") + " could not be opened"
                        + (audioError.isNotEmpty() ? " (" + audioError + ")" : juce::String()) + ": playing on " + fallbackName
                        + " meanwhile, trying it again",
                    theme::busy);
    }
    curveSettings = parseCurve(settings->getValue("velocityCurve"));
    velocityCurve.set(curveSettings);
    pedalSettings = parsePedalCurve(settings->getValue("pedalCurve"));
    pedalCurve.set(pedalSettings);
    host.rateMode = settings->getIntValue("engineRateMode", 0) == 1 ? 1 : 0; // before the device starts the host
    deviceManager.addAudioCallback(&host);
    deviceManager.addMidiInputDeviceCallback({}, &keyboardInput);
    deviceManager.addChangeListener(this);
    enableMidiInputs();
    midiDevicesConnection = juce::MidiDeviceListConnection::make([this] { enableMidiInputs(); }); // hot-plugged keyboard

    // Panels: their settings apply now, whether their windows open or not.
    enginePanel = std::make_unique<EnginePanel>(modal, player, *settings);
    audioPanel = std::make_unique<AudioPanel>(deviceManager, host, *settings);
    performancePanel.onReset = [this] {
        auto* d = deviceManager.getCurrentAudioDevice();
        loadTracker.reset(host.load, d != nullptr ? d->getXRunCount() : -1);
    };
    roomPanel = std::make_unique<RoomPanel>(host.room, *settings);
    gesturesPanel = std::make_unique<GesturesPanel>(keyboardInput, *settings);
    streamLook = std::make_unique<StreamLook>(*settings);
    streamLookPanel = std::make_unique<StreamLookPanel>(*streamLook);
    streamLook->onChanged = [this] { streamLookPanel->refresh(); };
    gesturesPanel->onMessage = [this](const juce::String& text) { showMessage(text, theme::good); };
    gesturesPanel->onChanged = [this] { refreshGestureKeys(); };
    keyboardInput.onGesture = [safe = juce::Component::SafePointer<MainComponent>(this)](int action, double start) {
        if (safe != nullptr)
            safe->runGesture(action, start);
    };
    roomPanel->onChange = [this] { refreshRoomButton(); };
    playerBar = std::make_unique<PlayerBar>(host.filePlayer, *settings);
    playerBar->onOpen = [this] { chooseMidiFile(); };
    playerBar->onClicked = [this] { keyboard.grabKeyboardFocus(); };
    addAndMakeVisible(*playerBar);

    // Toolbar: engine (switchable while playing, for A/B listening), recording, volume, velocity, pedal, load, level.
    referenceButton.setTooltip("The reference: the bank's recordings, played back (Ctrl+1)");
    modalButton.setTooltip("The PianoForge modal engine (Ctrl+2)");
    for (auto* b : { &referenceButton, &modalButton }) {
        b->setClickingTogglesState(false);
        b->setRadioGroupId(1);
        addAndMakeVisible(*b);
    }
    referenceButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    modalButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    referenceButton.onClick = [this] { selectEngine(false); };
    modalButton.onClick = [this] { selectEngine(true); };
    selectEngine(settings->getIntValue("engine", 1) == 2);

    recordButton.setTooltip("Record what you play: the sound in the formats ticked in File > Record formats, and the MIDI (Ctrl+R)");
    recordButton.onClick = [this] { toggleRecording(); };
    recordMidi = settings->getBoolValue("recordMidi", true);
    recordAtFirstNote = settings->getBoolValue("recordAtFirstNote", true);
    // The sound's formats: "wav32 flac mp3"...; before them, a single WAV switch.
    if (const auto keys = juce::StringArray::fromTokens(settings->getValue("recordFormats", settings->getBoolValue("recordWav", true) ? "wav32" : ""), " ", {});
        true) {
        recordFormats.clear();
        for (const auto& f : soundFormats())
            if (keys.contains(f.key) && canWrite(f.format))
                recordFormats.push_back(f.format);
    }
    pedalButton.setTooltip("The sustain pedal (Space): it fills with the pedal's travel as the engine receives it; "
                           "Keyboard > Pedal curve shapes the MIDI keyboard's pedal");
    pedalButton.setClickingTogglesState(false);
    pedalButton.onClick = [this] { sendSustain(!pedalButton.getToggleState()); };
    roomButton.setClickingTogglesState(false);
    roomButton.onClick = [this] { roomPanel->setOn(!roomPanel->isOn()); };
    refreshRoomButton();
    volumeLabel.setText("Volume", juce::dontSendNotification);
    volume.setRange(-24.0, 24.0, 0.5);
    volume.setTextValueSuffix(" dB");
    volume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 20);
    volume.setDoubleClickReturnValue(true, 6.0);
    // After the room, in the host: the OBS plugin's feed takes the sound before it (the stream keeps its level).
    volume.onValueChange = [this] { host.volume = juce::Decibels::decibelsToGain((float) volume.getValue()); };
    volume.onDragEnd = [this] { saveSettings(); };
    volume.setValue(settings->getDoubleValue("volume", 6.0));
    velocityLabel.setText("Velocity", juce::dontSendNotification);
    velocity.setRange(1.0, 127.0, 1.0);
    velocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
    velocity.setTooltip("The velocity of the mouse and the computer keyboard (C / V: -10 / +10)");
    velocity.onValueChange = [this] { keyboard.setVelocity((float) velocity.getValue() / 127.0f, true); };
    velocity.onDragEnd = [this] { saveSettings(); };
    velocity.setValue(settings->getIntValue("velocity", 100));
    loadMeter.setTooltip("How much of each audio block's time the engine uses. Click for the details.");
    loadMeter.onClick = [this] { showWindow(performanceWindow, "Performance", performancePanel); };
    levelMeter.setTooltip("Output level, left and right (click to clear the clip light)");
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &pedalButton, &roomButton, &volumeLabel, &volume, &velocityLabel,
                                                             &velocity, &loadMeter, &levelMeter })
        addAndMakeVisible(*c);

    // Dashboard and keyboard.
    for (auto* c : std::initializer_list<juce::Component*> { &engineCard, &noteCard, &audioCard, &sessionCard, &hint })
        addAndMakeVisible(*c);
    hint.setColour(juce::Label::textColourId, theme::dim);
    hint.setFont(theme::font(12.0f));
    keyboard.setAvailableRange(21, 108); // A0 to C8
    keyboard.setLowestVisibleKey(21);
    keyboard.setScrollButtonsVisible(false);
    keyboard.setOctaveForMiddleC(4);
    keyboard.setWantsKeyboardFocus(true);
    keyboard.addKeyListener(this);
    addAndMakeVisible(keyboard);
    refreshGestureKeys();
    setKeyLayout(settings->getIntValue("keyLayout", kLayoutAzerty));

    // Status bar.
    for (auto* l : { &statusLeft, &statusRight }) {
        l->setFont(theme::font(12.0f));
        l->setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(*l);
    }
    statusRight.setJustificationType(juce::Justification::centredRight);
    addChildComponent(progressBar);

    corpusFolder = findCorpus(settings->getValue("corpus"));
    if (const juce::File b(settings->getValue("profileB")); b.existsAsFile())
        loadProfileB(b, false); // ready for Ctrl+3; the engine starts on profile A
    if (corpusFolder.isDirectory())
        loadCorpus(corpusFolder);
    else
        showMessage("No corpus found: File > Change corpus..., and pick corpus/reference/noire-dry/single-notes", theme::busy);

    setSize(1440, 600);
    constructed = true;
    startTimerHz(20);
#if JUCE_WINDOWS
    // Updates at start, if asked for (Help > Update settings), once the window is up. (The update is the Windows
    // installer: elsewhere, PianoForge is updated as it was installed.)
    juce::Timer::callAfterDelay(4000, [safe = juce::Component::SafePointer<MainComponent>(this)] {
        if (auto* self = safe.getComponent())
            self->checkForUpdates(false);
    });
#endif
}

MainComponent::~MainComponent()
{
    stopTimer();
    updateCancel = true; // a download stops at its next block; a check, at its timeout at worst
    if (updateThread.joinable())
        updateThread.join();
    host.mirror.close();
    if (host.live.recording())
        finishRecording();
    saveSettings();
    if (takeRender.joinable())
        takeRender.join();
    if (modalLoader.joinable())
        modalLoader.join();
    if (profileBLoader.joinable())
        profileBLoader.join();
    takesWindow.reset();
    takesPanel.reset();
    host.playback.release();
    streamLookWindow.reset();
    streamLookPanel.reset();
    engineWindow.reset();
    audioWindow.reset();
    performanceWindow.reset();
    roomWindow.reset();
    curveWindow.reset(); // it refers to curveSettings and keyboardInput
    pedalWindow.reset();
    keyboard.removeKeyListener(this);
    deviceManager.removeChangeListener(this);
    deviceManager.removeMidiInputDeviceCallback({}, &keyboardInput);
    deviceManager.removeAudioCallback(&host);
    audioPanel.reset(); // before the device it refers to closes
    deviceManager.closeAudioDevice();
}

// ---- Menus ----

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Player", "Engine", "Presets", "Audio", "Keyboard", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu menu;
    auto item = [](int id, const juce::String& text, const juce::String& shortcut = {}, bool ticked = false, bool enabled = true) {
        juce::PopupMenu::Item i(text);
        i.itemID = id;
        i.isTicked = ticked;
        i.isEnabled = enabled;
        i.shortcutKeyDescription = shortcut;
        return i;
    };
    const bool fileLoaded = host.filePlayer.sequence() != nullptr;
    switch (index) {
    case 0: {
        menu.addItem(item(kOpenMidi, "Open MIDI file...", "Ctrl+O"));
        menu.addSeparator();
        menu.addItem(item(kRecord, host.live.recording() ? "Stop recording" : "Record", "Ctrl+R"));
        juce::PopupMenu formats;
        formats.addSectionHeader("The sound (converted when the recording stops)");
        for (int i = 0; i < (int) soundFormats().size(); ++i) {
            const auto& f = soundFormats()[(size_t) i];
            const bool can = canWrite(f.format);
            formats.addItem(item(kRecordFormat + i, juce::String(f.menuName) + (can ? "" : "  (not on this system)"), {},
                                 recordsFormat(f.format), can && !host.live.recording()));
        }
        formats.addSeparator();
        formats.addItem(item(kRecordMidi, "MIDI (aligned with the sound)", {}, recordMidi, !host.live.recording()));
        menu.addSubMenu("Record formats", formats);
        menu.addItem(item(kRecordAtFirstNote, "Start recordings at the first note", {}, recordAtFirstNote, !host.live.recording()));
        menu.addItem(item(kSaveTake, "Save last take (MIDI and WAV render)", "Ctrl+S"));
        menu.addSeparator();
        menu.addItem(item(kOpenRecordings, "Open the recordings folder"));
        menu.addItem(item(kTakes, "Takes and recordings...", "Ctrl+T"));
        menu.addItem(item(kOpenTakes, "Open the takes folder"));
        menu.addSeparator();
        menu.addItem(item(kChangeCorpus, "Change corpus..."));
        menu.addSeparator();
        menu.addItem(item(kQuit, "Quit", "Ctrl+Q"));
        break;
    }
    case 1: {
        menu.addItem(item(kOpenMidi, "Open MIDI file...", "Ctrl+O"));
        menu.addSeparator();
        menu.addItem(item(kPlayPause, host.filePlayer.wantsToPlay() ? "Pause" : "Play", "Ctrl+Space", false, fileLoaded));
        menu.addItem(item(kStopPlayer, "Stop (back to the start)", "Ctrl+Home", false, fileLoaded));
        menu.addItem(item(kBack, "Back 5 s", "Ctrl+Left", false, fileLoaded));
        menu.addItem(item(kForward, "Forward 5 s", "Ctrl+Right", false, fileLoaded));
        menu.addSeparator();
        menu.addItem(item(kLoop, "Loop", "Ctrl+L", host.filePlayer.isLooping()));
        juce::PopupMenu speeds;
        for (int i = 0; i < (int) std::size(kSpeedPresets); ++i)
            speeds.addItem(item(kSpeed + i, juce::String(kSpeedPresets[i]) + " %", {},
                                juce::roundToInt(host.filePlayer.getSpeed() * 100.0) == kSpeedPresets[i]));
        menu.addSubMenu("Speed", speeds);
        menu.addSeparator();
        menu.addItem(item(kCloseMidi, "Close the MIDI file", {}, false, fileLoaded));
        break;
    }
    case 2: {
        menu.addItem(item(kEngineReference, "Reference: " + bankName() + " samples", "Ctrl+1", !modalSelected()));
        menu.addItem(item(kEngineModal, "Modal, profile A: " + profileASummary, "Ctrl+2", modalSelected() && !profileBActive));
        menu.addItem(item(kEngineModalB, "Modal, profile B: " + profileBSummary, "Ctrl+3", modalSelected() && profileBActive,
                          profileB != nullptr));
        menu.addItem(item(kChooseProfileB, "Profile B...", {}));
        menu.addSeparator();
        for (int o = 0; o < (int) EnginePanel::Option::count; ++o) {
            if (o == (int) EnginePanel::Option::wholeTake)
                menu.addSeparator();
            menu.addItem(item(kEngineOption + o, EnginePanel::name((EnginePanel::Option) o), {}, enginePanel->isOn((EnginePanel::Option) o)));
        }
        menu.addSeparator();
        menu.addItem(item(kEngineSettings, "Engine settings (tuning, re-strike, levels)...", "Ctrl+E"));
        menu.addSeparator();
        menu.addItem(item(kRoomToggle, "Room: " + roomPanel->presetName(), {}, roomPanel->isOn()));
        juce::PopupMenu rooms;
        const auto names = roomPanel->presetNames();
        for (int i = 0; i < names.size(); ++i) {
            if (i == (int) RoomPanel::presets().size())
                rooms.addSectionHeader("My presets");
            rooms.addItem(item(kRoomPresetBase + i, names[i], {}, roomPanel->presetIndex() == i));
        }
        menu.addSubMenu("Room presets", rooms);
        menu.addItem(item(kRoomSettings, "Room settings...", "Ctrl+M"));
        menu.addItem(item(kHeadphones, "Heard on headphones (binaural)", "Ctrl+H", roomPanel->onHeadphones()));
        break;
    }
    case 3: {
        const auto list = instrumentPresets();
        const auto current = properties.getUserSettings()->getValue("instrumentPreset");
        if (list.isEmpty())
            menu.addItem(item(-1, "No preset of your own yet", {}, false, false));
        for (int k = 0; k < list.size(); ++k) {
            const auto name = list[k].getProperty("name", juce::String()).toString();
            menu.addItem(item(kInstrumentBase + k, name, {}, name == current));
        }
        menu.addSeparator();
        menu.addItem(item(kInstrumentSave, "Save the instrument as..."));
        if (!list.isEmpty()) {
            juce::PopupMenu remove;
            for (int k = 0; k < list.size(); ++k)
                remove.addItem(item(kInstrumentDeleteBase + k, list[k].getProperty("name", juce::String()).toString()));
            menu.addSubMenu("Delete", remove);
        }
        menu.addSeparator();
        menu.addItem(item(kExportSettings, "Export settings..."));
        menu.addItem(item(kImportSettings, "Import settings..."));
        break;
    }
    case 4:
        menu.addItem(item(kAudioSettings, "Audio, MIDI and streaming...", "Ctrl+,"));
        menu.addItem(item(kPerformance, "Performance...", "Ctrl+P"));
        menu.addItem(item(kStreamLook, "Stream look (OBS keyboard)..."));
        menu.addSeparator();
        menu.addItem(item(kResetDropouts, "Reset the dropout counters"));
        break;
    case 5:
        menu.addItem(item(kVelocityCurve, "Velocity curve..."));
        menu.addItem(item(kPedalCurve, "Pedal curve..."));
        menu.addItem(item(kGestures, "Piano gestures..."));
        menu.addSeparator();
        menu.addItem(item(kAzerty, "Computer keyboard: AZERTY", {}, keyLayout == kLayoutAzerty));
        menu.addItem(item(kQwerty, "Computer keyboard: QWERTY", {}, keyLayout == kLayoutQwerty));
        menu.addSeparator();
        menu.addItem(item(kOctaveDown, "Octave down", keyLayout == kLayoutAzerty ? "W" : "Z"));
        menu.addItem(item(kOctaveUp, "Octave up", "X"));
        menu.addItem(item(kVelocityDown, "Velocity -10", "C"));
        menu.addItem(item(kVelocityUp, "Velocity +10", "V"));
        break;
    case 6:
        menu.addItem(item(kShortcuts, "Keyboard shortcuts", "F1"));
        menu.addItem(item(kAbout, "About PianoForge"));
#if JUCE_WINDOWS
        menu.addSeparator();
        menu.addItem(item(kCheckUpdates, "Check for updates..."));
        menu.addItem(item(kUpdateSettings, "Update settings..."));
#endif
        break;
    default:
        break;
    }
    return menu;
}

void MainComponent::menuItemSelected(int id, int)
{
    if (id >= kEngineOption && id < kEngineOption + (int) EnginePanel::Option::count) {
        const auto o = (EnginePanel::Option) (id - kEngineOption);
        enginePanel->setOn(o, !enginePanel->isOn(o));
        return;
    }
    if (id >= kInstrumentBase && id < kInstrumentBase + 100) {
        applyInstrument(id - kInstrumentBase);
        return;
    }
    if (id >= kInstrumentDeleteBase && id < kInstrumentDeleteBase + 100) {
        askDeleteInstrument(id - kInstrumentDeleteBase);
        return;
    }
    if (id == kInstrumentSave) {
        askSaveInstrument();
        return;
    }
    if (id == kExportSettings) {
        exportSettings();
        return;
    }
    if (id == kImportSettings) {
        importSettings();
        return;
    }
    if (id >= kRoomPresetBase && id < kRoomPresetBase + roomPanel->presetNames().size()) {
        roomPanel->selectPreset(id - kRoomPresetBase);
        return;
    }
    if (id >= kRecordFormat && id < kRecordFormat + (int) soundFormats().size()) {
        toggleRecordFormat(soundFormats()[(size_t) (id - kRecordFormat)].format);
        return;
    }
    if (id >= kSpeed && id < kSpeed + (int) std::size(kSpeedPresets)) {
        playerBar->setSpeed(kSpeedPresets[id - kSpeed] / 100.0);
        return;
    }
    switch (id) {
    case kOpenMidi: chooseMidiFile(); break;
    case kPlayPause: playerBar->togglePlay(); break;
    case kStopPlayer: playerBar->stop(); break;
    case kBack: playerBar->nudge(-5.0); break;
    case kForward: playerBar->nudge(5.0); break;
    case kLoop: playerBar->setLoop(!host.filePlayer.isLooping()); break;
    case kCloseMidi: closeMidiFile(); break;
    case kRecord: toggleRecording(); break;
    case kRecordMidi: recordMidi = !recordMidi; saveSettings(); break;
    case kRecordAtFirstNote: recordAtFirstNote = !recordAtFirstNote; saveSettings(); break;
    case kSaveTake: saveLastTake(); break;
    case kOpenRecordings: recordingsFolder().createDirectory(); recordingsFolder().startAsProcess(); break;
    case kOpenTakes: takesFolder().createDirectory(); takesFolder().startAsProcess(); break;
    case kTakes: openTakes(); break;
    case kChangeCorpus: chooseCorpus(); break;
    case kQuit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
    case kEngineReference: selectEngine(false); break;
    case kEngineModal: selectModalProfile(false); break;
    case kEngineModalB: selectModalProfile(true); break;
    case kChooseProfileB: chooseProfileB(); break;
    case kEngineSettings: showWindow(engineWindow, "Engine settings", *enginePanel); break;
    case kRoomToggle: roomPanel->setOn(!roomPanel->isOn()); break;
    case kRoomSettings: showWindow(roomWindow, "Room", *roomPanel); break;
    case kHeadphones: toggleHeadphones(); break;
    case kAudioSettings: showWindow(audioWindow, "Audio, MIDI and streaming", *audioPanel); break;
    case kPerformance: showWindow(performanceWindow, "Performance", performancePanel); break;
    case kResetDropouts: performancePanel.onReset(); break;
    case kVelocityCurve: openVelocityCurve(); break;
    case kPedalCurve: openPedalCurve(); break;
    case kGestures: showWindow(gesturesWindow, "Piano gestures", *gesturesPanel); break;
    case kStreamLook: showWindow(streamLookWindow, "Stream look", *streamLookPanel); break;
    case kAzerty: setKeyLayout(kLayoutAzerty); break;
    case kQwerty: setKeyLayout(kLayoutQwerty); break;
    case kOctaveDown: shiftOctave(-1); break;
    case kOctaveUp: shiftOctave(1); break;
    case kVelocityDown: velocity.setValue(velocity.getValue() - 10.0); break;
    case kVelocityUp: velocity.setValue(velocity.getValue() + 10.0); break;
    case kShortcuts: showShortcuts(); break;
    case kCheckUpdates: checkForUpdates(true); break;
    case kUpdateSettings: askUpdateSettings(); break;
    case kAbout: showAbout(); break;
    default: break;
    }
}

void MainComponent::showWindow(std::unique_ptr<ToolWindow>& window, const juce::String& title, juce::Component& content)
{
    if (window == nullptr)
        window = std::make_unique<ToolWindow>(title, content, *this);
    window->show();
}

void MainComponent::showShortcuts()
{
    const bool azerty = keyLayout == kLayoutAzerty;
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::NoIcon, "Keyboard shortcuts",
        juce::String("Playing\n")
            + (azerty ? "  Q Z S E D F T G Y H U J K O L P M: the keys from C to E\n  W / X: octave down / up\n"
                      : "  A W S E D F T G Y H U J K O L P ;: the keys from C to E\n  Z / X: octave down / up\n")
            + "  C / V: velocity -10 / +10\n  Space: sustain pedal\n  B: sostenuto pedal (held)\n  N: soft pedal, una corda (held)\n\n"
              "MIDI file player\n  Ctrl+O: open a MIDI file (or drop it on the window)\n  Ctrl+Space: play / pause\n"
              "  Ctrl+Home: stop, back to the start\n  Ctrl+Left / Ctrl+Right: 5 s back / forward\n  Ctrl+L: loop\n\n"
              "Application\n  Ctrl+R: record / stop\n  Ctrl+S: save the last take\n  Ctrl+1 / Ctrl+2: reference / modal engine\n"
              "  Ctrl+T: takes and recordings\n  Ctrl+E: engine settings\n  Ctrl+M: room settings\n  Ctrl+H: heard on headphones / speakers\n  Ctrl+,: audio, MIDI and streaming\n  Ctrl+P: performance\n  F1: this list\n  Ctrl+Q: quit\n\n"
              "Piano gestures, on the MIDI keyboard (Keyboard > Piano gestures)\n" + gesturesHelp()
            + "\nThe mouse plays too: lower on a key is louder.");
}

void MainComponent::refreshGestureKeys()
{
    std::vector<GestureKeyboard::Entry> entries;
    if (keyboardInput.gesturesOn.load())
        for (int a = 0; a < (int) PianoAction::count; ++a)
            if (const auto g = keyboardInput.gesture(a); !g.empty())
                entries.push_back({ g, pianoActions()[(std::size_t) a].name });
    keyboard.setGestures(std::move(entries));
}

juce::String MainComponent::gesturesHelp() const
{
    juce::String out;
    for (int a = 0; a < (int) PianoAction::count; ++a)
        if (const auto g = keyboardInput.gesture(a); !g.empty())
            out << "  " << juce::String(g.describe()) << ": " << pianoActions()[(std::size_t) a].name << "\n";
    return out.isEmpty() ? juce::String("  none\n") : out;
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::NoIcon, "About PianoForge",
        "PianoForge " + juce::String(PIANOFORGE_VERSION) + "\n\nA concert grand played from a MIDI keyboard: a modal engine "
        "calibrated on a reference library, with the reference itself for A/B listening.\n\nAudio thread: lock-free, "
        "no allocation. Recording, the mirror output and the OBS plugin's feed only copy each block.\n\nUpdates: "
            + (properties.getUserSettings()->getValue("updateAddress").trim().isEmpty()
                   ? juce::String("none looked for (Help > Update settings).")
                   : properties.getUserSettings()->getValue("updateAddress").trim()
                         + (properties.getUserSettings()->getBoolValue("updateAtStart", false) ? ", checked at start." : ", checked on request.")));
}

// ---- Actions ----

void MainComponent::checkForUpdates(bool manual)
{
    auto* s = properties.getUserSettings();
    const auto address = s->getValue("updateAddress").trim();
    if (address.isEmpty()) {
        if (manual)
            askUpdateSettings();
        return;
    }
    if (!manual) { // at start: if asked for, once a day at most
        const auto last = s->getValue("updateLastCheck", "0").getLargeIntValue();
        if (!s->getBoolValue("updateAtStart", false) || juce::Time::currentTimeMillis() - last < 20LL * 3600 * 1000)
            return;
    }
    if (updateBusy.exchange(true)) {
        if (manual)
            showMessage("Already looking for an update", theme::busy);
        return;
    }
    if (updateThread.joinable())
        updateThread.join();
    if (manual)
        showMessage("Looking for an update...", theme::busy);
    updateThread = std::thread([address, manual, safe = juce::Component::SafePointer<MainComponent>(this)] {
        const auto result = UpdateChecker::fetch(address, PIANOFORGE_VERSION);
        juce::MessageManager::callAsync([safe, result, manual] {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            self->updateBusy = false;
            auto* settings = self->properties.getUserSettings();
            if (!result.ok) {
                if (manual)
                    self->showMessage("No update: " + result.error, theme::bad);
                return;
            }
            settings->setValue("updateLastCheck", juce::String(juce::Time::currentTimeMillis()));
            if (!result.newer) {
                if (manual)
                    self->showMessage("PianoForge " PIANOFORGE_VERSION " is up to date (the latest: " + result.release.version + ")", theme::good);
                return;
            }
            if (!manual && settings->getValue("updateSkipped") == result.release.version)
                return; // skipped: offered again only on request
            self->offerUpdate(result.release);
        });
    });
}

void MainComponent::askUpdateSettings()
{
    auto* s = properties.getUserSettings();
    auto* window = new juce::AlertWindow("Updates",
                                         "Where to look for a newer PianoForge: the address of a manifest (latest.json, written next "
                                         "to the installer by the installer build) or of a GitHub release "
                                         "(https://api.github.com/repos/<owner>/<repo>/releases/latest). https or a file. Empty: "
                                         "nothing is looked for, nothing leaves this computer.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("address", s->getValue("updateAddress"), "Address");
    auto atStart = std::make_shared<juce::ToggleButton>("Look when PianoForge starts (once a day at most)");
    atStart->setToggleState(s->getBoolValue("updateAtStart", false), juce::dontSendNotification);
    atStart->setSize(420, 26);
    window->addCustomComponent(atStart.get());
    window->addButton("Save and look now", 2);
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([this, window, atStart](int result) {
        if (result == 0)
            return;
        const auto address = window->getTextEditorContents("address").trim();
        if (address.isNotEmpty() && !UpdateChecker::addressAllowed(address)) {
            showMessage("Not saved: the address must be https (or a file)", theme::bad);
            return;
        }
        auto* settings = properties.getUserSettings();
        settings->setValue("updateAddress", address);
        settings->setValue("updateAtStart", atStart->getToggleState());
        settings->saveIfNeeded();
        if (result == 2 && address.isNotEmpty())
            checkForUpdates(true);
        else
            showMessage(address.isEmpty() ? juce::String("Updates: none looked for") : "Updates looked for at " + address, theme::good);
    }), true);
}

void MainComponent::offerUpdate(const UpdateChecker::Release& release)
{
    auto notes = release.notes.trim();
    if (notes.length() > 700)
        notes = notes.substring(0, 700).trimEnd() + "...";
    const auto size = release.size > 0 ? " (" + juce::String((double) release.size / 1048576.0, 1) + " MB)" : juce::String();
    juce::AlertWindow::showAsync(juce::MessageBoxOptions {}
                                     .withIconType(juce::MessageBoxIconType::InfoIcon)
                                     .withTitle("PianoForge " + release.version)
                                     .withMessage("PianoForge " + release.version + " is available; this is " PIANOFORGE_VERSION ".\n\n"
                                                  + (notes.isNotEmpty() ? notes + "\n\n" : juce::String()) + "Download and install" + size
                                                  + ": the installer is checked (SHA-256), then PianoForge closes, installs it and opens "
                                                    "again. The settings, takes and recordings stay.")
                                     .withButton("Download and install", 1)
                                     .withButton("Skip this version", 2)
                                     .withButton("Later", 0)
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<MainComponent>(this), release](int result) {
                                     auto* self = safe.getComponent();
                                     if (self == nullptr || result == 0)
                                         return;
                                     if (result == 2) {
                                         self->properties.getUserSettings()->setValue("updateSkipped", release.version);
                                         self->showMessage("PianoForge " + release.version + " skipped (Help > Check for updates offers it again)");
                                         return;
                                     }
                                     self->installUpdate(release);
                                 });
}

void MainComponent::installUpdate(const UpdateChecker::Release& release)
{
    if (host.live.recording()) {
        showMessage("Stop the recording first (Ctrl+R), then Help > Check for updates", theme::busy);
        return;
    }
    if (updateBusy.exchange(true))
        return;
    if (updateThread.joinable())
        updateThread.join();
    updateCancel = false;
    const auto destination = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("PianoForge-update").getChildFile(release.fileName());
    showMessage("Downloading PianoForge " + release.version + "...", theme::busy);
    updateThread = std::thread([release, destination, cancel = &updateCancel, safe = juce::Component::SafePointer<MainComponent>(this)] {
        int shown = -1;
        const auto error = UpdateChecker::download(release, destination, [&](double done) {
            if (const int percent = (int) (100.0 * done); percent / 5 != shown / 5) {
                shown = percent;
                juce::MessageManager::callAsync([safe, percent, version = release.version] {
                    if (auto* self = safe.getComponent())
                        self->showMessage("Downloading PianoForge " + version + ": " + juce::String(percent) + " %", theme::busy);
                });
            }
            return !cancel->load();
        });
        juce::MessageManager::callAsync([safe, error, destination] {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            self->updateBusy = false;
            if (error.isNotEmpty()) {
                self->showMessage("Update not installed: " + error, theme::bad);
                return;
            }
            if (self->host.live.recording()) {
                self->showMessage("Update downloaded: stop the recording, then Help > Check for updates", theme::busy);
                return;
            }
            // The installer waits for PianoForge to have closed (it closes what uses its files), installs, and opens it.
            if (!destination.startAsProcess(UpdateChecker::installerArguments())) {
                self->showMessage("Cannot start the installer " + destination.getFullPathName(), theme::bad);
                return;
            }
            self->saveSettings();
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        });
    });
}

void MainComponent::toggleHeadphones()
{
    roomPanel->setHeadphones(!roomPanel->onHeadphones());
    showMessage(roomPanel->onHeadphones() ? juce::String("Heard on headphones: the piano in front of you (binaural)")
                                          : juce::String("Heard on speakers"),
                theme::good);
}

void MainComponent::refreshRoomButton()
{
    roomButton.setToggleState(roomPanel->isOn(), juce::dontSendNotification);
    roomButton.setTooltip("The room around the piano: " + roomPanel->presetName()
                          + (roomPanel->isOn() ? juce::String(" (click: off)") : juce::String(" (off; click: on)"))
                          + (roomPanel->onHeadphones() ? juce::String(", heard on headphones") : juce::String())
                          + ". Ctrl+M: its settings, Ctrl+H: headphones or speakers");
}

void MainComponent::selectEngine(bool useModal)
{
    engines.selected = useModal ? 1 : 0;
    referenceButton.setToggleState(!useModal, juce::dontSendNotification);
    modalButton.setToggleState(useModal, juce::dontSendNotification);
    saveSettings();
}

void MainComponent::setKeyLayout(int layout)
{
    keyLayout = layout == kLayoutQwerty ? kLayoutQwerty : kLayoutAzerty;
    // AZERTY: home row q s d f g h j k l m = white keys, top row = black keys.
    const char* keys = keyLayout == kLayoutAzerty ? "qzsedftgyhujkolpm" : "awsedftgyhujkolp;";
    keyboard.clearKeyMappings();
    for (int i = 0; keys[i] != 0; ++i)
        keyboard.setKeyPressForNote(juce::KeyPress(keys[i]), i);
    keyboard.setKeyPressBaseOctave(octave + 1); // JUCE: octave n -> C = 12 * n; C4 (60) = octave 5
    hint.setText(juce::String("Computer keyboard from C") + juce::String(octave) + ": "
                     + (keyLayout == kLayoutAzerty ? "Q Z S E D F T G Y H U J K...   W / X octave" : "A W S E D F T G Y H U J K...   Z / X octave")
                     + "   C / V velocity   Space pedal   B sostenuto   N soft   Ctrl+R record   F1 all the shortcuts"
                     + "   Dots: piano gestures (mouse over)",
                 juce::dontSendNotification);
    saveSettings();
}

void MainComponent::shiftOctave(int delta)
{
    octave = juce::jlimit(0, 7, octave + delta);
    setKeyLayout(keyLayout);
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown()) {
        const int code = key.getKeyCode();
        if (code == juce::KeyPress::spaceKey) {
            playerBar->togglePlay();
            return true;
        }
        if (code == juce::KeyPress::homeKey) {
            playerBar->stop();
            return true;
        }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
            playerBar->nudge(code == juce::KeyPress::leftKey ? -5.0 : 5.0);
            return true;
        }
        switch (code) {
        case 'O': chooseMidiFile(); return true;
        case 'L': playerBar->setLoop(!host.filePlayer.isLooping()); return true;
        case 'R': toggleRecording(); return true;
        case 'S': saveLastTake(); return true;
        case 'E': showWindow(engineWindow, "Engine settings", *enginePanel); return true;
        case 'M': showWindow(roomWindow, "Room", *roomPanel); return true;
        case 'T': openTakes(); return true;
        case 'H': toggleHeadphones(); return true;
        case 'P': showWindow(performanceWindow, "Performance", performancePanel); return true;
        case ',': showWindow(audioWindow, "Audio, MIDI and streaming", *audioPanel); return true;
        case '1': selectEngine(false); return true;
        case '2': selectModalProfile(false); return true;
        case '3': selectModalProfile(true); return true;
        case 'Q': juce::JUCEApplication::getInstance()->systemRequestedQuit(); return true;
        default: break;
        }
    }
    if (key == juce::KeyPress::F1Key) {
        showShortcuts();
        return true;
    }
    const auto c = (int) juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    if (c == (keyLayout == kLayoutAzerty ? 'w' : 'z') || c == 'x') {
        shiftOctave(c == 'x' ? 1 : -1);
        return true;
    }
    if (c == 'c' || c == 'v') {
        velocity.setValue(velocity.getValue() + (c == 'v' ? 10.0 : -10.0));
        return true;
    }
    return key == juce::KeyPress::spaceKey || c == 'b'; // the pedals are handled in keyStateChanged
}

bool MainComponent::keyStateChanged(bool, juce::Component*)
{
    // Space is the pedal; Ctrl+Space is the player's play / pause, and leaves the pedal alone.
    const bool space = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
    if (space != spaceDown) {
        spaceDown = space;
        if (space && !juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown()) {
            sendSustain(true);
            pedalFromSpace = true;
        } else if (!space && pedalFromSpace) {
            sendSustain(false);
            pedalFromSpace = false;
        }
    }
    // B held: the sostenuto pedal (a keyboard without the three-pedal unit can try it).
    const bool b = juce::KeyPress::isKeyCurrentlyDown('b') && !juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown();
    if (b != sostenutoKeyDown) {
        sostenutoKeyDown = b;
        sendSostenuto(b);
    }
    // N held: the soft pedal (una corda).
    const bool n = juce::KeyPress::isKeyCurrentlyDown('n') && !juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown();
    if (n != softKeyDown) {
        softKeyDown = n;
        sendSoftPedal(n);
    }
    return false; // let the on-screen keyboard handle the other keys
}

void MainComponent::sendSustain(bool down)
{
    host.send(juce::MidiMessage::controllerEvent(1, 64, down ? 127 : 0));
}

void MainComponent::sendSostenuto(bool down)
{
    host.send(juce::MidiMessage::controllerEvent(1, 66, down ? 127 : 0));
}

void MainComponent::sendSoftPedal(bool down)
{
    host.send(juce::MidiMessage::controllerEvent(1, 67, down ? 127 : 0));
}

void MainComponent::enableMidiInputs()
{
    for (const auto& input : juce::MidiInput::getAvailableDevices())
        if (!deviceManager.isMidiInputDeviceEnabled(input.identifier))
            deviceManager.setMidiInputDeviceEnabled(input.identifier, true);
}

void MainComponent::openVelocityCurve()
{
    if (curveWindow == nullptr) {
        curveWindow = std::make_unique<VelocityCurveWindow>(curveSettings, keyboardInput, [this] {
            velocityCurve.set(curveSettings);
            saveSettings();
        });
        curveWindow->centreAroundComponent(this, curveWindow->getWidth(), curveWindow->getHeight());
        updateRenderedLayers();
    }
    curveWindow->setVisible(true);
    curveWindow->toFront(true);
}

// ---- Instrument presets and the settings' backup ----

const juce::StringArray& MainComponent::instrumentKeys()
{
    // What an instrument preset holds: the engine, its settings, the tuning, the room and the listening position,
    // the curves and the volume (not the audio device, the corpus, the folders, the recording formats).
    static const juce::StringArray keys { "engine", "volume", "velocityCurve", "pedalCurve",
                                          "modalBody", "modalResidual", "modalSympathetic", "modalHeldKeys", "modalPedalCatch",
                                          "modalReleaseNoise", "playWholeTake", "sympatheticDb", "unisonSpread", "restrikeFade",
                                          "restrikeAbsorption",
                                          "referenceA4", "temperament", "temperamentRoot",
                                          "roomModel", "roomOn", "roomPreset", "roomPresetName", "roomPosition", "roomKeepLoudness",
                                          "roomLevel", "roomSize", "roomDecay", "roomBrightness", "roomWidth", "roomDistance" };
    return keys;
}

juce::Array<juce::var> MainComponent::instrumentPresets()
{
    const auto parsed = juce::JSON::parse(properties.getUserSettings()->getValue("instrumentPresets", "[]"));
    return parsed.isArray() ? *parsed.getArray() : juce::Array<juce::var> {};
}

void MainComponent::reloadSettings()
{
    auto* s = properties.getUserSettings();
    curveSettings = parseCurve(s->getValue("velocityCurve"));
    velocityCurve.set(curveSettings);
    pedalSettings = parsePedalCurve(s->getValue("pedalCurve"));
    pedalCurve.set(pedalSettings);
    volume.setValue(s->getDoubleValue("volume", volume.getValue()), juce::sendNotificationSync);
    enginePanel->reload();
    roomPanel->reload();
    gesturesPanel->reload();
    if (s->getIntValue("engine", 2) == 2 ? !modalSelected() : modalSelected())
        selectEngine(s->getIntValue("engine", 2) == 2);
    refreshRoomButton();
    for (auto* w : { curveWindow.get(), pedalWindow.get() })
        if (w != nullptr)
            w->repaint();
}

void MainComponent::askSaveInstrument()
{
    auto* window = new juce::AlertWindow("Save the instrument",
                                         "A name for the whole setup: the engine and its settings, the tuning, the room and the "
                                         "listening position, the curves, the volume (an existing name is replaced):",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", properties.getUserSettings()->getValue("instrumentPreset"), "Name");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([this, window](int result) {
        const auto name = window->getTextEditorContents("name").trim();
        if (result != 1 || name.isEmpty())
            return;
        saveSettings(); // the panels save as they change; the main settings, now
        auto* s = properties.getUserSettings();
        auto* values = new juce::DynamicObject();
        for (const auto& key : instrumentKeys())
            if (s->containsKey(key))
                values->setProperty(key, s->getValue(key));
        auto* preset = new juce::DynamicObject();
        preset->setProperty("name", name);
        preset->setProperty("values", juce::var(values));
        auto list = instrumentPresets();
        int index = -1;
        for (int k = 0; k < list.size(); ++k)
            if (list[k].getProperty("name", juce::String()).toString() == name)
                index = k;
        if (index >= 0)
            list.set(index, juce::var(preset));
        else
            list.add(juce::var(preset));
        s->setValue("instrumentPresets", juce::JSON::toString(juce::var(list), true));
        s->setValue("instrumentPreset", name);
        s->saveIfNeeded();
        showMessage("Instrument saved as \"" + name + "\" (Presets menu)", theme::good);
    }), true);
}

void MainComponent::applyInstrument(int index)
{
    const auto list = instrumentPresets();
    if (index < 0 || index >= list.size())
        return;
    auto* s = properties.getUserSettings();
    const auto values = list[index].getProperty("values", juce::var());
    if (auto* object = values.getDynamicObject())
        for (const auto& property : object->getProperties())
            if (instrumentKeys().contains(property.name.toString()))
                s->setValue(property.name.toString(), property.value.toString());
    const auto name = list[index].getProperty("name", juce::String()).toString();
    s->setValue("instrumentPreset", name);
    s->saveIfNeeded();
    reloadSettings();
    showMessage("Instrument: " + name, theme::good);
}

void MainComponent::askDeleteInstrument(int index)
{
    const auto list = instrumentPresets();
    if (index < 0 || index >= list.size())
        return;
    const auto name = list[index].getProperty("name", juce::String()).toString();
    juce::AlertWindow::showAsync(juce::MessageBoxOptions {}
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Delete an instrument preset")
                                     .withMessage("Delete the preset \"" + name + "\"? The instrument as it sounds now stays.")
                                     .withButton("Delete", 1)
                                     .withButton("Cancel", 0)
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<MainComponent>(this), name](int result) {
                                     if (result == 0 || safe == nullptr)
                                         return;
                                     auto list = safe->instrumentPresets();
                                     for (int k = 0; k < list.size(); ++k)
                                         if (list[k].getProperty("name", juce::String()).toString() == name) {
                                             list.remove(k);
                                             break;
                                         }
                                     auto* s = safe->properties.getUserSettings();
                                     s->setValue("instrumentPresets", juce::JSON::toString(juce::var(list), true));
                                     if (s->getValue("instrumentPreset") == name)
                                         s->setValue("instrumentPreset", juce::String());
                                     s->saveIfNeeded();
                                 });
}

namespace {
/// Settings that belong to this computer, left out of an export and of an import.
bool machineSetting(const juce::String& key)
{
    return key == "audio" || key.startsWith("audioDevice.") || key == "corpus" || key == "profileB" || key == "midiFolder" || key == "mirrorDevice"
        || key == "engineRateMode" || key == "roomHeadphones" // what this computer plays through
        || key == "updateLastCheck" || key == "updateSkipped";
}
} // namespace

void MainComponent::exportSettings()
{
    saveSettings();
    settingsChooser = std::make_unique<juce::FileChooser>(
        "Export the settings", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("PianoForge settings.pfsettings"),
        "*.pfsettings");
    settingsChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                     | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [this](const juce::FileChooser& chooser) {
                                     auto file = chooser.getResult();
                                     if (file == juce::File())
                                         return;
                                     file = file.withFileExtension(".pfsettings");
                                     juce::XmlElement xml("PIANOFORGESETTINGS");
                                     xml.setAttribute("version", 1);
                                     const auto& all = properties.getUserSettings()->getAllProperties();
                                     int count = 0;
                                     for (int k = 0; k < all.size(); ++k) {
                                         const auto key = all.getAllKeys()[k];
                                         if (machineSetting(key))
                                             continue;
                                         auto* e = xml.createNewChildElement("VALUE");
                                         e->setAttribute("name", key);
                                         e->setAttribute("val", all.getAllValues()[k]);
                                         ++count;
                                     }
                                     if (xml.writeTo(file))
                                         showMessage("Exported " + juce::String(count) + " settings to " + file.getFullPathName(), theme::good);
                                     else
                                         showMessage("Cannot write " + file.getFullPathName(), theme::bad);
                                 });
}

void MainComponent::importSettings()
{
    settingsChooser = std::make_unique<juce::FileChooser>(
        "Import settings", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.pfsettings");
    settingsChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& chooser) {
                                     const auto file = chooser.getResult();
                                     if (file == juce::File())
                                         return;
                                     const auto xml = juce::XmlDocument::parse(file);
                                     if (xml == nullptr || !xml->hasTagName("PIANOFORGESETTINGS")) {
                                         showMessage(file.getFileName() + " is not a PianoForge settings file", theme::bad);
                                         return;
                                     }
                                     auto* s = properties.getUserSettings();
                                     int count = 0;
                                     for (auto* e : xml->getChildWithTagNameIterator("VALUE")) {
                                         const auto key = e->getStringAttribute("name");
                                         if (key.isEmpty() || machineSetting(key))
                                             continue;
                                         s->setValue(key, e->getStringAttribute("val"));
                                         ++count;
                                     }
                                     s->saveIfNeeded();
                                     reloadSettings();
                                     showMessage("Imported " + juce::String(count) + " settings from " + file.getFileName()
                                                     + " (the audio device and the folders of this computer are kept)",
                                                 theme::good);
                                 });
}

void MainComponent::openPedalCurve()
{
    if (pedalWindow == nullptr) {
        pedalWindow = std::make_unique<VelocityCurveWindow>(pedalSettings, keyboardInput, [this] {
            pedalCurve.set(pedalSettings);
            saveSettings();
        }, CurveKind::pedal);
        pedalWindow->centreAroundComponent(this, pedalWindow->getWidth(), pedalWindow->getHeight());
    }
    if (const auto profile = modal.sharedProfile()) // where the dampers graze the strings
        pedalWindow->panel().setDamperLift(profile->pedalLift);
    pedalWindow->setVisible(true);
    pedalWindow->toFront(true);
}

void MainComponent::updateRenderedLayers()
{
    if (curveWindow == nullptr)
        return;
    std::vector<int> layers;
    if (const auto* b = player.bank())
        for (const auto& s : b->samples)
            if (std::find(layers.begin(), layers.end(), s.velocity) == layers.end())
                layers.push_back(s.velocity);
    curveWindow->panel().setRenderedLayers(std::move(layers));
}

juce::File MainComponent::takesFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("PianoForge").getChildFile("Takes");
}

juce::File MainComponent::recordingsFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("PianoForge").getChildFile("Recordings");
}

void MainComponent::showMessage(const juce::String& text, juce::Colour colour)
{
    statusRight.setText(text, juce::dontSendNotification);
    statusRight.setColour(juce::Label::textColourId, colour);
    messageUntil = juce::Time::getMillisecondCounterHiRes() + 12000.0;
}

void MainComponent::toggleRecording(std::int64_t notesFrom)
{
    if (host.live.recording()) {
        finishRecording();
        return;
    }
    if (!recordMidi && recordFormats.empty()) {
        showMessage("Nothing to record: tick a format in File > Record formats.", theme::busy);
        return;
    }
    if (deviceManager.getCurrentAudioDevice() == nullptr || host.currentRate() <= 0.0) {
        showMessage("No audio device is running: Audio > Audio, MIDI and streaming.", theme::bad);
        return;
    }
    recordingStem = "PianoForge " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    // The live recording: the 32-bit float WAV, kept under the recording's name when it is one of the formats,
    // else a master the conversions start from and then remove.
    const auto wavFile = recordFormats.empty() ? juce::File()
                         : recordingsFolder().getChildFile(recordingStem + (recordsFormat(SoundFormat::wavFloat) ? ".wav" : ".master.wav"));
    const auto engineName = modalSelected() ? juce::String("modal engine") : juce::String("reference player");
    if (const auto started = host.live.start(wavFile, host.currentRate(), host.currentSegment(),
                                             "PianoForge live recording (" + engineName + ")", recordAtFirstNote, notesFrom);
        started.failed()) {
        showMessage("Cannot record: " + started.getErrorMessage(), theme::bad);
        return;
    }
    recordButton.setColour(juce::TextButton::buttonColourId, recordAtFirstNote ? theme::busy : theme::record);
    recordingMarked = false; // the mark (a chapter in OBS's recording) when it starts: now, or at the first note
}

void MainComponent::finishRecording(std::int64_t at)
{
    const bool started = !host.live.waiting();
    const auto span = host.live.stop(at);
    recordButton.removeColour(juce::TextButton::buttonColourId);
    if (started)
        host.addFeedMark("PianoForge recording stopped");
    drained.clear(); // every event up to the stop, into the session record
    host.drainRecorded(drained);
    takes.add(drained, host.currentSegment(), host.currentRate());
    if (!span.valid) {
        if (span.wav != juce::File())
            span.wav.deleteFile();
        if (!started) {
            showMessage("Recording disarmed: no note was played", theme::busy);
            return;
        }
        showMessage("Recording failed: " + (span.error.isNotEmpty() ? span.error : juce::String("nothing recorded")), theme::bad);
        return;
    }
    juce::StringArray saved;
    int notes = 0;
    if (recordMidi) {
        if (auto take = takes.span(span.segment, span.start, span.end)) {
            take->text = "PianoForge live recording " + recordingStem + ", " + juce::String(juce::roundToInt(span.sampleRate))
                       + " Hz: one tick per sample, tick 0 is the first sample of the sound files of the same name";
            const auto midiFile = recordingsFolder().getChildFile(recordingStem + ".mid");
            recordingsFolder().createDirectory();
            midiFile.deleteFile();
            juce::FileOutputStream out(midiFile);
            if (out.openedOk() && TakeRecorder::toMidiFile(*take).writeTo(out)) {
                saved.add(".mid");
                notes = take->notes;
            }
        }
    }
    // The other formats, from the live recording, in the background.
    RecordingExporter::Job job;
    if (span.wav.existsAsFile()) {
        if (recordsFormat(SoundFormat::wavFloat))
            saved.add(".wav");
        job.master = span.wav;
        job.removeMaster = !recordsFormat(SoundFormat::wavFloat);
        job.label = recordingStem;
        for (const auto f : recordFormats)
            if (f != SoundFormat::wavFloat)
                job.targets.emplace_back(f, recordingsFolder().getChildFile(recordingStem + infoFor(f).extension));
    }
    juce::StringArray pending;
    for (const auto& [f, file] : job.targets)
        pending.add(infoFor(f).shortName);
    lastSaved = recordingStem + " " + saved.joinIntoString(" + ") + (pending.isEmpty() ? "" : (saved.isEmpty() ? "" : " + ") + pending.joinIntoString(" + "))
              + " (" + juce::String((double) (span.end - span.start) / span.sampleRate, 1) + " s, " + juce::String(notes) + " notes)";
    auto text = "Recorded " + lastSaved + " in " + recordingsFolder().getFullPathName()
              + (pending.isEmpty() ? juce::String() : ", converting to " + pending.joinIntoString(", ") + "...");
    if (!job.targets.empty())
        exporter.submit(std::move(job));
    if (span.dropped > 0)
        text << "  WARNING: " << juce::String(span.dropped) << " samples lost (the disk did not keep up)";
    if (span.error.isNotEmpty())
        text << "  " << span.error;
    showMessage(text, span.dropped > 0 || span.error.isNotEmpty() ? theme::busy : theme::good);
}

void MainComponent::toggleRecordFormat(SoundFormat f)
{
    if (recordsFormat(f)) {
        recordFormats.erase(std::find(recordFormats.begin(), recordFormats.end(), f));
    } else {
        // One WAV: both would have the same name.
        const auto otherWav = f == SoundFormat::wavFloat ? SoundFormat::wav24 : f == SoundFormat::wav24 ? SoundFormat::wavFloat : f;
        if (otherWav != f && recordsFormat(otherWav))
            recordFormats.erase(std::find(recordFormats.begin(), recordFormats.end(), otherWav));
        recordFormats.push_back(f);
        std::sort(recordFormats.begin(), recordFormats.end());
    }
    saveSettings();
}

juce::String MainComponent::recordingExtensions() const
{
    juce::StringArray e;
    for (const auto f : recordFormats)
        e.addIfNotAlreadyThere(infoFor(f).extension);
    if (recordMidi)
        e.add(".mid");
    return e.joinIntoString(" ");
}

void MainComponent::runGesture(int action, double start)
{
    if (action < 0 || action >= (int) PianoAction::count)
        return;
    const auto& info = pianoActions()[(std::size_t) action];
    const auto gesture = juce::String(keyboardInput.gesture(action).describe());
    // Where the gesture's first note reached the engine, a little before: a recording or a take ends there.
    const auto before = host.positionAt(start) - (std::int64_t) std::llround(0.01 * host.currentRate());
    auto toObs = [&](pf::FeedCommand command) {
        host.addFeedCommand(command);
        pf::AudioFeedWriter::Reader readers[pf::AudioFeedWriter::kMaxReaders];
        bool obs = false;
        for (int i = 0, n = host.feedReaders(readers, pf::AudioFeedWriter::kMaxReaders); i < n; ++i)
            obs = obs || readers[i].kind == pf::FeedReaderKind::audio;
        showMessage("Gesture " + gesture + ": " + info.name + (obs ? juce::String() : juce::String(" (no PianoForge source in OBS reads the feed)")),
                    obs ? theme::good : theme::busy);
    };
    switch ((PianoAction) action) {
    case PianoAction::record:
        if (host.live.recording()) {
            finishRecording(host.live.waiting() ? -1 : before);
        } else {
            // Armed by the gesture: its own notes (the last one still on its way to the engine) do not start it.
            const auto now = host.positionAt(juce::Time::getMillisecondCounterHiRes() * 0.001);
            toggleRecording(now + (std::int64_t) std::llround(kGestureTailSeconds * host.currentRate()));
            if (host.live.recording())
                showMessage("Gesture " + gesture + ": " + (recordAtFirstNote ? juce::String("recording armed, it starts with the first note")
                                                                              : juce::String("recording")),
                            theme::good);
        }
        break;
    case PianoAction::saveTake: saveLastTake(before); break;
    case PianoAction::playPause:
        playerBar->togglePlay();
        showMessage("Gesture " + gesture + ": " + info.name, theme::good);
        break;
    case PianoAction::obsRecord: toObs(pf::FeedCommand::obsRecordToggle); break;
    case PianoAction::obsStream: toObs(pf::FeedCommand::obsStreamToggle); break;
    case PianoAction::obsChapter: toObs(pf::FeedCommand::obsChapter); break;
    case PianoAction::obsNextScene: toObs(pf::FeedCommand::obsNextScene); break;
    case PianoAction::obsPreviousScene: toObs(pf::FeedCommand::obsPreviousScene); break;
    default: break;
    }
}

void MainComponent::saveLastTake(std::int64_t before)
{
    const auto take = takes.lastTake(10.0, 0.2, before);
    if (!take.has_value() || take->notes == 0) {
        showMessage("Nothing played yet: play something, then save it.", theme::busy);
        return;
    }
    const auto folder = takesFolder();
    folder.createDirectory();
    const auto stem = "take-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    const auto midiFile = folder.getChildFile(stem + ".mid"), wavFile = folder.getChildFile(stem + ".wav");
    {
        juce::FileOutputStream out(midiFile);
        if (!out.openedOk() || !TakeRecorder::toMidiFile(*take).writeTo(out)) {
            showMessage("Cannot write " + midiFile.getFullPathName(), theme::bad);
            return;
        }
    }
    const auto summary = juce::String(take->notes) + " notes, " + juce::String((double) take->lengthSamples / take->sampleRate, 1) + " s";
    lastSaved = stem + " (" + summary + ")";
    host.addFeedMark("Take " + stem + " (" + summary + ")");
    auto bank = player.sharedBank();
    auto profile = modal.sharedProfile();
    const bool withModal = modalSelected() && profile != nullptr;
    if ((withModal ? profile == nullptr : bank == nullptr) || renderingTake.load()) {
        showMessage("Saved " + midiFile.getFileName() + " (" + summary + ")", theme::good);
        return;
    }
    showMessage("Saved " + midiFile.getFileName() + " (" + summary + "); rendering the WAV...");
    renderInBackground(TakeRecorder::toSequence(*take), take->sampleRate, wavFile, summary);
}

void MainComponent::renderInBackground(juce::MidiMessageSequence sequence, double rate, juce::File wavFile, juce::String summary,
                                       std::function<void(bool)> done)
{
    auto bank = player.sharedBank();
    auto profile = modal.sharedProfile();
    const bool withModal = modalSelected() && profile != nullptr;
    if ((withModal ? profile == nullptr : bank == nullptr) || renderingTake.load()) {
        showMessage(renderingTake.load() ? juce::String("A render is running: try again once it is done") : juce::String("No engine to render with yet"),
                    theme::busy);
        if (done)
            done(false);
        return;
    }
    if (takeRender.joinable())
        takeRender.join();
    renderingTake = true;
    // Same engine and settings as heard live, at the rate it was played: the WAV is the take as heard.
    // Every setting of the engine heard (tuning, sympathetic resonance, pedal catch, release noise...), the room and
    // the limiter, as they are now; the tail is longer by the room's.
    takeRender = std::thread([sequence = std::move(sequence), bank = std::move(bank), profile = std::move(profile),
                              withModal, settings = modal.settings(), room = host.room.settings(), limit = host.limiter.enabled.load(),
                              rate, restrike = enginePanel->restrikeSeconds(), whole = enginePanel->isOn(EnginePanel::Option::wholeTake),
                              wavFile, summary, done = std::move(done), safe = juce::Component::SafePointer<MainComponent>(this)] {
        const double tail = 4.0 + (room.enabled ? pf::Room::tailSeconds(room) : 0.0);
        auto audio = withModal ? renderSequenceModal(sequence, profile, rate, settings, tail)
                               : renderSequence(sequence, bank, rate, restrike, whole, tail);
        applyRoom(audio, room, rate);
        applyLimiter(audio, limit, rate);
        const auto written = writeWav(audio, rate, wavFile);
        juce::MessageManager::callAsync([safe, written, wavFile, summary, done] {
            if (auto* self = safe.getComponent()) {
                self->renderingTake = false;
                if (written.wasOk())
                    self->showMessage("Rendered " + wavFile.getFileName() + " (" + summary + ") in "
                                          + wavFile.getParentDirectory().getFullPathName(),
                                      theme::good);
                else
                    self->showMessage("WAV render failed: " + written.getErrorMessage(), theme::bad);
                if (self->takesPanel != nullptr)
                    self->takesPanel->refresh();
                if (done)
                    done(written.wasOk());
            }
        });
    });
}

void MainComponent::openTakes()
{
    if (takesPanel == nullptr) {
        TakesPanel::Hooks hooks;
        hooks.playMidi = [this](const juce::File& midi) {
            openMidiFile(midi);
            if (!host.filePlayer.isPlaying())
                playerBar->togglePlay();
        };
        hooks.renderAgain = [this](const juce::File& midiFile) {
            juce::MidiFile midi;
            juce::FileInputStream in(midiFile);
            if (!in.openedOk() || !midi.readFrom(in, false)) { // false: no added note-offs (see TakeRecorder)
                showMessage("Cannot read " + midiFile.getFileName(), theme::bad);
                return;
            }
            midi.convertTimestampTicksToSeconds();
            juce::MidiMessageSequence sequence;
            int notes = 0;
            for (int t = 0; t < midi.getNumTracks(); ++t)
                sequence.addSequence(*midi.getTrack(t), 0.0);
            for (const auto* e : sequence)
                notes += e->message.isNoteOn() ? 1 : 0;
            const double rate = host.currentRate() > 0.0 ? host.currentRate() : 44100.0;
            const auto stem = midiFile.getFileNameWithoutExtension();
            const auto wav = midiFile.getSiblingFile(stem + " (render " + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ").wav");
            showMessage("Rendering " + stem + " again with the settings heard now...");
            renderInBackground(sequence, rate, wav, juce::String(notes) + " notes");
        };
        takesPanel = std::make_unique<TakesPanel>(host.playback, std::vector<juce::File> { takesFolder(), recordingsFolder() }, hooks);
    } else {
        takesPanel->refresh();
    }
    showWindow(takesWindow, "Takes and recordings", *takesPanel);
}

void MainComponent::chooseCorpus()
{
    fileChooser = std::make_unique<juce::FileChooser>("Folder of one corpus pass (e.g. single-notes)",
                                                      corpusFolder.exists() ? corpusFolder : juce::File());
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& fc) {
                                 auto choice = fc.getResult();
                                 if (!choice.isDirectory())
                                     return;
                                 if (choice.getChildFile("single-notes").isDirectory())
                                     choice = choice.getChildFile("single-notes");
                                 loadCorpus(choice);
                                 saveSettings();
                             });
}

void MainComponent::loadCorpus(const juce::File& folder)
{
    corpusFolder = folder;
    bankInstalled = false;
    loader.load(folder);
    progressBar.setVisible(true);
    resized();
    loadModalProfile();
}

void MainComponent::loadModalProfile()
{
    // The profile written by `forge export` sits in the corpus state folder, next to the passes.
    const auto path = corpusFolder.getParentDirectory().getChildFile("modal-profile.bin");
    if (!path.existsAsFile()) {
        profileSummary = "Modal profile: none (forge export)";
        return;
    }
    if (modalLoader.joinable())
        modalLoader.join();
    profileSummary = "Modal profile: loading...";
    modalLoader = std::thread([path, safe = juce::Component::SafePointer<MainComponent>(this)] {
        std::string error;
        auto profile = pf::loadModalProfile(path.getFullPathName().toStdString(), error);
        juce::MessageManager::callAsync([safe, path, profile = std::move(profile), error]() mutable {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            if (profile == nullptr) {
                self->profileSummary = "Modal profile: " + juce::String(error);
                self->showMessage("The modal profile did not load: " + juce::String(error), theme::bad);
                return;
            }
            self->profileASummary = describeProfile(*profile, path);
            self->profileA = std::move(profile);
            self->installActiveProfile();
        });
    });
}

juce::String MainComponent::describeProfile(const pf::ModalProfile& profile, const juce::File& file)
{
    std::size_t layers = 0;
    for (const auto& n : profile.notes)
        layers = std::max(layers, n.layers.size());
    auto folder = file.getParentDirectory();
    auto name = folder.getFileName(); // the corpus state, or a backup folder inside it
    if (name.startsWith("backup"))
        name = folder.getParentDirectory().getFileName() + "/" + name;
    return name + " (" + juce::String((int) profile.notes.size()) + " notes x " + juce::String((int) layers) + " layers, "
           + juce::String(juce::roundToInt((double) profile.bytes / 1.0e6)) + " MB)";
}

juce::String MainComponent::bankName() const
{
    const auto state = corpusFolder.getParentDirectory().getFileName();
    return state.isNotEmpty() ? state : juce::String("the bank's");
}

void MainComponent::installActiveProfile()
{
    const auto chosen = profileBActive && profileB != nullptr ? profileB : profileA;
    if (chosen == nullptr)
        return;
    if (chosen != modal.sharedProfile())
        modal.install(chosen);
    profileSummary = chosen == profileB ? "Modal profile B: " + profileBSummary : "Modal profile A: " + profileASummary;
}

void MainComponent::selectModalProfile(bool useB)
{
    if (useB && profileB == nullptr) {
        chooseProfileB();
        return;
    }
    const bool change = useB != profileBActive;
    profileBActive = useB;
    selectEngine(true);
    if (change) {
        // The notes sounding fade out (the engine's fast fade, as an engine switch does), then the other profile goes
        // in: the same engine and settings, only the profile changes.
        modal.allSoundOff();
        juce::Timer::callAfterDelay(80, [safe = juce::Component::SafePointer<MainComponent>(this)] {
            if (auto* self = safe.getComponent())
                self->installActiveProfile();
        });
    }
    showMessage(useB ? "Modal engine, profile B: " + profileBSummary : "Modal engine, profile A: " + profileASummary,
                theme::accent);
}

void MainComponent::chooseProfileB()
{
    const auto start = profileBFile.existsAsFile() ? profileBFile.getParentDirectory() : corpusFolder.getParentDirectory().getParentDirectory();
    fileChooser = std::make_unique<juce::FileChooser>("Profile B: a modal profile to compare (modal-profile.bin)", start, "*.bin");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc) {
                                 if (const auto file = fc.getResult(); file.existsAsFile())
                                     loadProfileB(file, true);
                                 keyboard.grabKeyboardFocus();
                             });
}

void MainComponent::loadProfileB(const juce::File& file, bool activate)
{
    if (profileBLoader.joinable())
        profileBLoader.join();
    profileBSummary = "loading...";
    profileBLoader = std::thread([file, activate, safe = juce::Component::SafePointer<MainComponent>(this)] {
        std::string error;
        auto profile = pf::loadModalProfile(file.getFullPathName().toStdString(), error);
        juce::MessageManager::callAsync([safe, file, activate, profile = std::move(profile), error]() mutable {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            if (profile == nullptr) {
                self->profileBSummary = "none";
                self->showMessage("Profile B did not load: " + juce::String(error), theme::bad);
                return;
            }
            self->profileBSummary = describeProfile(*profile, file);
            self->profileB = std::move(profile);
            self->profileBFile = file;
            self->saveSettings();
            if (activate)
                self->selectModalProfile(true);
            else if (self->profileBActive)
                self->installActiveProfile();
        });
    });
}

void MainComponent::chooseMidiFile()
{
    auto* settings = properties.getUserSettings();
    const juce::File folder(settings->getValue("midiFolder"));
    fileChooser = std::make_unique<juce::FileChooser>("Open a MIDI file", folder.isDirectory() ? folder : juce::File(),
                                                      "*.mid;*.midi;*.smf;*.kar");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc) {
                                 if (const auto file = fc.getResult(); file.existsAsFile())
                                     openMidiFile(file);
                                 keyboard.grabKeyboardFocus();
                             });
}

void MainComponent::openMidiFile(const juce::File& file)
{
    juce::String error;
    auto sequence = MidiFilePlayer::read(file, error); // small files: read here, in a few milliseconds
    if (sequence == nullptr) {
        showMessage("Cannot open the MIDI file: " + error, theme::bad);
        return;
    }
    const int seconds = juce::roundToInt(sequence->length);
    const auto summary = juce::String::fromUTF8(sequence->name.c_str()) + " (" + juce::String(sequence->notes) + " notes, "
                       + (seconds >= 60 ? juce::String(seconds / 60) + " min " : juce::String()) + juce::String(seconds % 60) + " s)";
    host.filePlayer.load(std::move(sequence));
    playerBar->fileChanged();
    auto* settings = properties.getUserSettings();
    settings->setValue("midiFolder", file.getParentDirectory().getFullPathName());
    settings->saveIfNeeded();
    showMessage("Opened " + summary + ": Ctrl+Space plays it, with its own velocities (no velocity curve).", theme::good);
}

void MainComponent::closeMidiFile()
{
    host.filePlayer.load(nullptr);
    playerBar->fileChanged();
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isMidiFile(juce::File(f)))
            return true;
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    playerBar->setDropHighlight(false);
    for (const auto& f : files)
        if (const juce::File file(f); isMidiFile(file)) {
            openMidiFile(file);
            break;
        }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    noteAudioDevice(); // the audio device changed
    saveSettings();
}

bool MainComponent::opensWanted() const
{
    const auto* d = deviceManager.getCurrentAudioDevice();
    return wantedAudio == nullptr
        || (d != nullptr && deviceManager.getCurrentAudioDeviceType() == wantedAudio->getStringAttribute("deviceType")
            && d->getName() == wantedAudio->getStringAttribute("audioOutputDeviceName"));
}

void MainComponent::noteAudioDevice()
{
    const auto* d = deviceManager.getCurrentAudioDevice();
    const auto type = deviceManager.getCurrentAudioDeviceType();
    const auto name = d != nullptr ? d->getName() : juce::String();
    auto* s = properties.getUserSettings();
    // Only the Audio window chooses a device. Anything else that changes it is JUCE falling back on its own (the
    // device failed, or left the list: a driver reset, a DAC asleep, a virtual cable restarted): the choice is kept,
    // tried again, and taken back as soon as it can be.
    const bool byUser = audioWindow != nullptr && audioWindow->isVisible();
    if (!byUser && !opensWanted()) {
        const bool wantedListed = [&] {
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (wantedAudio != nullptr && t->getTypeName() == wantedAudio->getStringAttribute("deviceType"))
                    return t->getDeviceNames(false).contains(wantedAudio->getStringAttribute("audioOutputDeviceName"));
            return false;
        }();
        if (!audioFallback) {
            audioFallback = true;
            showMessage("Audio: " + (wantedAudio != nullptr ? wantedAudio->getStringAttribute("audioOutputDeviceName") : juce::String("the device"))
                            + " stopped: playing on " + (name.isNotEmpty() ? name : juce::String("nothing")) + " meanwhile, trying it again",
                        theme::busy);
            audioRetries = 0;
            nextAudioRetry = juce::Time::getMillisecondCounterHiRes() + 1500.0;
        } else if (wantedListed && !wantedWasListed) { // back in the list: tried again, at once
            audioRetries = 0;
            nextAudioRetry = juce::Time::getMillisecondCounterHiRes() + 500.0;
        }
        wantedWasListed = wantedListed; // (an ASIO driver stays listed without its device: no reason to try again)
        if (d != nullptr) {
            fallbackType = type;
            fallbackName = name;
        }
        return;
    }
    if (audioFallback && opensWanted())
        showMessage("Audio: back on " + name, theme::good);
    audioFallback = false;
    // A switch of device type opens that type's first device (JUCE): the one last used with it instead.
    if (byUser && type != lastDeviceType) {
        lastDeviceType = type;
        const auto remembered = s->getValue("audioDevice." + type);
        if (auto* t = deviceManager.getCurrentDeviceTypeObject();
            t != nullptr && remembered.isNotEmpty() && remembered != name && t->getDeviceNames(false).contains(remembered)) {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.outputDeviceName = remembered;
            setup.inputDeviceName = {};
            if (deviceManager.setAudioDeviceSetup(setup, true).isEmpty())
                return; // its own change message records it
        }
    }
    lastDeviceType = type;
    if (d != nullptr) {
        s->setValue("audioDevice." + type, name);
        if (const auto xml = deviceManager.createStateXml())
            wantedAudio = std::make_unique<juce::XmlElement>(*xml);
    }
}

bool MainComponent::silentForRetry() const
{
    // Nothing sounding, played or recorded: a moment without the stand-in device goes unheard.
    const int voices = modalSelected() ? modal.activeVoices.load() : player.activeVoices.load();
    return voices == 0 && !host.filePlayer.isPlaying() && !host.live.recording() && !host.playback.isPlaying();
}

void MainComponent::retryWantedAudio()
{
    // The device asked for, again; if it still cannot be opened, back to the stand-in (as it is running now, not
    // as JUCE last saved a choice). Not while the Audio window is open: the user is choosing.
    if (audioWindow != nullptr && audioWindow->isVisible())
        return;
    ++audioRetries;
    // Eight tries two seconds apart; then one every half a minute, and only in silence (each try stops the stand-in
    // a moment: it would be heard, recorded and streamed).
    nextAudioRetry = juce::Time::getMillisecondCounterHiRes() + (audioRetries < kQuickAudioRetries ? 2000.0 : 30000.0);
    juce::XmlElement standIn("DEVICESETUP");
    const auto setup = deviceManager.getAudioDeviceSetup();
    standIn.setAttribute("deviceType", deviceManager.getCurrentAudioDeviceType());
    standIn.setAttribute("audioOutputDeviceName", setup.outputDeviceName);
    standIn.setAttribute("audioDeviceRate", setup.sampleRate);
    standIn.setAttribute("audioDeviceBufferSize", setup.bufferSize);
    const auto error = deviceManager.initialise(0, 2, wantedAudio.get(), false, {}, &preferredAudio);
    if (error.isEmpty() && opensWanted())
        return; // the change message ends the fallback
    deviceManager.initialise(0, 2, &standIn, true, {}, &preferredAudio);
    if (audioRetries == kQuickAudioRetries)
        showMessage("Audio: " + wantedAudio->getStringAttribute("audioOutputDeviceName") + " is busy or missing"
                        + (error.isNotEmpty() ? " (" + error + ")" : juce::String()) + "; playing on " + fallbackName
                        + " meanwhile. It is taken back when it returns, or choose it again in Audio.",
                    theme::bad);
}

void MainComponent::saveSettings()
{
    if (!constructed)
        return;
    auto* s = properties.getUserSettings();
    if (audioFallback) {
        if (wantedAudio != nullptr) // the device asked for, not the stand-in
            s->setValue("audio", wantedAudio.get());
    } else if (const auto xml = deviceManager.createStateXml()) {
        s->setValue("audio", xml.get());
    }
    s->setValue("corpus", corpusFolder.getFullPathName());
    s->setValue("profileB", profileBFile.getFullPathName());
    s->setValue("volume", volume.getValue());
    s->setValue("velocity", (int) velocity.getValue());
    s->setValue("keyLayout", keyLayout);
    s->setValue("velocityCurve", formatCurve(curveSettings));
    s->setValue("pedalCurve", formatCurve(pedalSettings));
    s->setValue("engine", modalSelected() ? 2 : 1);
    s->setValue("recordMidi", recordMidi);
    s->setValue("recordAtFirstNote", recordAtFirstNote);
    juce::StringArray formats;
    for (const auto f : recordFormats)
        formats.add(infoFor(f).key);
    s->setValue("recordFormats", formats.joinIntoString(" "));
    s->saveIfNeeded();
}

// ---- Dashboard ----

juce::String MainComponent::midiInputs() const
{
    juce::StringArray names;
    for (const auto& e : juce::MidiInput::getAvailableDevices())
        if (deviceManager.isMidiInputDeviceEnabled(e.identifier))
            names.add(e.name);
    return names.isEmpty() ? juce::String("no MIDI keyboard") : "MIDI: " + names.joinIntoString(", ");
}

juce::StringArray MainComponent::engineLines() const
{
    if (modalSelected())
        return { juce::String(modal.activeVoices.load()) + " voices of " + juce::String(pf::ModalEngine::kMaxVoices) + ", "
                     + thousands(modal.activeModes.load()) + " resonators",
                 juce::String(modal.ringingStrings.load()) + " free strings ringing, pedal "
                     + juce::String(juce::roundToInt(modal.pedalPosition.load() * 100.0f)) + " %"
                     + (modal.sostenutoDown.load() ? ", sostenuto" : "") + (modal.softPedalPosition.load() > 0.0f ? ", soft pedal" : "") + ", "
                     + juce::String((int) modal.stolenVoices.load()) + " voices stolen" };
    return { juce::String(player.activeVoices.load()) + " voices of " + juce::String(ReferencePlayer::kMaxVoices),
             juce::String((int) player.stolenVoices.load()) + " voices stolen" + (player.sostenutoDown.load() ? ", sostenuto" : "") };
}

juce::StringArray MainComponent::deviceLines() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr)
        return { "No audio output open" };
    const double fs = d->getCurrentSampleRate();
    const int block = d->getCurrentBufferSizeSamples();
    const juce::String engine = host.rateFactor() > 1 ? ", engine at " + juce::String(host.currentRate() / 1000.0, 1) + " kHz" : juce::String();
    return { d->getName() + " (" + d->getTypeName() + ")",
             juce::String(fs / 1000.0, 1) + " kHz" + engine + ", blocks of " + juce::String(block) + " (" + juce::String(1000.0 * block / fs, 1)
                 + " ms), output latency ~" + juce::String(1000.0 * (d->getOutputLatencyInSamples() + block) / fs, 1) + " ms" };
}

juce::StringArray MainComponent::adviceLines() const
{
    juce::StringArray tips;
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr)
        return tips;
    const double fs = d->getCurrentSampleRate();
    const int block = d->getCurrentBufferSizeSamples();
    if (fs > 50000.0 && host.rateFactor() == 1)
        tips.add("At " + juce::String(juce::roundToInt(fs / 1000.0)) + " kHz the engine does " + juce::String(fs / 48000.0, 1)
                 + " times the work of 48 kHz for nothing audible: in Audio, let the engine run at 44.1 or 48 kHz.");
    if (1000.0 * block / fs > 6.0)
        tips.add("Blocks of " + juce::String(1000.0 * block / fs, 1) + " ms add as much latency between key and sound: "
                 "128 samples (2.9 ms at 44.1 kHz) is comfortable for this engine.");
#if JUCE_WINDOWS
    if (d->getTypeName() != "ASIO")
        tips.add("The lowest latency on Windows comes with ASIO (Audio > Audio, MIDI and streaming > Audio device type).");
#endif
    const auto& s = loadTracker.snapshot();
    if (s.dropouts > 0 && s.peakMinute < theme::kBusyLoad)
        tips.add("Dropouts with a low load come from the driver or the system, not the engine: a larger buffer, or ASIO.");
    else if (s.peakMinute >= theme::kOverloaded)
        tips.add("The load reaches the overloaded zone: a lower sample rate, a larger buffer, or fewer resonances (Engine settings).");
    return tips;
}

void MainComponent::refreshFeed()
{
    if (!host.feedOn())
        return;
    // The state for overlays (the OBS text source), when it changes.
    pf::FeedInfo info;
    auto put = [](char* to, std::size_t size, const juce::String& text) {
        const auto utf8 = text.toUTF8();
        std::size_t n = std::min(size - 1, std::strlen(utf8.getAddress()));
        while (n > 0 && ((unsigned char) utf8.getAddress()[n] & 0xC0) == 0x80) // whole characters
            --n;
        std::memcpy(to, utf8.getAddress(), n);
        to[n] = 0;
    };
    put(info.engine, sizeof(info.engine), modalSelected() ? "Modal" : "Reference");
    put(info.preset, sizeof(info.preset), properties.getUserSettings()->getValue("instrumentPreset"));
    put(info.room, sizeof(info.room), roomPanel->isOn() ? roomPanel->presetName() : juce::String("off"));
    const auto sequence = host.filePlayer.sequence();
    const bool playing = sequence != nullptr && host.filePlayer.isPlaying();
    if (sequence != nullptr)
        put(info.file, sizeof(info.file), juce::String::fromUTF8(sequence->name.c_str()));
    info.filePlaying = playing ? 1u : 0u;
    info.recording = host.live.recording() ? 1u : 0u;
    if (std::memcmp(&info, &feedInfo, sizeof(info)) != 0) {
        feedInfo = info;
        host.setFeedInfo(info);
    }
    // A MIDI file starting to play: a mark (a chapter in OBS's recording).
    if (playing && !filePlayingBefore && sequence != nullptr)
        host.addFeedMark("MIDI: " + juce::String::fromUTF8(sequence->name.c_str()));
    filePlayingBefore = playing;
    // The readers: the speakers mix rendered only while an OBS source takes it.
    pf::AudioFeedWriter::Reader readers[pf::AudioFeedWriter::kMaxReaders];
    const int count = host.feedReaders(readers, pf::AudioFeedWriter::kMaxReaders);
    bool speakers = false;
    for (int i = 0; i < count; ++i)
        speakers = speakers || (readers[i].kind == pf::FeedReaderKind::audio && readers[i].mix == pf::FeedMix::speakers);
    host.speakersWanted = speakers;
}

juce::String MainComponent::feedSummary() const
{
    if (!host.feedOn())
        return "OBS plugin not fed (another PianoForge)";
    pf::AudioFeedWriter::Reader readers[pf::AudioFeedWriter::kMaxReaders];
    const int count = host.feedReaders(readers, pf::AudioFeedWriter::kMaxReaders);
    if (count == 0)
        return "OBS plugin fed, no OBS source reading";
    int audio = 0, keys = 0, text = 0;
    float latency = 0.0f;
    for (int i = 0; i < count; ++i) {
        audio += readers[i].kind == pf::FeedReaderKind::audio ? 1 : 0;
        keys += readers[i].kind == pf::FeedReaderKind::keyboard ? 1 : 0;
        text += readers[i].kind == pf::FeedReaderKind::text ? 1 : 0;
        if (readers[i].kind == pf::FeedReaderKind::audio)
            latency = std::max(latency, readers[i].latencyMs);
    }
    juce::StringArray what;
    if (audio > 0)
        what.add("sound");
    if (keys > 0)
        what.add("keyboard");
    if (text > 0)
        what.add("text");
    return "OBS reads the " + what.joinIntoString(", ") + (latency > 0.0f ? ", key to OBS " + juce::String(juce::roundToInt(latency)) + " ms" : juce::String());
}

void MainComponent::refreshDashboard()
{
    // Engine.
    auto engineInfo = engineLines();
    engineCard.setLines({ modalSelected() ? (profileBActive && profileB != nullptr ? "Modal: profile B" : "Modal: profile A")
                                          : "Reference: " + bankName() + " samples", engineInfo[0], engineInfo[1],
                          modalSelected() ? profileSummary : corpusSummary },
                        theme::accent);

    // Last note.
    const int note = modalSelected() ? modal.lastNote.load() : player.lastNote.load();
    if (note >= 0) {
        const int played = modalSelected() ? modal.lastVelocity.load() : player.lastVelocity.load();
        const int layer = modalSelected() ? modal.lastLayerVelocity.load() : player.lastRenderedVelocity.load();
        const int in = keyboardInput.lastInput.load(), out = keyboardInput.lastOutput.load();
        noteCard.setLines({ noteName(note) + "   velocity " + juce::String(played),
                            layer > 0 ? (modalSelected() ? "modal layer " + juce::String(layer) + ", levels interpolated per partial"
                                                         : "take recorded at velocity " + juce::String(layer))
                                      : juce::String("no data for this note"),
                            host.filePlayer.isPlaying() ? juce::String("from the MIDI file (its own velocities)")
                            : in > 0 ? "MIDI keyboard: velocity " + juce::String(in) + " -> " + juce::String(out) + " (velocity curve)"
                                     : juce::String("mouse or computer keyboard"),
                            "computer keyboard from C" + juce::String(octave) });
    } else {
        noteCard.setLines({ "Nothing played yet", midiInputs(), "computer keyboard from C" + juce::String(octave) });
    }

    // Audio.
    auto dev = deviceLines();
    const auto& load = loadTracker.snapshot();
    juce::String loadLine = "load " + juce::String(juce::roundToInt(load.average * 100.0f)) + " % (peak "
                          + juce::String(juce::roundToInt(load.peakMinute * 100.0f)) + " % over a minute), "
                          + (load.dropouts == 0 ? juce::String("no dropout") : juce::String(load.dropouts) + " dropouts");
    audioCard.setLines({ dev[0], dev.size() > 1 ? dev[1] : juce::String(), loadLine,
                         adviceLines().isEmpty() ? midiInputs() : "advice in Audio > Performance" },
                       deviceManager.getCurrentAudioDevice() == nullptr ? theme::bad : theme::text);

    // Session.
    if (host.live.recording() && host.live.waiting()) {
        sessionCard.setLines({ "Armed", "the recording starts with the first note (" + recordingStem + ")", feedSummary(),
                               audioPanel->mirrorSummary() },
                             theme::busy);
    } else if (host.live.recording()) {
        const double t = host.live.seconds();
        sessionCard.setLines({ juce::String::formatted("Recording  %d:%04.1f", (int) t / 60, std::fmod(t, 60.0)),
                               recordingStem + " " + recordingExtensions(), feedSummary(), audioPanel->mirrorSummary() },
                             theme::record);
    } else {
        sessionCard.setLines({ exporter.busy() ? "Converting " + exporter.progressText() : juce::String("Not recording"),
                               lastSaved.isEmpty() ? juce::String("Ctrl+R records, Ctrl+S saves the last take") : "Last: " + lastSaved,
                               feedSummary(), audioPanel->mirrorSummary() });
    }
}

void MainComponent::timerCallback()
{
    if (!focusGiven && isShowing()) {
        keyboard.grabKeyboardFocus();
        focusGiven = true;
    }
    if (audioFallback && juce::Time::getMillisecondCounterHiRes() >= nextAudioRetry
        && (audioRetries < kQuickAudioRetries || silentForRetry()))
        retryWantedAudio();

    if (streamLook != nullptr && ++streamLookTicks % 2 == 0) // the stream look, with OBS, ten times a second
        streamLook->poll();
    drained.clear(); // take capture: move what reached the engine into the session record
    host.drainRecorded(drained);
    takes.add(drained, host.currentSegment(), host.currentRate());
    if (host.live.endedByDevice()) // the device stopped or changed: the recording ends there
        finishRecording();
    for (const auto& report : exporter.takeReports())
        showMessage(report.text + (report.ok ? " in " + recordingsFolder().getFullPathName() : juce::String()), report.ok ? theme::good : theme::bad);
    if (host.live.recording() && host.live.waiting()) {
        recordButton.setButtonText("Armed: play");
    } else if (host.live.recording()) {
        if (!recordingMarked) { // it started (at once, or with the first note): a chapter in OBS's recording
            recordingMarked = true;
            host.addFeedMark("PianoForge recording: " + recordingStem);
            recordButton.setColour(juce::TextButton::buttonColourId, theme::record);
        }
        recordButton.setButtonText(juce::String::formatted("Stop  %d:%04.1f", (int) host.live.seconds() / 60, std::fmod(host.live.seconds(), 60.0)));
    } else {
        recordButton.setButtonText("Record");
    }

    playerBar->refresh();
    refreshFeed();

    // Corpus loading.
    if (loader.isLoading()) {
        loadProgress = loader.progress();
        corpusSummary = "Corpus: loading " + juce::String(juce::roundToInt(loadProgress * 100.0)) + " %";
    } else if (!bankInstalled) {
        if (auto bank = loader.takeResult()) {
            // Run the device at the corpus rate when it can: no resampling at all, hence no interpolation loss.
            const double corpusRate = bank->sampleRate;
            player.installBank(std::move(bank));
            bankInstalled = true;
            updateRenderedLayers();
            if (auto* device = deviceManager.getCurrentAudioDevice();
                device != nullptr && std::abs(device->getCurrentSampleRate() - corpusRate) > 0.5
                && device->getAvailableSampleRates().contains(corpusRate)) {
                auto setup = deviceManager.getAudioDeviceSetup();
                setup.sampleRate = corpusRate;
                deviceManager.setAudioDeviceSetup(setup, true);
            }
            progressBar.setVisible(false);
            resized();
            corpusSummary = "Corpus: " + loader.status();
        }
    }

    // Load, level, pedal.
    auto* device = deviceManager.getCurrentAudioDevice();
    loadTracker.update(host.load, device != nullptr ? device->getXRunCount() : -1);
    const auto& load = loadTracker.snapshot();
    loadMeter.set(load.average, load.peakRecent, load.dropouts);
    levelMeter.push(host.load.peakLeft.exchange(0.0f), host.load.peakRight.exchange(0.0f));
    player.outputPeak.exchange(0.0f);
    modal.outputPeak.exchange(0.0f);
    float peak = 0.0f;
    bool dropout = false;
    if (loadTracker.takeColumn(peak, dropout))
        performancePanel.pushColumn(peak, dropout);
    if (performanceWindow != nullptr && performanceWindow->isVisible()) {
        auto engine = engineLines();
        engine.insert(0, modalSelected() ? "Modal: PianoForge engine" : "Reference: " + bankName() + " samples");
        performancePanel.update(load, engine, deviceLines(), adviceLines());
    }
    const bool sustain = modalSelected() ? modal.sustainDown.load() : player.sustainDown.load();
    pedalButton.setToggleState(sustain, juce::dontSendNotification);
    pedalButton.setDepth((float) host.pedalValue.load() / 127.0f);

    // Dashboard and status bar.
    refreshDashboard();
    statusLeft.setText(corpusSummary + "   |   " + profileSummary + "   |   " + midiInputs(), juce::dontSendNotification);
    if (messageUntil > 0.0 && juce::Time::getMillisecondCounterHiRes() > messageUntil) {
        statusRight.setText({}, juce::dontSendNotification);
        messageUntil = 0.0;
    }
}

// ---- Layout ----

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    g.setColour(theme::panel);
    g.fillRect(toolbarArea);
    g.fillRect(statusArea);
    g.setColour(theme::panelEdge);
    g.drawHorizontalLine(toolbarArea.getBottom() - 1, 0.0f, (float) getWidth());
    g.drawHorizontalLine(statusArea.getY(), 0.0f, (float) getWidth());
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Toolbar.
    toolbarArea = area.removeFromTop(64);
    auto bar = toolbarArea.reduced(12, 10);
    auto engineBox = bar.removeFromLeft(220).withSizeKeepingCentre(220, 30);
    referenceButton.setBounds(engineBox.removeFromLeft(110));
    modalButton.setBounds(engineBox);
    bar.removeFromLeft(12);
    recordButton.setBounds(bar.removeFromLeft(130).withSizeKeepingCentre(130, 30));
    bar.removeFromLeft(16);
    auto level = bar.removeFromRight(170);
    levelMeter.setBounds(level.withSizeKeepingCentre(170, 26));
    bar.removeFromRight(14);
    loadMeter.setBounds(bar.removeFromRight(250));
    bar.removeFromRight(16);
    pedalButton.setBounds(bar.removeFromRight(70).withSizeKeepingCentre(70, 30));
    bar.removeFromRight(8);
    roomButton.setBounds(bar.removeFromRight(70).withSizeKeepingCentre(70, 30));
    bar.removeFromRight(12);
    const int sliderWidth = juce::jmax(140, (bar.getWidth() - 120) / 2);
    volumeLabel.setBounds(bar.removeFromLeft(54));
    volume.setBounds(bar.removeFromLeft(sliderWidth).withSizeKeepingCentre(sliderWidth, 28));
    bar.removeFromLeft(10);
    velocityLabel.setBounds(bar.removeFromLeft(58));
    velocity.setBounds(bar.removeFromLeft(sliderWidth).withSizeKeepingCentre(sliderWidth, 28));

    // Status bar.
    statusArea = area.removeFromBottom(26);
    auto status = statusArea.reduced(12, 3);
    if (progressBar.isVisible())
        progressBar.setBounds(status.removeFromRight(220).reduced(0, 2));
    statusRight.setBounds(status.removeFromRight(status.getWidth() / 2));
    statusLeft.setBounds(status);

    // Keyboard and its hint.
    area.reduce(12, 0);
    auto keys = area.removeFromBottom(juce::jlimit(120, 280, area.getHeight() - 118 - 12 - 26 - 16 - 82));
    keyboard.setBounds(keys);
    keyboard.setKeyWidth((float) keys.getWidth() / 52.0f); // 52 white keys from A0 to C8
    area.removeFromBottom(4);
    hint.setBounds(area.removeFromBottom(22));

    // Dashboard.
    area.removeFromTop(12);
    auto cards = area.removeFromTop(juce::jmin(118, area.getHeight() - 8));
    const int gap = 10, width = (cards.getWidth() - 3 * gap) / 4;
    for (auto* card : { &engineCard, &noteCard, &audioCard, &sessionCard }) {
        card->setBounds(cards.removeFromLeft(width));
        cards.removeFromLeft(gap);
    }
    area.removeFromTop(10);
    playerBar->setBounds(area.removeFromTop(72));
}

} // namespace pf::app
