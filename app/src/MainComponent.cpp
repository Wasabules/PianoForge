#include "MainComponent.h"

#include "OfflineRender.h"

namespace pf::app {

namespace {

constexpr int kLayoutAzerty = 1, kLayoutQwerty = 2;

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

    title.setText("PianoForge - listening lab", juce::dontSendNotification);
    title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    for (auto* l : { &title, &corpusStatus, &modalStatus, &audioStatus, &noteStatus, &engineStatus, &help })
        addAndMakeVisible(*l);
    help.setColour(juce::Label::textColourId, juce::Colours::grey);

    audioButton.setButtonText("Audio and MIDI settings...");
    audioButton.onClick = [this] { openAudioSettings(); };
    corpusButton.setButtonText("Change corpus...");
    corpusButton.onClick = [this] { chooseCorpus(); };
    curveButton.setButtonText("Velocity curve...");
    curveButton.onClick = [this] { openVelocityCurve(); };
    addAndMakeVisible(audioButton);
    addAndMakeVisible(corpusButton);
    addAndMakeVisible(curveButton);

    // Engine: switchable while playing, for A/B listening from the keyboard.
    engineChoice.addItem("Reference: Noire samples", kEngineReference);
    engineChoice.addItem("Modal: PianoForge engine", kEngineModal);
    engineChoice.setSelectedId(settings->getIntValue("engine", kEngineReference) == kEngineModal ? kEngineModal : kEngineReference,
                               juce::dontSendNotification);
    engineChoice.onChange = [this] {
        engines.selected = modalSelected() ? 1 : 0;
        saveSettings();
    };
    engines.selected = modalSelected() ? 1 : 0;
    addAndMakeVisible(engineChoice);
    bodyButton.setButtonText("Soundboard body");
    bodyButton.setToggleState(settings->getBoolValue("modalBody", true), juce::dontSendNotification);
    bodyButton.onClick = [this] { modal.bodyEnabled = bodyButton.getToggleState(); saveSettings(); };
    residualButton.setButtonText("Attack residual");
    residualButton.setToggleState(settings->getBoolValue("modalResidual", true), juce::dontSendNotification);
    residualButton.onClick = [this] { modal.residualEnabled = residualButton.getToggleState(); saveSettings(); };
    modal.bodyEnabled = bodyButton.getToggleState();
    modal.residualEnabled = residualButton.getToggleState();
    addAndMakeVisible(bodyButton);
    addAndMakeVisible(residualButton);

    // Modal engine: what a piano does and the reference library does not can be switched off for A/B listening.
    struct Switch {
        juce::ToggleButton& button;
        const char* text;
        const char* key;
        std::atomic<bool>& target;
        const char* tip;
    };
    for (const auto& s : { Switch { sympatheticButton, "Sympathetic resonance", "modalSympathetic", modal.sympatheticEnabled,
                                    "Free strings (pedal down) ring with what is played" },
                           Switch { heldKeysButton, "Held keys resonate", "modalHeldKeys", modal.heldKeysResonate,
                                    "The strings of keys held down ring too, without the pedal (a piano does; the reference does not)" },
                           Switch { pedalCatchButton, "Pedal catches releases", "modalPedalCatch", modal.pedalCatch,
                                    "Pressing the pedal again keeps what is left of the released notes (a piano does; the reference does not)" },
                           Switch { releaseNoiseButton, "Release noise", "modalReleaseNoise", modal.releaseNoiseEnabled,
                                    "The knock of the dampers landing, on the soundboard" } }) {
        s.button.setButtonText(s.text);
        s.button.setTooltip(s.tip);
        s.button.setToggleState(settings->getBoolValue(s.key, true), juce::dontSendNotification);
        s.target = s.button.getToggleState();
        s.button.onClick = [this, &button = s.button, &target = s.target] {
            target = button.getToggleState();
            saveSettings();
        };
        addAndMakeVisible(s.button);
    }

    // Tuning of the modal engine: reference A4 and temperament.
    tuningLabel.setText("Tuning (modal): A4", juce::dontSendNotification);
    referenceA4.setRange(415.0, 466.0, 0.1);
    referenceA4.setTextValueSuffix(" Hz");
    referenceA4.setNumDecimalPlacesToDisplay(1);
    referenceA4.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    referenceA4.setValue(settings->getDoubleValue("referenceA4", 440.0), juce::dontSendNotification);
    referenceA4.onValueChange = [this] { applyTuning(); };
    referenceA4.onDragEnd = [this] { saveSettings(); };
    referenceA4.setDoubleClickReturnValue(true, 440.0);
    for (int i = 0; i < (int) kTemperaments.size(); ++i)
        temperamentChoice.addItem(kTemperaments[(size_t) i].name, i + 1);
    temperamentChoice.setSelectedId(juce::jlimit(1, (int) kTemperaments.size(), settings->getIntValue("temperament", 1)),
                                    juce::dontSendNotification);
    const char* keys[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    for (int i = 0; i < 12; ++i)
        temperamentRoot.addItem(juce::String("on ") + keys[i], i + 1);
    temperamentRoot.setSelectedId(juce::jlimit(1, 12, settings->getIntValue("temperamentRoot", 1)), juce::dontSendNotification);
    temperamentChoice.onChange = temperamentRoot.onChange = [this] { applyTuning(); saveSettings(); };
    for (auto* c : std::initializer_list<juce::Component*> { &tuningLabel, &referenceA4, &temperamentChoice, &temperamentRoot })
        addAndMakeVisible(*c);
    applyTuning();

    // Stream output: a copy of the sound on another device (a virtual cable for OBS), the main device untouched.
    mirrorLabel.setText("Stream output (OBS)", juce::dontSendNotification);
    mirrorChoice.setTooltip("A copy of the sound on another device, through the Windows mixer: pick a virtual cable "
                            "(VB-Audio Hi-Fi Cable Input) and add its output to OBS as an audio input. The main output "
                            "(ASIO) is not affected.");
    fillMirrorDevices();
    if (const int i = mirrorDevices.indexOf(settings->getValue("mirrorDevice")); i >= 0)
        mirrorChoice.setSelectedId(i + 2, juce::dontSendNotification);
    else
        mirrorChoice.setSelectedId(1, juce::dontSendNotification);
    mirrorChoice.onChange = [this] { openMirror(); saveSettings(); };
    for (auto* c : std::initializer_list<juce::Component*> { &mirrorLabel, &mirrorChoice, &mirrorStatus })
        addAndMakeVisible(*c);

    saveTakeButton.setButtonText("Save last take");
    saveTakeButton.onClick = [this] { saveLastTake(); };
    takesFolderButton.setButtonText("Takes folder");
    takesFolderButton.onClick = [] {
        takesFolder().createDirectory();
        takesFolder().startAsProcess();
    };
    takeStatus.setText("Every note is captured. 'Save last take' keeps what was played since the last 10 s of silence, "
                       "as a MIDI file and a WAV render, in " + takesFolder().getFullPathName(),
                       juce::dontSendNotification);
    addAndMakeVisible(saveTakeButton);
    addAndMakeVisible(takesFolderButton);
    addAndMakeVisible(takeStatus);

    // Live recording: what the device plays, as it plays it (WAV), and the MIDI over the same span.
    recordButton.setButtonText("Record");
    recordButton.setTooltip("Start or stop a live recording (Ctrl+R)");
    recordButton.onClick = [this] { toggleRecording(); };
    recordMidiButton.setButtonText("MIDI");
    recordMidiButton.setToggleState(settings->getBoolValue("recordMidi", true), juce::dontSendNotification);
    recordMidiButton.onClick = [this] { saveSettings(); };
    recordWavButton.setButtonText("WAV");
    recordWavButton.setToggleState(settings->getBoolValue("recordWav", true), juce::dontSendNotification);
    recordWavButton.onClick = [this] { saveSettings(); };
    recordingsFolderButton.setButtonText("Recordings folder");
    recordingsFolderButton.onClick = [] {
        recordingsFolder().createDirectory();
        recordingsFolder().startAsProcess();
    };
    recordStatus.setText("Record (Ctrl+R): the sound as it leaves the engine, in a 32-bit float WAV, and the MIDI aligned "
                         "with it to the sample, in " + recordingsFolder().getFullPathName(),
                         juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &recordMidiButton, &recordWavButton,
                                                             &recordingsFolderButton, &recordStatus })
        addAndMakeVisible(*c);

    sustainButton.setButtonText("Sustain pedal (Space)");
    sustainButton.onClick = [this] { sendSustain(sustainButton.getToggleState()); };
    wholeTakeButton.setButtonText("Play the whole take (ignore key release)");
    wholeTakeButton.setToggleState(settings->getBoolValue("playWholeTake", false), juce::dontSendNotification);
    wholeTakeButton.onClick = [this] { player.playWholeTake = wholeTakeButton.getToggleState(); saveSettings(); };
    player.playWholeTake = wholeTakeButton.getToggleState();
    addAndMakeVisible(sustainButton);
    addAndMakeVisible(wholeTakeButton);

    volumeLabel.setText("Volume", juce::dontSendNotification);
    volume.setRange(-24.0, 24.0, 0.5);
    volume.setTextValueSuffix(" dB");
    volume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    volume.onValueChange = [this] {
        player.outputGain = modal.outputGain = juce::Decibels::decibelsToGain((float) volume.getValue());
    };
    volume.setValue(settings->getDoubleValue("volume", 6.0));
    velocityLabel.setText("Velocity", juce::dontSendNotification);
    velocity.setRange(1.0, 127.0, 1.0);
    velocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    velocity.onValueChange = [this] { keyboard.setVelocity((float) velocity.getValue() / 127.0f, true); };
    velocity.setValue(settings->getIntValue("velocity", 100));
    restrikeLabel.setText("Re-strike: previous note fades to -40 dB in", juce::dontSendNotification);
    restrikeFade.setRange(0.015, 5.0, 0.005);
    restrikeFade.setSkewFactorFromMidPoint(0.8);
    restrikeFade.setTextValueSuffix(" s");
    restrikeFade.setNumDecimalPlacesToDisplay(2);
    restrikeFade.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    restrikeFade.onValueChange = [this] { player.restrikeFadeSeconds = modal.restrikeFadeSeconds = (float) restrikeFade.getValue(); };
    restrikeFade.setValue(settings->getDoubleValue("restrikeFade", 0.8));
    restrikeFade.onDragEnd = [this] { saveSettings(); };
    for (auto* c : std::initializer_list<juce::Component*> { &volumeLabel, &volume, &velocityLabel, &velocity,
                                                             &restrikeLabel, &restrikeFade })
        addAndMakeVisible(*c);

    keyLayout.addItem("AZERTY", kLayoutAzerty);
    keyLayout.addItem("QWERTY", kLayoutQwerty);
    keyLayout.setSelectedId(settings->getIntValue("keyLayout", kLayoutAzerty), juce::dontSendNotification);
    keyLayout.onChange = [this] { applyKeyLayout(); saveSettings(); };
    addAndMakeVisible(keyLayout);

    keyboard.setAvailableRange(21, 108); // A0 to C8
    keyboard.setLowestVisibleKey(21);
    keyboard.setScrollButtonsVisible(false);
    keyboard.setOctaveForMiddleC(4);
    keyboard.setWantsKeyboardFocus(true);
    keyboard.addKeyListener(this);
    addAndMakeVisible(keyboard);
    applyKeyLayout();

    addChildComponent(progressBar);

    corpusFolder = findCorpus(settings->getValue("corpus"));
    if (corpusFolder.isDirectory())
        loadCorpus(corpusFolder);
    else
        corpusStatus.setText("No corpus found: use 'Change corpus...' and pick corpus/reference/noire-dry/single-notes",
                             juce::dontSendNotification);

    setSize(1400, 630);
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
    curveWindow.reset(); // it refers to curveSettings and keyboardInput
    keyboard.removeKeyListener(this);
    deviceManager.removeChangeListener(this);
    deviceManager.removeMidiInputDeviceCallback({}, &keyboardInput);
    deviceManager.removeAudioCallback(&host);
    deviceManager.closeAudioDevice();
}

void MainComponent::enableMidiInputs()
{
    for (const auto& input : juce::MidiInput::getAvailableDevices())
        if (!deviceManager.isMidiInputDeviceEnabled(input.identifier))
            deviceManager.setMidiInputDeviceEnabled(input.identifier, true);
}

void MainComponent::applyKeyLayout()
{
    // AZERTY: home row q s d f g h j k l m = white keys, top row = black keys.
    const bool azerty = keyLayout.getSelectedId() != kLayoutQwerty;
    const char* keys = azerty ? "qzsedftgyhujkolpm" : "awsedftgyhujkolp;";
    keyboard.clearKeyMappings();
    for (int i = 0; keys[i] != 0; ++i)
        keyboard.setKeyPressForNote(juce::KeyPress(keys[i]), i);
    keyboard.setKeyPressBaseOctave(octave + 1); // JUCE: octave n -> C = 12 * n; C4 (60) = octave 5
    help.setText(juce::String("Mouse: click a key (lower on the key = louder).   Computer keyboard: ")
                     + (azerty ? "Q Z S E D F T G Y H U J K... = C to E;  W / X: octave;  C / V: velocity;  Space: sustain."
                               : "A W S E D F T G Y H U J K... = C to E;  Z / X: octave;  C / V: velocity;  Space: sustain.")
                     + "   Current octave: C" + juce::String(octave),
                 juce::dontSendNotification);
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'R') {
        toggleRecording();
        return true;
    }
    const auto c = (int) juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    const bool azerty = keyLayout.getSelectedId() != kLayoutQwerty;
    if (c == (azerty ? 'w' : 'z') || c == 'x') {
        octave = juce::jlimit(0, 7, octave + (c == 'x' ? 1 : -1));
        applyKeyLayout();
        return true;
    }
    if (c == 'c' || c == 'v') {
        velocity.setValue(velocity.getValue() + (c == 'v' ? 10.0 : -10.0));
        return true;
    }
    return key == juce::KeyPress::spaceKey; // the pedal is handled in keyStateChanged
}

bool MainComponent::keyStateChanged(bool, juce::Component*)
{
    const bool space = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
    if (space != spaceDown) {
        spaceDown = space;
        sendSustain(space);
    }
    return false; // let the on-screen keyboard handle the other keys
}

void MainComponent::sendSustain(bool down)
{
    host.send(juce::MidiMessage::controllerEvent(1, 64, down ? 127 : 0));
}

void MainComponent::openAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 0, 0, 1, 2, true, false, true, false);
    selector->setSize(600, 520);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(selector);
    o.dialogTitle = "Audio and MIDI settings";
    o.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
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

void MainComponent::toggleRecording()
{
    if (host.live.recording()) {
        finishRecording();
        return;
    }
    const bool midi = recordMidiButton.getToggleState(), wav = recordWavButton.getToggleState();
    if (!midi && !wav) {
        recordStatus.setText("Nothing to record: tick MIDI, WAV or both.", juce::dontSendNotification);
        return;
    }
    if (deviceManager.getCurrentAudioDevice() == nullptr || host.currentRate() <= 0.0) {
        recordStatus.setText("No audio device is running: see Audio and MIDI settings.", juce::dontSendNotification);
        return;
    }
    recordingStem = "PianoForge " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    const auto wavFile = wav ? recordingsFolder().getChildFile(recordingStem + ".wav") : juce::File();
    const auto engineName = modalSelected() ? juce::String("modal engine") : juce::String("reference player");
    if (const auto started = host.live.start(wavFile, host.currentRate(), host.currentSegment(),
                                             "PianoForge live recording (" + engineName + ")");
        started.failed()) {
        recordStatus.setText("Cannot record: " + started.getErrorMessage(), juce::dontSendNotification);
        return;
    }
    recordButton.setButtonText("Stop");
    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
}

void MainComponent::finishRecording()
{
    const auto span = host.live.stop();
    recordButton.setButtonText("Record");
    recordButton.removeColour(juce::TextButton::buttonColourId);
    drained.clear(); // every event up to the stop, into the session record
    host.drainRecorded(drained);
    takes.add(drained, host.currentSegment(), host.currentRate());
    if (!span.valid) {
        if (span.wav != juce::File())
            span.wav.deleteFile();
        recordStatus.setText("Recording failed: " + (span.error.isNotEmpty() ? span.error : juce::String("nothing recorded")),
                             juce::dontSendNotification);
        return;
    }
    juce::StringArray saved;
    int notes = 0;
    if (recordMidiButton.getToggleState()) {
        if (auto take = takes.span(span.segment, span.start, span.end)) {
            take->text = "PianoForge live recording " + recordingStem + ", " + juce::String(span.sampleRate, 0)
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
    auto text = "Saved " + recordingStem + " " + saved.joinIntoString(" and ") + " ("
              + juce::String((double) (span.end - span.start) / span.sampleRate, 1) + " s, " + juce::String(notes)
              + " notes) in " + recordingsFolder().getFullPathName();
    if (span.dropped > 0)
        text << "  WARNING: " << juce::String(span.dropped) << " samples lost (the disk did not keep up)";
    if (span.error.isNotEmpty())
        text << "  " << span.error;
    recordStatus.setText(text, juce::dontSendNotification);
}

void MainComponent::saveLastTake()
{
    const auto take = takes.lastTake();
    if (!take.has_value() || take->notes == 0) {
        takeStatus.setText("Nothing played yet: play something, then save it.", juce::dontSendNotification);
        return;
    }
    const auto folder = takesFolder();
    folder.createDirectory();
    const auto stem = "take-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    const auto midiFile = folder.getChildFile(stem + ".mid"), wavFile = folder.getChildFile(stem + ".wav");
    {
        juce::FileOutputStream out(midiFile);
        if (!out.openedOk() || !TakeRecorder::toMidiFile(*take).writeTo(out)) {
            takeStatus.setText("Cannot write " + midiFile.getFullPathName(), juce::dontSendNotification);
            return;
        }
    }
    const auto summary = juce::String(take->notes) + " notes, " + juce::String((double) take->lengthSamples / take->sampleRate, 1) + " s";
    auto bank = player.sharedBank();
    auto profile = modal.sharedProfile();
    const bool withModal = modalSelected() && profile != nullptr;
    if ((withModal ? profile == nullptr : bank == nullptr) || renderingTake.load()) {
        takeStatus.setText("Saved " + midiFile.getFileName() + " (" + summary + ").", juce::dontSendNotification);
        return;
    }
    takeStatus.setText("Saved " + midiFile.getFileName() + " (" + summary + "); rendering the WAV...", juce::dontSendNotification);
    if (takeRender.joinable())
        takeRender.join();
    renderingTake = true;
    // Same engine and settings as heard live, at the rate it was played: the WAV is the take as heard.
    takeRender = std::thread([sequence = TakeRecorder::toSequence(*take), bank = std::move(bank), profile = std::move(profile),
                              withModal, body = bodyButton.getToggleState(), residual = residualButton.getToggleState(),
                              rate = take->sampleRate, restrike = (float) restrikeFade.getValue(),
                              whole = wholeTakeButton.getToggleState(), wavFile, summary,
                              safe = juce::Component::SafePointer<MainComponent>(this)] {
        const auto audio = withModal ? renderSequenceModal(sequence, profile, rate, restrike, body, residual)
                                     : renderSequence(sequence, bank, rate, restrike, whole);
        const auto written = writeWav(audio, rate, wavFile);
        juce::MessageManager::callAsync([safe, written, wavFile, summary] {
            if (auto* self = safe.getComponent()) {
                self->renderingTake = false;
                self->takeStatus.setText(written.wasOk() ? "Saved " + wavFile.getFileNameWithoutExtension() + " .mid and .wav ("
                                                               + summary + ") in " + wavFile.getParentDirectory().getFullPathName()
                                                         : "WAV render failed: " + written.getErrorMessage(),
                                         juce::dontSendNotification);
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
        modalProfileText = "Modal profile: none in " + path.getParentDirectory().getFullPathName()
                         + " (forge modes, forge soundboard, then forge export)";
        return;
    }
    if (modalLoader.joinable())
        modalLoader.join();
    modalProfileText = "Modal profile: loading " + path.getFullPathName() + "...";
    modalLoader = std::thread([path, safe = juce::Component::SafePointer<MainComponent>(this)] {
        std::string error;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto profile = pf::loadModalProfile(path.getFullPathName().toStdString(), error);
        const double ms = juce::Time::getMillisecondCounterHiRes() - start;
        juce::MessageManager::callAsync([safe, profile = std::move(profile), error, ms]() mutable {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            if (profile == nullptr) {
                self->modalProfileText = "Modal profile: " + juce::String(error);
                return;
            }
            std::size_t layers = 0, modes = 0;
            for (const auto& n : profile->notes) {
                layers = std::max(layers, n.layers.size());
                for (const auto& l : n.layers)
                    modes += l.modes.size();
            }
            self->modalProfileText = "Modal profile: " + juce::String((int) profile->notes.size()) + " notes x "
                                   + juce::String((int) layers) + " velocity layers, " + juce::String((int) modes) + " modes, "
                                   + juce::String((int) profile->bodyFreq.size()) + " soundboard resonances, "
                                   + juce::String((double) profile->bytes / 1.0e6, 1) + " MB (loaded in "
                                   + juce::String(ms / 1000.0, 2) + " s)";
            self->modal.install(std::move(profile));
        });
    });
}

void MainComponent::applyTuning()
{
    const auto& t = kTemperaments[(size_t) juce::jlimit(0, (int) kTemperaments.size() - 1, temperamentChoice.getSelectedId() - 1)];
    const auto offsets = pitchClassOffsets(t, juce::jlimit(0, 11, temperamentRoot.getSelectedId() - 1));
    modal.referenceA4 = (float) referenceA4.getValue();
    for (int pc = 0; pc < 12; ++pc)
        modal.pitchClassCents[(size_t) pc] = (float) offsets[(size_t) pc];
    modal.tuningVersion.fetch_add(1); // the free strings follow
}

void MainComponent::fillMirrorDevices()
{
    mirrorDevices = MirrorOutput::deviceNames();
    mirrorChoice.clear(juce::dontSendNotification);
    mirrorChoice.addItem("Off", 1);
    for (int i = 0; i < mirrorDevices.size(); ++i)
        mirrorChoice.addItem(mirrorDevices[i], i + 2);
}

void MainComponent::openMirror()
{
    const int id = mirrorChoice.getSelectedId();
    if (id < 2) {
        host.mirror.close();
        mirrorStatus.setText("Off", juce::dontSendNotification);
        return;
    }
    const auto name = mirrorDevices[id - 2];
    if (auto* main = deviceManager.getCurrentAudioDevice(); main != nullptr && main->getName() == name) {
        mirrorStatus.setText("This is the main output already: pick another device", juce::dontSendNotification);
        host.mirror.close();
        return;
    }
    const auto opened = host.mirror.open(name, host.mirror.mainSampleRate(), host.mirror.mainBlockSize());
    mirrorStatus.setText(opened.wasOk() ? host.mirror.status() : "Cannot open " + name + ": " + opened.getErrorMessage(),
                         juce::dontSendNotification);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    saveSettings(); // the audio device changed
}

void MainComponent::saveSettings()
{
    auto* s = properties.getUserSettings();
    if (const auto xml = deviceManager.createStateXml())
        s->setValue("audio", xml.get());
    s->setValue("corpus", corpusFolder.getFullPathName());
    s->setValue("volume", volume.getValue());
    s->setValue("velocity", (int) velocity.getValue());
    s->setValue("keyLayout", keyLayout.getSelectedId());
    s->setValue("playWholeTake", wholeTakeButton.getToggleState());
    s->setValue("velocityCurve", formatCurve(curveSettings));
    s->setValue("restrikeFade", restrikeFade.getValue());
    s->setValue("engine", engineChoice.getSelectedId());
    s->setValue("modalBody", bodyButton.getToggleState());
    s->setValue("modalResidual", residualButton.getToggleState());
    s->setValue("recordMidi", recordMidiButton.getToggleState());
    s->setValue("modalSympathetic", sympatheticButton.getToggleState());
    s->setValue("modalHeldKeys", heldKeysButton.getToggleState());
    s->setValue("modalPedalCatch", pedalCatchButton.getToggleState());
    s->setValue("modalReleaseNoise", releaseNoiseButton.getToggleState());
    s->setValue("referenceA4", referenceA4.getValue());
    s->setValue("temperament", temperamentChoice.getSelectedId());
    s->setValue("temperamentRoot", temperamentRoot.getSelectedId());
    s->setValue("mirrorDevice", mirrorChoice.getSelectedId() >= 2 ? mirrorDevices[mirrorChoice.getSelectedId() - 2] : juce::String());
    s->setValue("recordWav", recordWavButton.getToggleState());
    s->saveIfNeeded();
}

juce::String MainComponent::audioDescription() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr)
        return "No audio output open: see 'Audio and MIDI settings...'";
    const double fs = d->getCurrentSampleRate();
    const int block = d->getCurrentBufferSizeSamples();
    const double latencyMs = 1000.0 * (d->getOutputLatencyInSamples() + block) / fs;
    auto text = "Output: " + d->getTypeName() + " | " + d->getName() + " | " + juce::String(fs / 1000.0, 1) + " kHz | buffer "
              + juce::String(block) + " samples | output latency ~" + juce::String(latencyMs, 1) + " ms";
    if (const auto* b = player.bank(); b != nullptr && b->sampleRate > 0 && std::abs(b->sampleRate - fs) > 0.5)
        text << "  (corpus at " << juce::String(b->sampleRate / 1000.0, 1) << " kHz: resampled)";
    int inputs = 0;
    for (const auto& e : juce::MidiInput::getAvailableDevices())
        inputs += deviceManager.isMidiInputDeviceEnabled(e.identifier) ? 1 : 0;
    text << "   |   Active MIDI inputs: " << inputs;
    return text;
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
    // Stream output: opened once the main device runs, again when its rate or block size changes.
    if (mirrorChoice.getSelectedId() >= 2 && host.mirror.mainSampleRate() > 0.0
        && (!host.mirror.isOpen() || host.mirror.needsReopen()) && !mirrorStatus.getText().startsWith("Cannot")
        && !mirrorStatus.getText().startsWith("This is"))
        openMirror();
    if (host.mirror.isOpen())
        mirrorStatus.setText(host.mirror.status(), juce::dontSendNotification);
    if (host.live.endedByDevice()) { // the device stopped or changed: the recording ends there
        finishRecording();
    } else if (host.live.recording()) {
        const double t = host.live.seconds();
        auto text = juce::String::formatted("Recording %d:%04.1f", (int) t / 60, std::fmod(t, 60.0)) + "  -  " + recordingStem
                  + (recordWavButton.getToggleState() ? " .wav" : "") + (recordMidiButton.getToggleState() ? " .mid" : "");
        if (const auto lost = host.live.droppedSamples(); lost > 0)
            text << "  (" << juce::String(lost) << " samples lost)";
        recordStatus.setText(text, juce::dontSendNotification);
    }

    if (loader.isLoading()) {
        loadProgress = loader.progress();
        corpusStatus.setText("Loading corpus " + corpusFolder.getFullPathName() + "... "
                                 + juce::String(juce::roundToInt(loadProgress * 100.0)) + " %",
                             juce::dontSendNotification);
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
            corpusStatus.setText("Corpus: " + corpusFolder.getFullPathName() + "  -  " + loader.status(),
                                 juce::dontSendNotification);
        }
    }

    audioStatus.setText(audioDescription(), juce::dontSendNotification);
    modalStatus.setText(modalProfileText, juce::dontSendNotification);

    if (const int modalNote = modal.lastNote.load(); modalSelected() && modalNote >= 0) {
        const int layer = modal.lastLayerVelocity.load();
        noteStatus.setText("Last note: " + juce::MidiMessage::getMidiNoteName(modalNote, true, true, 4) + " | played velocity "
                               + juce::String(modal.lastVelocity.load())
                               + (layer > 0 ? " -> modal layer " + juce::String(layer) + ", levels interpolated per partial"
                                            : juce::String(" -> no modal data for this note"))
                               + (keyboardInput.lastInput.load() > 0
                                      ? "   |   last MIDI keyboard key: " + juce::String(keyboardInput.lastInput.load())
                                            + " -> " + juce::String(keyboardInput.lastOutput.load()) + " (velocity curve)"
                                      : juce::String()),
                           juce::dontSendNotification);
    } else if (const int note = player.lastNote.load(); !modalSelected() && note >= 0) {
        const int rendered = player.lastRenderedVelocity.load();
        noteStatus.setText("Last note: " + juce::MidiMessage::getMidiNoteName(note, true, true, 4) + " | played velocity "
                               + juce::String(player.lastVelocity.load())
                               + (rendered > 0 ? " -> take rendered at velocity " + juce::String(rendered)
                                               : juce::String(" -> no take for this note in the corpus"))
                               + (keyboardInput.lastInput.load() > 0
                                      ? "   |   last MIDI keyboard key: " + juce::String(keyboardInput.lastInput.load())
                                            + " -> " + juce::String(keyboardInput.lastOutput.load()) + " (velocity curve)"
                                      : juce::String()),
                           juce::dontSendNotification);
    }

    if (modalSelected())
        engineStatus.setText("Modal engine | voices: " + juce::String(modal.activeVoices.load()) + " / "
                                 + juce::String(pf::ModalEngine::kMaxVoices) + "   |   resonators: "
                                 + juce::String(modal.activeModes.load()) + "   |   free strings ringing: "
                                 + juce::String(modal.ringingStrings.load()) + "   |   pedal "
                                 + juce::String(juce::roundToInt(modal.pedalPosition.load() * 100.0f)) + " %   |   Audio thread load: "
                                 + juce::String(deviceManager.getCpuUsage() * 100.0, 1) + " %   |   Stolen voices: "
                                 + juce::String((int) modal.stolenVoices.load()),
                             juce::dontSendNotification);
    else
        engineStatus.setText("Reference player | voices: " + juce::String(player.activeVoices.load()) + " / "
                                 + juce::String(ReferencePlayer::kMaxVoices) + "   |   Audio thread load: "
                                 + juce::String(deviceManager.getCpuUsage() * 100.0, 1) + " %   |   Stolen voices: "
                                 + juce::String((int) player.stolenVoices.load()),
                             juce::dontSendNotification);

    sustainButton.setToggleState(modalSelected() ? modal.sustainDown.load() : player.sustainDown.load(),
                                 juce::dontSendNotification);

    const float peak = juce::jmax(player.outputPeak.exchange(0.0f), modal.outputPeak.exchange(0.0f));
    displayedPeak = juce::jmax(peak, displayedPeak * 0.85f);
    repaint(peakArea);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Output peak, from -60 to 0 dBFS.
    const float db = juce::Decibels::gainToDecibels(displayedPeak, -60.0f);
    auto area = peakArea.toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.fillRoundedRectangle(area, 3.0f);
    const float fill = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(displayedPeak >= 1.0f ? juce::Colours::red : (db > -6.0f ? juce::Colours::orange : juce::Colours::limegreen));
    g.fillRoundedRectangle(area.withWidth(area.getWidth() * fill), 3.0f);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(displayedPeak >= 1.0f ? juce::String("CLIPPING") : "Peak " + juce::String(db, 1) + " dBFS", peakArea,
               juce::Justification::centred);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto bottom = area.removeFromBottom(170);
    keyboard.setBounds(bottom);
    keyboard.setKeyWidth((float) bottom.getWidth() / 52.0f); // 52 white keys from A0 to C8
    area.removeFromBottom(6);
    help.setBounds(area.removeFromBottom(22));

    auto header = area.removeFromTop(30);
    audioButton.setBounds(header.removeFromRight(200).reduced(2));
    corpusButton.setBounds(header.removeFromRight(160).reduced(2));
    curveButton.setBounds(header.removeFromRight(150).reduced(2));
    header.removeFromRight(12);
    residualButton.setBounds(header.removeFromRight(130));
    bodyButton.setBounds(header.removeFromRight(140));
    engineChoice.setBounds(header.removeFromRight(240).reduced(2));
    title.setBounds(header);

    corpusStatus.setBounds(area.removeFromTop(22));
    modalStatus.setBounds(area.removeFromTop(22));
    progressBar.setBounds(area.removeFromTop(progressBar.isVisible() ? 16 : 0));
    audioStatus.setBounds(area.removeFromTop(22));
    noteStatus.setBounds(area.removeFromTop(22));
    engineStatus.setBounds(area.removeFromTop(22));
    area.removeFromTop(8);

    auto row = area.removeFromTop(28);
    volumeLabel.setBounds(row.removeFromLeft(60));
    volume.setBounds(row.removeFromLeft(240));
    row.removeFromLeft(16);
    velocityLabel.setBounds(row.removeFromLeft(60));
    velocity.setBounds(row.removeFromLeft(240));
    row.removeFromLeft(16);
    keyLayout.setBounds(row.removeFromLeft(100).reduced(0, 2));
    row.removeFromLeft(16);
    sustainButton.setBounds(row.removeFromLeft(180));
    wholeTakeButton.setBounds(row.removeFromLeft(300));
    row.removeFromLeft(16);
    peakArea = row.removeFromLeft(220).reduced(0, 4);

    area.removeFromTop(4);
    row = area.removeFromTop(28);
    restrikeLabel.setBounds(row.removeFromLeft(270));
    restrikeFade.setBounds(row.removeFromLeft(300));
    row.removeFromLeft(16);
    sympatheticButton.setBounds(row.removeFromLeft(180));
    heldKeysButton.setBounds(row.removeFromLeft(160));
    pedalCatchButton.setBounds(row.removeFromLeft(190));
    releaseNoiseButton.setBounds(row.removeFromLeft(140));

    area.removeFromTop(4);
    row = area.removeFromTop(28);
    tuningLabel.setBounds(row.removeFromLeft(130));
    referenceA4.setBounds(row.removeFromLeft(300));
    row.removeFromLeft(12);
    temperamentChoice.setBounds(row.removeFromLeft(220).reduced(0, 2));
    row.removeFromLeft(8);
    temperamentRoot.setBounds(row.removeFromLeft(90).reduced(0, 2));
    row.removeFromLeft(24);
    mirrorLabel.setBounds(row.removeFromLeft(130));
    mirrorChoice.setBounds(row.removeFromLeft(300).reduced(0, 2));
    row.removeFromLeft(8);
    mirrorStatus.setBounds(row);

    area.removeFromTop(6);
    row = area.removeFromTop(28);
    saveTakeButton.setBounds(row.removeFromLeft(140).reduced(2));
    takesFolderButton.setBounds(row.removeFromLeft(120).reduced(2));
    row.removeFromLeft(8);
    takeStatus.setBounds(row);

    area.removeFromTop(6);
    row = area.removeFromTop(28);
    recordButton.setBounds(row.removeFromLeft(140).reduced(2));
    recordMidiButton.setBounds(row.removeFromLeft(70));
    recordWavButton.setBounds(row.removeFromLeft(70));
    recordingsFolderButton.setBounds(row.removeFromLeft(140).reduced(2));
    row.removeFromLeft(8);
    recordStatus.setBounds(row);
}

} // namespace pf::app
