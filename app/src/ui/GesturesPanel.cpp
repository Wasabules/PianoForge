// SPDX-License-Identifier: AGPL-3.0-or-later

#include "GesturesPanel.h"

#include "Theme.h"

namespace pf::app {

GesturesPanel::GesturesPanel(KeyboardInput& in, juce::PropertiesFile& s) : input(in), settings(s)
{
    addAndMakeVisible(enabled);
    enabled.onClick = [this] {
        input.gesturesOn = enabled.getToggleState();
        settings.setValue("gesturesOn", enabled.getToggleState());
        settings.saveIfNeeded();
        if (onChanged)
            onChanged();
    };
    intro.setText("Keys struck together that no piece asks for, or one key struck twice quickly, played on the MIDI keyboard, "
                  "do what is chosen here. Their notes sound as any others; a recording stopped by a gesture ends before them, "
                  "and a take saved by one leaves them out. The OBS actions need the PianoForge audio source in OBS, its "
                  "\"Let PianoForge's gestures control OBS\" on.",
                  juce::dontSendNotification);
    intro.setColour(juce::Label::textColourId, theme::dim);
    intro.setFont(theme::font(13.0f));
    intro.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(intro);
    for (int a = 0; a < (int) PianoAction::count; ++a) {
        auto& row = rows[(std::size_t) a];
        row.name.setText(pianoActions()[(std::size_t) a].name, juce::dontSendNotification);
        row.name.setFont(theme::font(14.0f));
        row.gesture.setFont(theme::font(14.0f, true));
        row.gesture.setColour(juce::Label::textColourId, theme::accent);
        row.learn.setTooltip("Play the gesture on the MIDI keyboard: keys struck together (two to five), or one key struck twice quickly");
        row.learn.onClick = [this, a] { learning == a ? stopLearning() : startLearning(a); };
        row.clear.onClick = [this, a] { assign(a, {}); };
        row.reset.onClick = [this, a] { assign(a, pf::PianoGesture::parse(pianoActions()[(std::size_t) a].defaultGesture)); };
        for (auto* c : std::initializer_list<juce::Component*> { &row.name, &row.gesture, &row.learn, &row.clear, &row.reset })
            addAndMakeVisible(c);
    }
    reload();
    setSize(720, 150 + 34 * (int) PianoAction::count);
}

GesturesPanel::~GesturesPanel()
{
    stopTimer();
    input.stopLearningGesture();
}

void GesturesPanel::reload()
{
    enabled.setToggleState(settings.getBoolValue("gesturesOn", true), juce::dontSendNotification);
    input.gesturesOn = enabled.getToggleState();
    for (int a = 0; a < (int) PianoAction::count; ++a) {
        const auto& info = pianoActions()[(std::size_t) a];
        const auto key = juce::String("gesture.") + info.key;
        const auto text = settings.containsKey(key) ? settings.getValue(key) : juce::String(info.defaultGesture);
        input.setGesture(a, pf::PianoGesture::parse(text.toStdString()));
    }
    refresh();
    if (onChanged)
        onChanged();
}

void GesturesPanel::assign(int action, const pf::PianoGesture& g)
{
    if (const int other = input.gestureOwner(g, action); other >= 0) { // a gesture does one thing
        input.setGesture(other, {});
        settings.setValue(juce::String("gesture.") + pianoActions()[(std::size_t) other].key, juce::String());
        if (onMessage)
            onMessage("Gesture " + juce::String(g.describe()) + " moved from \"" + pianoActions()[(std::size_t) other].name + "\"");
    }
    input.setGesture(action, g);
    settings.setValue(juce::String("gesture.") + pianoActions()[(std::size_t) action].key, juce::String(g.text()));
    settings.saveIfNeeded();
    refresh();
    if (onChanged)
        onChanged();
}

void GesturesPanel::startLearning(int action)
{
    learning = action;
    input.learnGesture();
    startTimer(50);
    refresh();
}

void GesturesPanel::stopLearning()
{
    learning = -1;
    input.stopLearningGesture();
    stopTimer();
    refresh();
}

void GesturesPanel::timerCallback()
{
    if (learning < 0)
        return;
    if (const auto g = input.learnedGesture()) {
        const int action = learning;
        learning = -1;
        stopTimer();
        assign(action, *g);
        if (onMessage)
            onMessage("Gesture learned: " + juce::String(g->describe()) + " - " + pianoActions()[(std::size_t) action].name);
    }
}

void GesturesPanel::refresh()
{
    for (int a = 0; a < (int) PianoAction::count; ++a) {
        auto& row = rows[(std::size_t) a];
        const auto g = input.gesture(a);
        if (learning == a) {
            row.gesture.setText("play it now...", juce::dontSendNotification);
            row.gesture.setColour(juce::Label::textColourId, theme::busy);
            row.learn.setButtonText("Cancel");
        } else {
            row.gesture.setText(g.empty() ? juce::String("none") : juce::String(g.describe()), juce::dontSendNotification);
            row.gesture.setColour(juce::Label::textColourId, g.empty() ? theme::dim : theme::accent);
            row.learn.setButtonText("Learn");
        }
        row.learn.setEnabled(learning < 0 || learning == a);
        row.clear.setEnabled(learning < 0 && !g.empty());
    }
}

void GesturesPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
}

void GesturesPanel::resized()
{
    auto area = getLocalBounds().reduced(16, 12);
    enabled.setBounds(area.removeFromTop(26));
    area.removeFromTop(4);
    intro.setBounds(area.removeFromTop(72));
    area.removeFromTop(8);
    for (auto& row : rows) {
        auto line = area.removeFromTop(30);
        area.removeFromTop(4);
        row.reset.setBounds(line.removeFromRight(70));
        line.removeFromRight(6);
        row.clear.setBounds(line.removeFromRight(60));
        line.removeFromRight(6);
        row.learn.setBounds(line.removeFromRight(70));
        line.removeFromRight(10);
        row.gesture.setBounds(line.removeFromRight(200));
        row.name.setBounds(line);
    }
}

} // namespace pf::app
