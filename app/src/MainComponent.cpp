#include "MainComponent.h"

#include "OfflineRender.h"

namespace pf::app {

namespace {

constexpr int kLayoutAzerty = 1, kLayoutQwerty = 2;

enum MenuId {
    // File
    kRecord = 1, kRecordMidi, kRecordWav, kSaveTake, kOpenRecordings, kOpenTakes, kChangeCorpus, kQuit, kOpenMidi,
    // Player
    kPlayPause = 80, kStopPlayer, kBack, kForward, kLoop, kCloseMidi, kSpeed = 100,
    // Engine
    kEngineReference = 20, kEngineModal, kEngineOption = 30, kEngineSettings = 50, kRoomToggle, kRoomSettings,
    // Audio
    kAudioSettings = 60, kPerformance, kResetDropouts,
    // Keyboard
    kVelocityCurve = 70, kAzerty, kQwerty, kOctaveDown, kOctaveUp, kVelocityDown, kVelocityUp,
    // Help
    kShortcuts = 90, kAbout,
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

/// Saved as "inputMin;inputMax;p0,p1,p2,p3,p4".
pf::VelocityCurve::Settings parseCurve(const juce::String& text)
{
    pf::VelocityCurve::Settings s;
    const auto parts = juce::StringArray::fromTokens(text, ";", "");
    if (parts.size() != 3)
        return s;
    const auto points = juce::StringArray::fromTokens(parts[2], ",", "");
    if (points.size() != pf::VelocityCurve::kPoints)
        return s;
    s.inputMin = juce::jlimit(1, 126, parts[0].getIntValue());
    s.inputMax = juce::jlimit(s.inputMin + 1, 127, parts[1].getIntValue());
    for (int i = 0; i < pf::VelocityCurve::kPoints; ++i)
        s.points[(size_t) i] = juce::jlimit(0.0f, 1.0f, points[i].getFloatValue());
    return s;
}

juce::String formatCurve(const pf::VelocityCurve::Settings& s)
{
    juce::StringArray points;
    for (const float p : s.points)
        points.add(juce::String(p, 4));
    return juce::String(s.inputMin) + ";" + juce::String(s.inputMax) + ";" + points.joinIntoString(",");
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
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.sampleRate = 44100.0;
    preferred.bufferSize = 128;
    const auto savedAudio = settings->getXmlValue("audio");
    deviceManager.initialise(0, 2, savedAudio.get(), true, {}, &preferred);
    curveSettings = parseCurve(settings->getValue("velocityCurve"));
    velocityCurve.set(curveSettings);
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
    roomPanel->onChange = [this] { refreshRoomButton(); };
    playerBar = std::make_unique<PlayerBar>(host.filePlayer, *settings);
    playerBar->onOpen = [this] { chooseMidiFile(); };
    playerBar->onClicked = [this] { keyboard.grabKeyboardFocus(); };
    addAndMakeVisible(*playerBar);

    // Toolbar: engine (switchable while playing, for A/B listening), recording, volume, velocity, pedal, load, level.
    referenceButton.setTooltip("The reference: Noire's recordings, played back (Ctrl+1)");
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

    recordButton.setTooltip("Record what you play: WAV and MIDI aligned to the sample (Ctrl+R)");
    recordButton.onClick = [this] { toggleRecording(); };
    recordMidi = settings->getBoolValue("recordMidi", true);
    recordWav = settings->getBoolValue("recordWav", true);
    pedalButton.setTooltip("The sustain pedal (Space)");
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
    volume.onValueChange = [this] { player.outputGain = modal.outputGain = juce::Decibels::decibelsToGain((float) volume.getValue()); };
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
    if (corpusFolder.isDirectory())
        loadCorpus(corpusFolder);
    else
        showMessage("No corpus found: File > Change corpus..., and pick corpus/reference/noire-dry/single-notes", theme::busy);

    setSize(1440, 600);
    constructed = true;
    startTimerHz(20);
}

MainComponent::~MainComponent()
{
    stopTimer();
    host.mirror.close();
    if (host.live.recording())
        finishRecording();
    saveSettings();
    if (takeRender.joinable())
        takeRender.join();
    if (modalLoader.joinable())
        modalLoader.join();
    engineWindow.reset();
    audioWindow.reset();
    performanceWindow.reset();
    roomWindow.reset();
    curveWindow.reset(); // it refers to curveSettings and keyboardInput
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
    return { "File", "Player", "Engine", "Audio", "Keyboard", "Help" };
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
        formats.addItem(item(kRecordWav, "WAV (the sound, 32-bit float)", {}, recordWav, !host.live.recording()));
        formats.addItem(item(kRecordMidi, "MIDI (aligned with the WAV)", {}, recordMidi, !host.live.recording()));
        menu.addSubMenu("Record formats", formats);
        menu.addItem(item(kSaveTake, "Save last take (MIDI and WAV render)", "Ctrl+S"));
        menu.addSeparator();
        menu.addItem(item(kOpenRecordings, "Open the recordings folder"));
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
        menu.addItem(item(kEngineReference, "Reference: Noire samples", "Ctrl+1", !modalSelected()));
        menu.addItem(item(kEngineModal, "Modal: PianoForge engine", "Ctrl+2", modalSelected()));
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
        menu.addItem(item(kRoomSettings, "Room settings...", "Ctrl+M"));
        break;
    }
    case 3:
        menu.addItem(item(kAudioSettings, "Audio, MIDI and streaming...", "Ctrl+,"));
        menu.addItem(item(kPerformance, "Performance...", "Ctrl+P"));
        menu.addSeparator();
        menu.addItem(item(kResetDropouts, "Reset the dropout counters"));
        break;
    case 4:
        menu.addItem(item(kVelocityCurve, "Velocity curve..."));
        menu.addSeparator();
        menu.addItem(item(kAzerty, "Computer keyboard: AZERTY", {}, keyLayout == kLayoutAzerty));
        menu.addItem(item(kQwerty, "Computer keyboard: QWERTY", {}, keyLayout == kLayoutQwerty));
        menu.addSeparator();
        menu.addItem(item(kOctaveDown, "Octave down", keyLayout == kLayoutAzerty ? "W" : "Z"));
        menu.addItem(item(kOctaveUp, "Octave up", "X"));
        menu.addItem(item(kVelocityDown, "Velocity -10", "C"));
        menu.addItem(item(kVelocityUp, "Velocity +10", "V"));
        break;
    case 5:
        menu.addItem(item(kShortcuts, "Keyboard shortcuts", "F1"));
        menu.addItem(item(kAbout, "About PianoForge"));
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
    case kRecordWav: recordWav = !recordWav; saveSettings(); break;
    case kSaveTake: saveLastTake(); break;
    case kOpenRecordings: recordingsFolder().createDirectory(); recordingsFolder().startAsProcess(); break;
    case kOpenTakes: takesFolder().createDirectory(); takesFolder().startAsProcess(); break;
    case kChangeCorpus: chooseCorpus(); break;
    case kQuit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
    case kEngineReference: selectEngine(false); break;
    case kEngineModal: selectEngine(true); break;
    case kEngineSettings: showWindow(engineWindow, "Engine settings", *enginePanel); break;
    case kRoomToggle: roomPanel->setOn(!roomPanel->isOn()); break;
    case kRoomSettings: showWindow(roomWindow, "Room", *roomPanel); break;
    case kAudioSettings: showWindow(audioWindow, "Audio, MIDI and streaming", *audioPanel); break;
    case kPerformance: showWindow(performanceWindow, "Performance", performancePanel); break;
    case kResetDropouts: performancePanel.onReset(); break;
    case kVelocityCurve: openVelocityCurve(); break;
    case kAzerty: setKeyLayout(kLayoutAzerty); break;
    case kQwerty: setKeyLayout(kLayoutQwerty); break;
    case kOctaveDown: shiftOctave(-1); break;
    case kOctaveUp: shiftOctave(1); break;
    case kVelocityDown: velocity.setValue(velocity.getValue() - 10.0); break;
    case kVelocityUp: velocity.setValue(velocity.getValue() + 10.0); break;
    case kShortcuts: showShortcuts(); break;
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
            + "  C / V: velocity -10 / +10\n  Space: sustain pedal\n  B: sostenuto pedal (held)\n\n"
              "MIDI file player\n  Ctrl+O: open a MIDI file (or drop it on the window)\n  Ctrl+Space: play / pause\n"
              "  Ctrl+Home: stop, back to the start\n  Ctrl+Left / Ctrl+Right: 5 s back / forward\n  Ctrl+L: loop\n\n"
              "Application\n  Ctrl+R: record / stop\n  Ctrl+S: save the last take\n  Ctrl+1 / Ctrl+2: reference / modal engine\n"
              "  Ctrl+E: engine settings\n  Ctrl+M: room settings\n  Ctrl+,: audio, MIDI and streaming\n  Ctrl+P: performance\n  F1: this list\n  Ctrl+Q: quit\n\n"
              "The mouse plays too: lower on a key is louder.");
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::NoIcon, "About PianoForge",
        "PianoForge " + juce::String(PIANOFORGE_VERSION) + "\n\nA concert grand played from a MIDI keyboard: a modal engine "
        "calibrated on a reference library, with the reference itself for A/B listening.\n\nAudio thread: lock-free, "
        "no allocation. Recording, the mirror output and the OBS plugin's feed only copy each block.");
}

// ---- Actions ----

void MainComponent::refreshRoomButton()
{
    roomButton.setToggleState(roomPanel->isOn(), juce::dontSendNotification);
    roomButton.setTooltip("The room around the piano: " + roomPanel->presetName()
                          + (roomPanel->isOn() ? juce::String(" (click: off)") : juce::String(" (off; click: on)"))
                          + ". Ctrl+M: its settings");
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
                     + "   C / V velocity   Space pedal   B sostenuto   Ctrl+R record   F1 all the shortcuts",
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
        case 'P': showWindow(performanceWindow, "Performance", performancePanel); return true;
        case ',': showWindow(audioWindow, "Audio, MIDI and streaming", *audioPanel); return true;
        case '1': selectEngine(false); return true;
        case '2': selectEngine(true); return true;
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

void MainComponent::toggleRecording()
{
    if (host.live.recording()) {
        finishRecording();
        return;
    }
    if (!recordMidi && !recordWav) {
        showMessage("Nothing to record: tick a format in File > Record formats.", theme::busy);
        return;
    }
    if (deviceManager.getCurrentAudioDevice() == nullptr || host.currentRate() <= 0.0) {
        showMessage("No audio device is running: Audio > Audio, MIDI and streaming.", theme::bad);
        return;
    }
    recordingStem = "PianoForge " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    const auto wavFile = recordWav ? recordingsFolder().getChildFile(recordingStem + ".wav") : juce::File();
    const auto engineName = modalSelected() ? juce::String("modal engine") : juce::String("reference player");
    if (const auto started = host.live.start(wavFile, host.currentRate(), host.currentSegment(),
                                             "PianoForge live recording (" + engineName + ")");
        started.failed()) {
        showMessage("Cannot record: " + started.getErrorMessage(), theme::bad);
        return;
    }
    recordButton.setColour(juce::TextButton::buttonColourId, theme::record);
}

void MainComponent::finishRecording()
{
    const auto span = host.live.stop();
    recordButton.removeColour(juce::TextButton::buttonColourId);
    drained.clear(); // every event up to the stop, into the session record
    host.drainRecorded(drained);
    takes.add(drained, host.currentSegment(), host.currentRate());
    if (!span.valid) {
        if (span.wav != juce::File())
            span.wav.deleteFile();
        showMessage("Recording failed: " + (span.error.isNotEmpty() ? span.error : juce::String("nothing recorded")), theme::bad);
        return;
    }
    juce::StringArray saved;
    int notes = 0;
    if (recordMidi) {
        if (auto take = takes.span(span.segment, span.start, span.end)) {
            take->text = "PianoForge live recording " + recordingStem + ", " + juce::String(juce::roundToInt(span.sampleRate))
                       + " Hz: one tick per sample, tick 0 is the first sample of the WAV of the same name";
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
    if (span.wav.existsAsFile())
        saved.add(".wav");
    lastSaved = recordingStem + " " + saved.joinIntoString(" + ") + " (" + juce::String((double) (span.end - span.start) / span.sampleRate, 1)
              + " s, " + juce::String(notes) + " notes)";
    auto text = "Recorded " + lastSaved + " in " + recordingsFolder().getFullPathName();
    if (span.dropped > 0)
        text << "  WARNING: " << juce::String(span.dropped) << " samples lost (the disk did not keep up)";
    if (span.error.isNotEmpty())
        text << "  " << span.error;
    showMessage(text, span.dropped > 0 || span.error.isNotEmpty() ? theme::busy : theme::good);
}

void MainComponent::saveLastTake()
{
    const auto take = takes.lastTake();
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
    auto bank = player.sharedBank();
    auto profile = modal.sharedProfile();
    const bool withModal = modalSelected() && profile != nullptr;
    if ((withModal ? profile == nullptr : bank == nullptr) || renderingTake.load()) {
        showMessage("Saved " + midiFile.getFileName() + " (" + summary + ")", theme::good);
        return;
    }
    showMessage("Saved " + midiFile.getFileName() + " (" + summary + "); rendering the WAV...");
    if (takeRender.joinable())
        takeRender.join();
    renderingTake = true;
    // Same engine and settings as heard live, at the rate it was played: the WAV is the take as heard.
    // Every setting of the engine heard (tuning, sympathetic resonance, pedal catch, release noise...) and the room,
    // as they are now; the tail is longer by the room's.
    takeRender = std::thread([sequence = TakeRecorder::toSequence(*take), bank = std::move(bank), profile = std::move(profile),
                              withModal, settings = modal.settings(), room = host.room.settings(), rate = take->sampleRate,
                              restrike = enginePanel->restrikeSeconds(), whole = enginePanel->isOn(EnginePanel::Option::wholeTake),
                              wavFile, summary, safe = juce::Component::SafePointer<MainComponent>(this)] {
        const double tail = 4.0 + (room.enabled ? pf::Room::tailSeconds(room) : 0.0);
        auto audio = withModal ? renderSequenceModal(sequence, profile, rate, settings, tail)
                               : renderSequence(sequence, bank, rate, restrike, whole, tail);
        applyRoom(audio, room, rate);
        const auto written = writeWav(audio, rate, wavFile);
        juce::MessageManager::callAsync([safe, written, wavFile, summary] {
            if (auto* self = safe.getComponent()) {
                self->renderingTake = false;
                if (written.wasOk())
                    self->showMessage("Saved " + wavFile.getFileNameWithoutExtension() + " .mid and .wav (" + summary + ") in "
                                          + wavFile.getParentDirectory().getFullPathName(),
                                      theme::good);
                else
                    self->showMessage("WAV render failed: " + written.getErrorMessage(), theme::bad);
            }
        });
    });
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
        juce::MessageManager::callAsync([safe, profile = std::move(profile), error]() mutable {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            if (profile == nullptr) {
                self->profileSummary = "Modal profile: " + juce::String(error);
                self->showMessage("The modal profile did not load: " + juce::String(error), theme::bad);
                return;
            }
            std::size_t layers = 0;
            for (const auto& n : profile->notes)
                layers = std::max(layers, n.layers.size());
            self->profileSummary = "Modal profile: " + juce::String((int) profile->notes.size()) + " notes x "
                                 + juce::String((int) layers) + " layers, " + juce::String(juce::roundToInt((double) profile->bytes / 1.0e6)) + " MB";
            self->modal.install(std::move(profile));
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
    saveSettings(); // the audio device changed
}

void MainComponent::saveSettings()
{
    if (!constructed)
        return;
    auto* s = properties.getUserSettings();
    if (const auto xml = deviceManager.createStateXml())
        s->setValue("audio", xml.get());
    s->setValue("corpus", corpusFolder.getFullPathName());
    s->setValue("volume", volume.getValue());
    s->setValue("velocity", (int) velocity.getValue());
    s->setValue("keyLayout", keyLayout);
    s->setValue("velocityCurve", formatCurve(curveSettings));
    s->setValue("engine", modalSelected() ? 2 : 1);
    s->setValue("recordMidi", recordMidi);
    s->setValue("recordWav", recordWav);
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
                     + (modal.sostenutoDown.load() ? ", sostenuto" : "") + ", "
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
    return { d->getName() + " (" + d->getTypeName() + ")",
             juce::String(fs / 1000.0, 1) + " kHz, blocks of " + juce::String(block) + " (" + juce::String(1000.0 * block / fs, 1)
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
    if (fs > 50000.0)
        tips.add("At " + juce::String(juce::roundToInt(fs / 1000.0)) + " kHz the engine does " + juce::String(fs / 48000.0, 1)
                 + " times the work of 48 kHz for nothing audible: the corpus is at 44.1 kHz. Use 44.1 or 48 kHz.");
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

void MainComponent::refreshDashboard()
{
    // Engine.
    auto engineInfo = engineLines();
    engineCard.setLines({ modalSelected() ? "Modal: PianoForge" : "Reference: Noire samples", engineInfo[0], engineInfo[1],
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
    if (host.live.recording()) {
        const double t = host.live.seconds();
        sessionCard.setLines({ juce::String::formatted("Recording  %d:%04.1f", (int) t / 60, std::fmod(t, 60.0)),
                               recordingStem + (recordWav ? " .wav" : "") + (recordMidi ? " .mid" : ""),
                               host.feedOn() ? "OBS plugin fed" : "OBS plugin not fed (another PianoForge)", audioPanel->mirrorSummary() },
                             theme::record);
    } else {
        sessionCard.setLines({ "Not recording",
                               lastSaved.isEmpty() ? juce::String("Ctrl+R records, Ctrl+S saves the last take") : "Last: " + lastSaved,
                               host.feedOn() ? "OBS plugin fed" : "OBS plugin not fed (another PianoForge)", audioPanel->mirrorSummary() });
    }
}

void MainComponent::timerCallback()
{
    if (!focusGiven && isShowing()) {
        keyboard.grabKeyboardFocus();
        focusGiven = true;
    }

    drained.clear(); // take capture: move what reached the engine into the session record
    host.drainRecorded(drained);
    takes.add(drained, host.currentSegment(), host.currentRate());
    if (host.live.endedByDevice()) // the device stopped or changed: the recording ends there
        finishRecording();
    recordButton.setButtonText(host.live.recording()
                                   ? juce::String::formatted("Stop  %d:%04.1f", (int) host.live.seconds() / 60, std::fmod(host.live.seconds(), 60.0))
                                   : juce::String("Record"));

    playerBar->refresh();

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
        engine.insert(0, modalSelected() ? "Modal: PianoForge engine" : "Reference: Noire samples");
        performancePanel.update(load, engine, deviceLines(), adviceLines());
    }
    const bool sustain = modalSelected() ? modal.sustainDown.load() : player.sustainDown.load();
    pedalButton.setToggleState(sustain, juce::dontSendNotification);

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
