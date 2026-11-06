#include "PlayerBar.h"

#include <cmath>

namespace pf::app {

namespace {

juce::String clock(double seconds)
{
    const int s = juce::jmax(0, (int) std::floor(seconds));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

} // namespace

// ---- TransportButton ----

void TransportButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    auto fill = theme::panel.brighter(0.08f);
    if (getToggleState())
        fill = theme::accent.withAlpha(0.45f);
    if (over)
        fill = fill.brighter(0.1f);
    if (down)
        fill = fill.brighter(0.15f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(theme::panelEdge);
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    g.setColour(isEnabled() ? theme::text : theme::dim.withAlpha(0.6f));
    const float h = juce::jmin(area.getHeight(), area.getWidth()) * 0.42f;
    const auto c = area.getCentre();
    switch (icon) {
    case Icon::play: {
        juce::Path p;
        p.addTriangle(c.x - h * 0.42f, c.y - h * 0.5f, c.x - h * 0.42f, c.y + h * 0.5f, c.x + h * 0.5f, c.y);
        g.fillPath(p);
        break;
    }
    case Icon::pause:
        g.fillRect(juce::Rectangle<float>(c.x - h * 0.42f, c.y - h * 0.5f, h * 0.3f, h));
        g.fillRect(juce::Rectangle<float>(c.x + h * 0.12f, c.y - h * 0.5f, h * 0.3f, h));
        break;
    case Icon::stop:
        g.fillRect(juce::Rectangle<float>(c.x - h * 0.4f, c.y - h * 0.4f, h * 0.8f, h * 0.8f));
        break;
    }
}

// ---- PlayerBar ----

PlayerBar::PlayerBar(MidiFilePlayer& p, juce::PropertiesFile& s) : player(p), settings(s)
{
    openButton.setTooltip("Open a MIDI file (Ctrl+O), or drop one on the window");
    openButton.onClick = [this] {
        if (onOpen)
            onOpen();
    };
    playButton.setTooltip("Play / pause (Ctrl+Space)");
    playButton.onClick = [this] { togglePlay(); };
    stopButton.setTooltip("Stop and back to the start (Ctrl+Home)");
    stopButton.onClick = [this] { stop(); };

    nameLabel.setFont(theme::font(13.0f));
    nameLabel.setMinimumHorizontalScale(0.7f);
    timeLabel.setFont(theme::font(13.0f));
    timeLabel.setJustificationType(juce::Justification::centred);

    position.setSliderStyle(juce::Slider::LinearHorizontal);
    position.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    position.setRange(0.0, 1.0);
    position.setTooltip("Click or drag to move in the file (Ctrl+Left / Ctrl+Right: 5 s)");
    position.onDragStart = [this] { dragging = true; };
    position.onDragEnd = [this] {
        dragging = false;
        player.seek(position.getValue());
        if (onClicked)
            onClicked();
    };
    position.onValueChange = [this] { // while dragging: the time follows the thumb
        if (dragging)
            timeLabel.setText(clock(position.getValue()) + " / " + clock(position.getMaximum()), juce::dontSendNotification);
    };

    speedLabel.setText("Speed", juce::dontSendNotification);
    speedLabel.setColour(juce::Label::textColourId, theme::dim);
    speedLabel.setJustificationType(juce::Justification::centredRight);
    speed.setRange(25.0, 200.0, 1.0);
    speed.setSkewFactorFromMidPoint(100.0);
    speed.setTextValueSuffix(" %");
    speed.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
    speed.setDoubleClickReturnValue(true, 100.0);
    speed.setTooltip("The tempo of the file (double-click: 100 %)");
    speed.onValueChange = [this] {
        player.setSpeed(speed.getValue() / 100.0);
        if (ready && !speed.isMouseButtonDown()) // a typed value or a menu preset; a drag saves at its end
            save();
    };
    speed.onDragEnd = [this] {
        save();
        if (onClicked)
            onClicked();
    };
    speed.setValue(juce::jlimit(25.0, 200.0, settings.getDoubleValue("playerSpeed", 100.0)), juce::sendNotificationSync);

    loopButton.setTooltip("Start again at the end (Ctrl+L)");
    loopButton.setToggleState(settings.getBoolValue("playerLoop", false), juce::dontSendNotification);
    player.setLoop(loopButton.getToggleState());
    loopButton.onClick = [this] {
        player.setLoop(loopButton.getToggleState());
        save();
        if (onClicked)
            onClicked();
    };

    // The keyboard keeps the focus: the computer keyboard goes on playing after a click here.
    for (auto* c : std::initializer_list<juce::Component*> { &openButton, &playButton, &stopButton, &position, &loopButton }) {
        c->setWantsKeyboardFocus(false);
        c->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &timeLabel, &speedLabel, &speed })
        addAndMakeVisible(*c);
    fileChanged();
    ready = true;
}

void PlayerBar::togglePlay()
{
    if (player.wantsToPlay())
        player.pause();
    else
        player.play();
    refresh();
    if (onClicked)
        onClicked();
}

void PlayerBar::stop()
{
    player.stop();
    position.setValue(0.0, juce::dontSendNotification);
    refresh();
    if (onClicked)
        onClicked();
}

void PlayerBar::nudge(double seconds)
{
    if (const auto s = player.sequence())
        player.seek(juce::jlimit(0.0, s->length, player.position() + seconds));
}

void PlayerBar::setSpeed(double factor)
{
    speed.setValue(factor * 100.0, juce::sendNotificationSync); // saved by the slider
}

void PlayerBar::setLoop(bool on)
{
    loopButton.setToggleState(on, juce::dontSendNotification);
    player.setLoop(on);
    save();
}

void PlayerBar::setDropHighlight(bool on)
{
    if (on != dropHighlight) {
        dropHighlight = on;
        repaint();
    }
}

void PlayerBar::fileChanged()
{
    const auto s = player.sequence();
    const bool loaded = s != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &position })
        c->setEnabled(loaded);
    position.setAlpha(loaded ? 1.0f : 0.35f);
    if (loaded) {
        position.setRange(0.0, juce::jmax(0.001, s->length), 0.0);
        juce::String details = juce::String(s->notes) + " notes, " + juce::String(s->tracks) + (s->tracks == 1 ? " track" : " tracks");
        if (s->drumsLeftOut)
            details << ", drums (channel 10) left out";
        nameLabel.setText(s->name + "   (" + details + ")", juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::text);
        nameLabel.setTooltip(s->path);
    } else {
        position.setRange(0.0, 1.0, 0.0);
        nameLabel.setText("No MIDI file: Open, or drop a .mid file on the window", juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::dim);
        nameLabel.setTooltip({});
    }
    position.setValue(0.0, juce::dontSendNotification);
    refresh();
}

void PlayerBar::refresh()
{
    const auto s = player.sequence();
    const bool playing = player.wantsToPlay();
    playButton.setIcon(playing ? TransportButton::Icon::pause : TransportButton::Icon::play);
    playButton.setToggleState(playing, juce::dontSendNotification);
    if (s == nullptr) {
        timeLabel.setText("-:-- / -:--", juce::dontSendNotification);
        return;
    }
    if (!dragging) {
        position.setValue(player.position(), juce::dontSendNotification);
        timeLabel.setText(clock(player.position()) + " / " + clock(s->length), juce::dontSendNotification);
    }
}

void PlayerBar::save()
{
    settings.setValue("playerSpeed", speed.getValue());
    settings.setValue("playerLoop", loopButton.getToggleState());
    settings.saveIfNeeded();
}

void PlayerBar::paint(juce::Graphics& g)
{
    theme::drawCard(g, getLocalBounds(), {});
    if (dropHighlight) {
        g.setColour(theme::accent);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 2.0f);
    }
    g.setColour(theme::dim);
    g.setFont(theme::font(11.0f, true));
    g.drawText("MIDI FILE", getLocalBounds().reduced(12, 0).removeFromLeft(70), juce::Justification::centredLeft);
}

void PlayerBar::resized()
{
    auto area = getLocalBounds().reduced(12, 6);
    area.removeFromLeft(70); // the title
    openButton.setBounds(area.removeFromLeft(104).reduced(0, 1));
    area.removeFromLeft(10);
    playButton.setBounds(area.removeFromLeft(40).reduced(0, 1));
    area.removeFromLeft(4);
    stopButton.setBounds(area.removeFromLeft(40).reduced(0, 1));
    area.removeFromLeft(12);
    loopButton.setBounds(area.removeFromRight(64));
    area.removeFromRight(8);
    speed.setBounds(area.removeFromRight(170));
    speedLabel.setBounds(area.removeFromRight(50));
    area.removeFromRight(12);
    timeLabel.setBounds(area.removeFromRight(100));
    nameLabel.setBounds(area.removeFromLeft(juce::jmin(380, area.getWidth() / 2)));
    area.removeFromLeft(8);
    position.setBounds(area);
}

} // namespace pf::app
