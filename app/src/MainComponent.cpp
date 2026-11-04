#include "MainComponent.h"

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

    // Audio: ask for 48 kHz (the corpus rate) and a small buffer; the saved setting takes precedence.
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.sampleRate = 48000.0;
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

    title.setText("PianoForge - reference corpus listening lab", juce::dontSendNotification);
    title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    for (auto* l : { &title, &corpusStatus, &audioStatus, &noteStatus, &engineStatus, &help })
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
    volume.onValueChange = [this] { player.outputGain = juce::Decibels::decibelsToGain((float) volume.getValue()); };
    volume.setValue(settings->getDoubleValue("volume", 6.0));
    velocityLabel.setText("Velocity", juce::dontSendNotification);
    velocity.setRange(1.0, 127.0, 1.0);
    velocity.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    velocity.onValueChange = [this] { keyboard.setVelocity((float) velocity.getValue() / 127.0f, true); };
    velocity.setValue(settings->getIntValue("velocity", 100));
    for (auto* c : std::initializer_list<juce::Component*> { &volumeLabel, &volume, &velocityLabel, &velocity })
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

    setSize(1400, 470);
    startTimerHz(20);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveSettings();
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

    if (loader.isLoading()) {
        loadProgress = loader.progress();
        corpusStatus.setText("Loading corpus " + corpusFolder.getFullPathName() + "... "
                                 + juce::String(juce::roundToInt(loadProgress * 100.0)) + " %",
                             juce::dontSendNotification);
    } else if (!bankInstalled) {
        if (auto bank = loader.takeResult()) {
            player.installBank(std::move(bank));
            bankInstalled = true;
            updateRenderedLayers();
            progressBar.setVisible(false);
            resized();
            corpusStatus.setText("Corpus: " + corpusFolder.getFullPathName() + "  -  " + loader.status(),
                                 juce::dontSendNotification);
        }
    }

    audioStatus.setText(audioDescription(), juce::dontSendNotification);

    if (const int note = player.lastNote.load(); note >= 0) {
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

    engineStatus.setText("Voices: " + juce::String(player.activeVoices.load()) + " / " + juce::String(ReferencePlayer::kMaxVoices)
                             + "   |   Audio thread load: " + juce::String(deviceManager.getCpuUsage() * 100.0, 1) + " %"
                             + "   |   Stolen voices: " + juce::String((int) player.stolenVoices.load()),
                         juce::dontSendNotification);

    sustainButton.setToggleState(player.sustainDown.load(), juce::dontSendNotification);

    const float peak = player.outputPeak.exchange(0.0f);
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
    title.setBounds(header);

    corpusStatus.setBounds(area.removeFromTop(22));
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
}

} // namespace pf::app
