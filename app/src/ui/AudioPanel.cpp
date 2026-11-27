// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AudioPanel.h"

#include "Theme.h"
#include "../AudioDevices.h"
#include "../LinuxAudio.h"
#include "../ObsFlatpak.h"
#include "../PipeWireAudio.h"

#include <thread>

namespace pf::app {

AudioPanel::AudioPanel(juce::AudioDeviceManager& d, AudioHost& h, juce::PropertiesFile& s)
    : devices(d), host(h), settings(s), selector(d, 0, 0, 1, 2, true, false, true, false)
{
    addAndMakeVisible(selector);

    mirrorChoice.setTooltip("A copy of the sound on another device, through the system's mixer (for a virtual cable). "
                            "The OBS plugin does not need it.");
    fillMirrorDevices();
    if (const int i = mirrorDevices.indexOf(settings.getValue("mirrorDevice")); i >= 0)
        mirrorChoice.setSelectedId(i + 2, juce::dontSendNotification);
    else
        mirrorChoice.setSelectedId(1, juce::dontSendNotification);
    mirrorChoice.onChange = [this] {
        openMirror();
        settings.setValue("mirrorDevice", mirrorChoice.getSelectedId() >= 2 ? mirrorDevices[mirrorChoice.getSelectedId() - 2] : juce::String());
        settings.saveIfNeeded();
    };
    refreshDevices.onClick = [this] {
        const auto current = mirrorChoice.getSelectedId() >= 2 ? mirrorDevices[mirrorChoice.getSelectedId() - 2] : juce::String();
        fillMirrorDevices();
        const int i = mirrorDevices.indexOf(current);
        mirrorChoice.setSelectedId(i >= 0 ? i + 2 : 1, juce::dontSendNotification);
    };
    mirrorNote.setText("A copy of what the main output plays, on another device through the system's mixer: a virtual "
                       "cable for streaming software (both ends of the cable at the same sample rate). The main output "
                       "is not touched; the copy follows its clock.",
                       juce::dontSendNotification);
#if JUCE_WINDOWS
    feedNote.setText("In OBS, add the \"PianoForge\" audio source (plugin in %ProgramData%/obs-studio/plugins/pianoforge-obs): "
                     "it reads this application directly, without a cable. Turn its monitoring off in OBS.",
                     juce::dontSendNotification);
#else
    feedNote.setText("In OBS, add the \"PianoForge\" audio source (Flathub's OBS: obs/install-flatpak.sh installs the plugin "
                     "and lets OBS see PianoForge): it reads this application directly, without a cable. Turn its "
                     "monitoring off in OBS.",
                     juce::dontSendNotification);
#endif
    rateChoice.addItem("Engine at 44.1 or 48 kHz, brought up to the device's rate (recommended)", 1);
    rateChoice.addItem("Engine at the device's rate", 2);
    rateChoice.setSelectedId(settings.getIntValue("engineRateMode", 0) == 1 ? 2 : 1, juce::dontSendNotification);
    rateChoice.onChange = [this] {
        const int mode = rateChoice.getSelectedId() == 2 ? 1 : 0;
        settings.setValue("engineRateMode", mode);
        settings.saveIfNeeded();
        applyRateMode(mode);
    };
    rateNote.setText("A device may run fast (the Windows mixer imposes up to 192 kHz; a PipeWire graph may be set to 96 or "
                     "192 kHz): the engine then runs at "
                     "the device's rate divided by 2, 4 or 8 (44.1 or 48 kHz), four times less work at 192 kHz, and its "
                     "sound is brought up to the device's (flat to 20 kHz, images 90 dB down, 0.7 ms). Recordings, takes "
                     "and streams are at the engine's rate.",
                     juce::dontSendNotification);
    limiterOn.setTooltip("Nothing over -1 dBFS reaches the device, a recording or the OBS plugin: a loud chord with the room "
                         "is brought down at once, then let back within a second. No latency; untouched while under it.");
    limiterOn.setToggleState(settings.getBoolValue("outputLimiter", true), juce::dontSendNotification);
    host.limiter.enabled = limiterOn.getToggleState();
    limiterOn.onClick = [this] {
        host.limiter.enabled = limiterOn.getToggleState();
        settings.setValue("outputLimiter", limiterOn.getToggleState());
        settings.saveIfNeeded();
    };
#if JUCE_LINUX
    pipewireForce.setTooltip("PipeWire's graph runs at the block chosen above while PianoForge plays (node.force-quantum), "
                             "under its own minimum (clock.min-quantum); every program's sound goes at that pace "
                             "meanwhile (a little more work for the processor), and as before once PianoForge closes.");
    pipewireForce.setToggleState(pipewire::forceQuantum().load(), juce::dontSendNotification);
    pipewireForce.onClick = [this] {
        pipewire::forceQuantum() = pipewireForce.getToggleState();
        settings.setValue("pipewireForceQuantum", pipewireForce.getToggleState());
        settings.saveIfNeeded();
        if (auto* device = devices.getCurrentAudioDevice(); device != nullptr && throughPipeWire(device)) { // asked for when it opens
            devices.closeAudioDevice();
            devices.restartLastAudioDevice();
        }
    };
    pipewireNote.setText("With PipeWire (most Linux desktops), the PipeWire device type plays straight into its graph, in "
                         "its realtime thread, at the block chosen: 64 samples, about 3 ms from key to sound; 128, about 5. "
                         "ALSA's PipeWire device keeps four blocks of its own before it; JACK is PipeWire's once its JACK "
                         "library is installed (the package pipewire-jack).",
                         juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &pipewireForce, &pipewireStatus, &pipewireNote })
        addAndMakeVisible(*c);
    obsFlatpakAllow.setTooltip("flatpak override --user --device=shm com.obsproject.Studio: a Flatpak keeps a shared memory of its "
                               "own, where PianoForge's sound is not. OBS sees it from its next start.");
    obsFlatpakAllow.onClick = [this] {
        const auto error = obsflatpak::allowSharedMemory();
        obsFlatpakStatus.setText(error.isEmpty() ? juce::String("Done: restart OBS") : "Not done: " + error, juce::dontSendNotification);
        checkObsFlatpak();
    };
    obsFlatpakStatus.setFont(theme::font(12.0f));
    addChildComponent(obsFlatpakStatus);
    addChildComponent(obsFlatpakAllow);
    checkObsFlatpak();
#endif
    for (auto* l : { &mirrorNote, &feedNote, &rateNote, &pipewireNote }) {
        l->setColour(juce::Label::textColourId, theme::dim);
        l->setFont(theme::font(12.0f));
        l->setJustificationType(juce::Justification::topLeft);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &mirrorChoice, &refreshDevices, &mirrorStatus, &mirrorNote,
                                                             &feedStatus, &feedNote, &rateChoice, &rateStatus, &rateNote,
                                                             &limiterOn, &limiterStatus })
        addAndMakeVisible(*c);
    mirrorStatus.setText("Off", juce::dontSendNotification);
    if (settings.getIntValue("engineRateMode", 0) == 1) // the device started in the default mode: again in this one
        applyRateMode(1);
#if JUCE_LINUX
    setSize(660, 1120);
#else
    setSize(660, 950);
#endif
    startTimerHz(4);
}

AudioPanel::~AudioPanel()
{
    stopTimer();
}

void AudioPanel::checkObsFlatpak()
{
    std::thread([safe = juce::Component::SafePointer<AudioPanel>(this)] {
        const auto state = obsflatpak::query();
        juce::MessageManager::callAsync([safe, state] {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            self->obsFlatpakStatus.setVisible(state.installed);
            self->obsFlatpakAllow.setVisible(state.installed && !state.sharedMemory);
            if (!state.installed)
                return;
            juce::String text = "Flathub's OBS: ";
            text << (state.plugin ? "the plugin is in it" : "the plugin is not in it yet (obs/install-flatpak.sh)") << "; "
                 << (state.sharedMemory ? "it sees PianoForge" : "it cannot see PianoForge yet");
            self->obsFlatpakStatus.setText(text, juce::dontSendNotification);
            self->obsFlatpakStatus.setColour(juce::Label::textColourId, state.plugin && state.sharedMemory ? theme::good : theme::busy);
        });
    }).detach();
}

void AudioPanel::applyRateMode(int mode)
{
    if (host.rateMode.load() == mode)
        return;
    host.rateMode = mode;
    if (devices.getCurrentAudioDevice() != nullptr) { // the engine's rate is set when the device starts
        devices.closeAudioDevice();
        devices.restartLastAudioDevice();
    }
}

void AudioPanel::fillMirrorDevices()
{
    mirrorDevices = MirrorOutput::deviceNames();
    mirrorChoice.clear(juce::dontSendNotification);
    mirrorChoice.addItem("Off", 1);
    for (int i = 0; i < mirrorDevices.size(); ++i)
        mirrorChoice.addItem(mirrorDevices[i], i + 2);
}

void AudioPanel::openMirror()
{
    const int id = mirrorChoice.getSelectedId();
    if (id < 2) {
        host.mirror.close();
        mirrorStatus.setText("Off", juce::dontSendNotification);
        return;
    }
    const auto name = mirrorDevices[id - 2];
    if (auto* main = devices.getCurrentAudioDevice(); main != nullptr && main->getName() == name) {
        mirrorStatus.setText("This is the main output already: pick another device", juce::dontSendNotification);
        host.mirror.close();
        return;
    }
    const auto opened = host.mirror.open(name, host.mirror.mainSampleRate(), host.mirror.mainBlockSize());
    mirrorStatus.setText(opened.wasOk() ? host.mirror.status() : "Cannot open " + name + ": " + opened.getErrorMessage(),
                         juce::dontSendNotification);
}

void AudioPanel::timerCallback()
{
    // The mirror opens once the main device runs, and again when its rate or block size changes.
    const auto text = mirrorStatus.getText();
    if (mirrorChoice.getSelectedId() >= 2 && host.mirror.mainSampleRate() > 0.0 && (!host.mirror.isOpen() || host.mirror.needsReopen())
        && !text.startsWith("Cannot") && !text.startsWith("This is"))
        openMirror();
    if (host.mirror.isOpen())
        mirrorStatus.setText(host.mirror.status(), juce::dontSendNotification);
    feedStatus.setText(host.feedOn() ? "Feeding the OBS plugin" : "Not feeding: another PianoForge is feeding the OBS plugin",
                       juce::dontSendNotification);
    feedStatus.setColour(juce::Label::textColourId, host.feedOn() ? theme::good : theme::busy);
    limiterDeepest = std::min(limiterDeepest, host.limiter.takeReduction());
    limiterStatus.setText(!host.limiter.enabled.load() ? juce::String("Off: nothing stops an over")
                          : limiterDeepest < 0.0f ? "Acted, at most " + juce::String(limiterDeepest, 1) + " dB (since this window opened)"
                                                  : juce::String("Never needed so far"),
                          juce::dontSendNotification);
    if (const double device = host.deviceRate(); device > 0.0)
        rateStatus.setText("Engine at " + juce::String(host.currentRate() / 1000.0, 1) + " kHz, device at " + juce::String(device / 1000.0, 1)
                               + " kHz" + (host.rateFactor() > 1 ? " (x" + juce::String(host.rateFactor()) + ", brought up)" : juce::String()),
                           juce::dontSendNotification);
    else
        rateStatus.setText("No device running", juce::dontSendNotification);
#if JUCE_LINUX
    auto* device = devices.getCurrentAudioDevice();
    juce::String state;
    juce::Colour colour = theme::text;
    if (!linuxaudio::pipewireRunning()) {
        state = "PipeWire is not running: ALSA and JACK as they are";
    } else if (const auto pw = pipewire::status(device); pw.has_value() && pw->graphBlock > 0 && pw->graphRate > 0.0) {
        state = "Its graph at " + juce::String(pw->graphBlock) + " samples, " + juce::String(pw->graphRate / 1000.0, 1) + " kHz ("
               + juce::String(1000.0 * pw->graphBlock / pw->graphRate, 1) + " ms, " + (pw->forced ? "asked for" : "its own")
               + "), " + juce::String(pw->latencyMs, 1) + " ms to the device"
               + (pw->xruns > 0 ? ", " + juce::String((int) pw->xruns) + " xruns: a larger block" : juce::String());
        colour = pw->xruns > 0 ? theme::busy : theme::good;
        if (const auto out = pipewire::output(device); out.bluetooth) {
            state << "; " << juce::String::fromUTF8(out.description.c_str()) << " is Bluetooth: 100 to 250 ms more";
            colour = theme::busy;
        }
    } else if (device != nullptr && throughPipeWire(device)) {
        state = device->getTypeName() + " through PipeWire" + (pipewire::forceQuantum().load()
                                                             ? ", its graph asked for " + juce::String(device->getCurrentBufferSizeSamples()) + " samples"
                                                             : juce::String(", at its graph's own block"));
    } else if (device != nullptr) {
        state = device->getTypeName() + ", not through PipeWire";
    }
    if (linuxaudio::pipewireRunning() && !linuxaudio::pipewireJack())
        state << (state.isEmpty() ? "" : "; ") << "JACK: PipeWire's not installed";
    pipewireStatus.setText(state, juce::dontSendNotification);
    pipewireStatus.setColour(juce::Label::textColourId, colour);
#endif
}

juce::String AudioPanel::mirrorSummary() const
{
    return host.mirror.isOpen() ? "Mirror: " + host.mirror.currentDevice() : juce::String("Mirror: off");
}

void AudioPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, deviceArea, "Audio device and MIDI inputs");
    theme::drawCard(g, rateArea, "Engine rate");
    theme::drawCard(g, limiterArea, "Output");
    if (!pipewireArea.isEmpty())
        theme::drawCard(g, pipewireArea, "PipeWire (latency)");
    theme::drawCard(g, mirrorArea, "Mirror output");
    theme::drawCard(g, feedArea, "OBS plugin");
}

void AudioPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
#if JUCE_LINUX
    feedArea = area.removeFromBottom(126);
#else
    feedArea = area.removeFromBottom(96);
#endif
    area.removeFromBottom(10);
    mirrorArea = area.removeFromBottom(130);
    area.removeFromBottom(10);
    rateArea = area.removeFromBottom(120);
    area.removeFromBottom(10);
    limiterArea = area.removeFromBottom(60);
    area.removeFromBottom(10);
#if JUCE_LINUX
    pipewireArea = area.removeFromBottom(130);
    area.removeFromBottom(10);
#endif
    deviceArea = area;
    selector.setBounds(deviceArea.reduced(6, 4).withTrimmedTop(20));

    auto inner = mirrorArea.reduced(12, 8).withTrimmedTop(20);
    auto row = inner.removeFromTop(26);
    refreshDevices.setBounds(row.removeFromRight(80).reduced(0, 1));
    row.removeFromRight(6);
    mirrorChoice.setBounds(row.removeFromLeft(320));
    row.removeFromLeft(8);
    mirrorStatus.setBounds(row);
    inner.removeFromTop(4);
    mirrorNote.setBounds(inner);

    inner = limiterArea.reduced(12, 8).withTrimmedTop(20);
    row = inner.removeFromTop(24);
    limiterOn.setBounds(row.removeFromLeft(260));
    limiterStatus.setBounds(row);

    inner = rateArea.reduced(12, 8).withTrimmedTop(20);
    row = inner.removeFromTop(26);
    rateChoice.setBounds(row.removeFromLeft(420));
    row.removeFromLeft(8);
    rateStatus.setBounds(row);
    inner.removeFromTop(4);
    rateNote.setBounds(inner);

    if (!pipewireArea.isEmpty()) {
        inner = pipewireArea.reduced(12, 8).withTrimmedTop(20);
        pipewireForce.setBounds(inner.removeFromTop(24).removeFromLeft(440));
        pipewireStatus.setBounds(inner.removeFromTop(20));
        inner.removeFromTop(2);
        pipewireNote.setBounds(inner);
    }

    inner = feedArea.reduced(12, 8).withTrimmedTop(20);
    feedStatus.setBounds(inner.removeFromTop(20));
#if JUCE_LINUX
    auto flatpak = inner.removeFromBottom(26);
    obsFlatpakAllow.setBounds(flatpak.removeFromRight(190).reduced(0, 1));
    flatpak.removeFromRight(8);
    obsFlatpakStatus.setBounds(flatpak);
#endif
    feedNote.setBounds(inner);
}

} // namespace pf::app
