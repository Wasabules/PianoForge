#include "AudioPanel.h"

#include "Theme.h"

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
    feedNote.setText("In OBS, add the \"PianoForge\" audio source (plugin in %ProgramData%/obs-studio/plugins/pianoforge-obs): "
                     "it reads this application directly, without a cable. Turn its monitoring off in OBS.",
                     juce::dontSendNotification);
    for (auto* l : { &mirrorNote, &feedNote }) {
        l->setColour(juce::Label::textColourId, theme::dim);
        l->setFont(theme::font(12.0f));
        l->setJustificationType(juce::Justification::topLeft);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &mirrorChoice, &refreshDevices, &mirrorStatus, &mirrorNote,
                                                             &feedStatus, &feedNote })
        addAndMakeVisible(*c);
    mirrorStatus.setText("Off", juce::dontSendNotification);
    setSize(660, 760);
    startTimerHz(4);
}

AudioPanel::~AudioPanel()
{
    stopTimer();
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
}

juce::String AudioPanel::mirrorSummary() const
{
    return host.mirror.isOpen() ? "Mirror: " + host.mirror.currentDevice() : juce::String("Mirror: off");
}

void AudioPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, deviceArea, "Audio device and MIDI inputs");
    theme::drawCard(g, mirrorArea, "Mirror output");
    theme::drawCard(g, feedArea, "OBS plugin");
}

void AudioPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    feedArea = area.removeFromBottom(96);
    area.removeFromBottom(10);
    mirrorArea = area.removeFromBottom(130);
    area.removeFromBottom(10);
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

    inner = feedArea.reduced(12, 8).withTrimmedTop(20);
    feedStatus.setBounds(inner.removeFromTop(20));
    feedNote.setBounds(inner);
}

} // namespace pf::app
