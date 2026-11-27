// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DesktopPanel.h"

#include "Theme.h"

namespace pf::app {

namespace {

constexpr double kScales[] = { 0.0, 1.0, 1.25, 1.5, 1.75, 2.0 }; // 0: automatic (the system's)

struct RowText {
    const char *key, *label, *tooltip;
    bool linuxOnly;
};

constexpr RowText kRows[] = {
    { "keepAwake", "Keep the screen on and the computer awake while playing",
      "A MIDI keyboard is no activity to the desktop: without this, the screen may lock and a laptop sleep while you "
      "play or record.",
      false },
    { "performanceProfile", "Performance power profile while playing",
      "The desktop's \"performance\" profile (power-profiles-daemon) while playing: the processor keeps its speed, and "
      "small audio blocks stay steady. Given back after ten minutes without a note, and when PianoForge closes.",
      true },
    { "mediaControls", "Media keys and the desktop's media controls play the MIDI player",
      "Play, pause, next and previous from the keyboard's media keys, the shell's player widget or a headset (MPRIS).",
      true },
    { "notifyRecordings", "Notify when a recording is saved while PianoForge is in the background",
      "A desktop notification with the recording's name.", true },
    { "singleInstance", "One PianoForge: files and the menu's actions go to the one running",
      "A MIDI file opened from the file manager plays in the PianoForge already open, rather than in a second one "
      "(which would play beside it). From the next start.",
      true },
    { "virtualMidiInput", "A MIDI input named \"PianoForge\", for other programs to play",
      "A DAW, a score editor or a sequencer sends its notes to PianoForge (ALSA and PipeWire list it): they play as "
      "sent, without the keyboard's velocity curve.",
      true },
};

} // namespace

double DesktopPanel::savedScale(const juce::PropertiesFile& settings)
{
    const double s = settings.getDoubleValue("interfaceScale", 0.0);
    return s >= 0.5 && s <= 3.0 ? s : 0.0;
}

DesktopPanel::DesktopPanel(juce::PropertiesFile& s, DesktopIntegration& i) : settings(s), integration(i)
{
    scaleLabel.setText("Size of the interface", juce::dontSendNotification);
    scale.addItem("Automatic (the system's)", 1);
    for (int k = 1; k < (int) std::size(kScales); ++k)
        scale.addItem(juce::String(juce::roundToInt(kScales[k] * 100.0)) + " %", k + 1);
    const double saved = savedScale(settings);
    int selected = 1;
    for (int k = 1; k < (int) std::size(kScales); ++k)
        if (std::abs(kScales[k] - saved) < 0.01)
            selected = k + 1;
    scale.setSelectedId(selected, juce::dontSendNotification);
    scale.onChange = [this] {
        const double factor = kScales[std::clamp(scale.getSelectedId() - 1, 0, (int) std::size(kScales) - 1)];
        settings.setValue("interfaceScale", factor);
        settings.saveIfNeeded();
        juce::Desktop::getInstance().setGlobalScaleFactor(factor > 0.0 ? (float) factor : 1.0f);
    };
#if JUCE_LINUX
    scaleNote.setText("PianoForge draws through XWayland: with the desktop's scale at 125 or 150 %, it can look blurred or "
                      "small. A size of its own here.",
                      juce::dontSendNotification);
#else
    scaleNote.setText("Over the system's own scale.", juce::dontSendNotification);
#endif
    playingNote.setText("Playing: a note in the last ten minutes, a MIDI file playing, or a recording.", juce::dontSendNotification);
    for (auto* l : { &scaleNote, &playingNote }) {
        l->setColour(juce::Label::textColourId, theme::dim);
        l->setFont(theme::font(12.0f));
        l->setJustificationType(juce::Justification::topLeft);
    }
    addAndMakeVisible(scaleLabel);
    addAndMakeVisible(scale);
    addAndMakeVisible(scaleNote);
    addAndMakeVisible(playingNote);

    for (std::size_t k = 0; k < rows.size(); ++k) {
        auto& row = rows[k];
        const auto& text = kRows[k];
        row.key = text.key;
#if !JUCE_LINUX
        row.shown = !text.linuxOnly || (JUCE_MAC && juce::String(text.key) == "virtualMidiInput");
#endif
        row.toggle.setButtonText(text.label);
        row.toggle.setTooltip(text.tooltip);
        row.toggle.setToggleState(settings.getBoolValue(text.key, true), juce::dontSendNotification);
        row.toggle.onClick = [this, k] {
            settings.setValue(rows[k].key, rows[k].toggle.getToggleState());
            settings.saveIfNeeded();
            integration.apply();
            timerCallback();
        };
        row.status.setFont(theme::font(12.0f));
        row.status.setColour(juce::Label::textColourId, theme::dim);
        if (row.shown) {
            addAndMakeVisible(row.toggle);
            addAndMakeVisible(row.status);
        }
    }
    setSize(660, 540);
    timerCallback();
    startTimerHz(2);
}

void DesktopPanel::timerCallback()
{
    const auto s = integration.status();
    const juce::String texts[] = { s.keepAwake, s.performance, s.media, s.notifications, s.instance, s.midi };
    for (std::size_t k = 0; k < rows.size(); ++k)
        rows[k].status.setText(texts[k], juce::dontSendNotification);
}

void DesktopPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, interfaceArea, "Interface");
    theme::drawCard(g, playingArea, "While playing");
    theme::drawCard(g, desktopArea, "Desktop");
}

void DesktopPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    interfaceArea = area.removeFromTop(96);
    area.removeFromTop(10);
    playingArea = area.removeFromTop(148);
    area.removeFromTop(10);
    desktopArea = area;

    auto inner = interfaceArea.reduced(12, 8).withTrimmedTop(20);
    auto line = inner.removeFromTop(26);
    scaleLabel.setBounds(line.removeFromLeft(170));
    scale.setBounds(line.removeFromLeft(220));
    inner.removeFromTop(4);
    scaleNote.setBounds(inner);

    auto place = [](Row& row, juce::Rectangle<int>& box) {
        if (!row.shown)
            return;
        row.toggle.setBounds(box.removeFromTop(24));
        row.status.setBounds(box.removeFromTop(18).withTrimmedLeft(28));
        box.removeFromTop(4);
    };
    inner = playingArea.reduced(12, 8).withTrimmedTop(20);
    playingNote.setBounds(inner.removeFromTop(20));
    place(rows[0], inner);
    place(rows[1], inner);
    inner = desktopArea.reduced(12, 8).withTrimmedTop(20);
    for (std::size_t k = 2; k < rows.size(); ++k)
        place(rows[k], inner);
}

} // namespace pf::app
