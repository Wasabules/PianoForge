#include "PlayerBar.h"

#include <cmath>

namespace pf::app {

namespace {

juce::String clock(double seconds)
{
    const int s = juce::jmax(0, (int) std::floor(seconds));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

/// The time at the speed set: the counter goes on one second per second, against the duration at that speed;
/// away from 100 %, the file's own time follows in brackets.
juce::String timeText(double position, double length, double speed)
{
    if (std::abs(speed - 1.0) < 1e-3)
        return clock(position) + " / " + clock(length);
    return clock(position / speed) + " / " + clock(length / speed) + "   (file " + clock(position) + " / " + clock(length) + ")";
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

// ---- The parts of a MIDI file, in a callout ----

namespace {

/// The parts of the file (the instruments), each switched on or off; All / None.
class PartList final : public juce::Component {
public:
    PartList(MidiFilePlayer& p, std::function<void()> changed) : player(p), onChanged(std::move(changed))
    {
        const auto s = player.sequence();
        for (int i = 0; s != nullptr && i < (int) s->parts.size(); ++i) {
            const auto& part = s->parts[(std::size_t) i];
            juce::String text = juce::String(i + 1) + ". ";
            text << (part.name.empty() ? "Track " + juce::String(part.track + 1) : juce::String::fromUTF8(part.name.c_str()));
            text << "   (channel " << (part.channel + 1);
            if (const auto instrument = part.instrument(); !instrument.empty())
                text << ", " << juce::String(instrument);
            text << ", " << part.notes << " notes)";
            auto* toggle = toggles.add(new juce::ToggleButton(text));
            toggle->setToggleState(player.partEnabled(i), juce::dontSendNotification);
            toggle->onClick = [this, i, toggle] {
                player.setPartEnabled(i, toggle->getToggleState());
                if (onChanged)
                    onChanged();
            };
            content.addAndMakeVisible(toggle);
        }
        for (auto* b : { &all, &none }) {
            b->onClick = [this, b] {
                for (int i = 0; i < toggles.size(); ++i) {
                    toggles[i]->setToggleState(b == &all, juce::dontSendNotification);
                    player.setPartEnabled(i, b == &all);
                }
                if (onChanged)
                    onChanged();
            };
            addAndMakeVisible(*b);
        }
        title.setText("Parts to play (a track and a channel each: one instrument)", juce::dontSendNotification);
        title.setFont(theme::font(12.0f, true));
        title.setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(title);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);
        setSize(480, juce::jlimit(110, 420, 64 + 26 * toggles.size()));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto top = area.removeFromTop(24);
        none.setBounds(top.removeFromRight(60).reduced(0, 1));
        top.removeFromRight(6);
        all.setBounds(top.removeFromRight(60).reduced(0, 1));
        title.setBounds(top);
        area.removeFromTop(6);
        viewport.setBounds(area);
        content.setSize(area.getWidth() - 12, 26 * toggles.size());
        for (int i = 0; i < toggles.size(); ++i)
            toggles[i]->setBounds(0, 26 * i, content.getWidth(), 24);
    }

private:
    MidiFilePlayer& player;
    std::function<void()> onChanged;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::TextButton all { "All" }, none { "None" };
    juce::Label title;
    juce::Viewport viewport;
    juce::Component content;
};

juce::String transposeText(int semitones)
{
    if (semitones == 0)
        return "0";
    const juce::String sign = semitones > 0 ? "+" : "-";
    const int n = std::abs(semitones);
    juce::String s = sign + juce::String(n) + " st";
    if (n % 12 == 0)
        s << " (" << sign << n / 12 << " oct)";
    return s;
}

} // namespace

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
    timeLabel.setTooltip("The time at the speed set (one second per second, against the duration at that speed); "
                         "away from 100 %, the file's own time in brackets");

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
            timeLabel.setText(timeText(position.getValue(), position.getMaximum(), player.getSpeed()), juce::dontSendNotification);
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

    partsButton.setTooltip("The parts of the file (its instruments) to play: every one but the drums at first");
    partsButton.onClick = [this] { showParts(); };
    transposeLabel.setText("Transpose", juce::dontSendNotification);
    transposeLabel.setColour(juce::Label::textColourId, theme::dim);
    transposeLabel.setJustificationType(juce::Justification::centredRight);
    transposeValue.setJustificationType(juce::Justification::centred);
    transposeValue.setFont(theme::font(13.0f));
    transposeValue.setTooltip("Semitones added to every note of the file (back to 0 with every new file)");
    octaveDown.onClick = [this] { transposeBy(-12); };
    semitoneDown.onClick = [this] { transposeBy(-1); };
    semitoneUp.onClick = [this] { transposeBy(1); };
    octaveUp.onClick = [this] { transposeBy(12); };
    octaveDown.setTooltip("An octave down");
    octaveUp.setTooltip("An octave up");
    semitoneDown.setTooltip("A semitone down");
    semitoneUp.setTooltip("A semitone up");

    // The keyboard keeps the focus: the computer keyboard goes on playing after a click here.
    for (auto* c : std::initializer_list<juce::Component*> { &openButton, &playButton, &stopButton, &position, &loopButton,
                                                             &partsButton, &octaveDown, &semitoneDown, &semitoneUp, &octaveUp }) {
        c->setWantsKeyboardFocus(false);
        c->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &timeLabel, &speedLabel, &speed, &transposeLabel, &transposeValue })
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

void PlayerBar::transposeBy(int semitones)
{
    if (player.sequence() == nullptr)
        return;
    player.setTranspose(player.getTranspose() + semitones);
    showTranspose();
    if (onClicked)
        onClicked();
}

void PlayerBar::showTranspose()
{
    const int t = player.getTranspose();
    transposeValue.setText(transposeText(t), juce::dontSendNotification);
    transposeValue.setColour(juce::Label::textColourId, t == 0 ? theme::dim : theme::accent);
}

void PlayerBar::showParts()
{
    const auto s = player.sequence();
    if (s == nullptr)
        return;
    auto list = std::make_unique<PartList>(player, [this] { fileChanged(false); });
    juce::CallOutBox::launchAsynchronously(std::move(list), partsButton.getScreenBounds(), nullptr);
}

void PlayerBar::setDropHighlight(bool on)
{
    if (on != dropHighlight) {
        dropHighlight = on;
        repaint();
    }
}

void PlayerBar::fileChanged(bool newFile)
{
    const auto s = player.sequence();
    const bool loaded = s != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &position, &partsButton, &octaveDown,
                                                             &semitoneDown, &semitoneUp, &octaveUp })
        c->setEnabled(loaded);
    position.setAlpha(loaded ? 1.0f : 0.35f);
    showTranspose();
    if (loaded) {
        position.setRange(0.0, juce::jmax(0.001, s->length), 0.0);
        int playing = 0;
        for (int i = 0; i < (int) s->parts.size(); ++i)
            playing += player.partEnabled(i) ? 1 : 0;
        juce::String details = juce::String(s->notes) + " notes, " + juce::String(s->tracks) + (s->tracks == 1 ? " track" : " tracks");
        if (s->parts.size() > 1)
            details << ", " << playing << " of " << (int) s->parts.size() << " parts played";
        partsButton.setButtonText(s->parts.size() > 1 ? "Tracks " + juce::String(playing) + "/" + juce::String((int) s->parts.size()) + "..."
                                                      : juce::String("Tracks..."));
        nameLabel.setText(juce::String::fromUTF8(s->name.c_str()) + "   (" + details + ")", juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::text);
        nameLabel.setTooltip(juce::String::fromUTF8(s->path.c_str()));
    } else {
        position.setRange(0.0, 1.0, 0.0);
        nameLabel.setText("No MIDI file: Open, or drop a .mid file on the window", juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::dim);
        nameLabel.setTooltip({});
        partsButton.setButtonText("Tracks...");
    }
    if (newFile)
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
        timeLabel.setText(timeText(player.position(), s->length, player.getSpeed()), juce::dontSendNotification);
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
    g.drawText("MIDI FILE", getLocalBounds().reduced(12, 6).removeFromTop(28).removeFromLeft(70), juce::Justification::centredLeft);
}

void PlayerBar::resized()
{
    auto all = getLocalBounds().reduced(12, 6);
    auto area = all.removeFromTop(28); // first row: the file, its parts, the transposition
    area.removeFromLeft(70); // the title
    openButton.setBounds(area.removeFromLeft(104).reduced(0, 1));
    area.removeFromLeft(10);
    playButton.setBounds(area.removeFromLeft(40).reduced(0, 1));
    area.removeFromLeft(4);
    stopButton.setBounds(area.removeFromLeft(40).reduced(0, 1));
    area.removeFromLeft(12);
    octaveUp.setBounds(area.removeFromRight(46).reduced(0, 1));
    semitoneUp.setBounds(area.removeFromRight(30).reduced(0, 1));
    transposeValue.setBounds(area.removeFromRight(96));
    semitoneDown.setBounds(area.removeFromRight(30).reduced(0, 1));
    octaveDown.setBounds(area.removeFromRight(46).reduced(0, 1));
    transposeLabel.setBounds(area.removeFromRight(76));
    area.removeFromRight(12);
    partsButton.setBounds(area.removeFromRight(110).reduced(0, 1));
    area.removeFromRight(12);
    nameLabel.setBounds(area);
    all.removeFromTop(6);
    area = all.removeFromTop(26); // second row: position, time, speed, loop
    area.removeFromLeft(70);
    loopButton.setBounds(area.removeFromRight(64));
    area.removeFromRight(8);
    speed.setBounds(area.removeFromRight(170));
    speedLabel.setBounds(area.removeFromRight(50));
    area.removeFromRight(12);
    timeLabel.setBounds(area.removeFromRight(235)); // the time at the speed, and the file's
    position.setBounds(area);
}

} // namespace pf::app
