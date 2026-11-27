// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"

#include "../../app/src/RoomPresets.h"

#include <BinaryData.h>

namespace pf::plugin {

/// The velocity curve: what the keyboard sends horizontally, what the engine receives vertically; its five points
/// dragged vertically; the last note-on as a dot.
class CurveGraph final : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit CurveGraph(Processor& p) : processor(p) {}

    void paint(juce::Graphics& g) override
    {
        const auto area = plot();
        g.setColour(theme::background);
        g.fillRoundedRectangle(area.expanded(4.0f), 4.0f);
        g.setColour(theme::panelEdge);
        for (int i = 1; i < 4; ++i) {
            g.drawHorizontalLine(juce::roundToInt(area.getY() + area.getHeight() * i / 4.0f), area.getX(), area.getRight());
            g.drawVerticalLine(juce::roundToInt(area.getX() + area.getWidth() * i / 4.0f), area.getY(), area.getBottom());
        }
        g.setColour(theme::dim.withAlpha(0.5f));
        g.drawLine(area.getX(), area.getBottom(), area.getRight(), area.getY(), 1.0f); // linear
        const auto s = processor.curveSettings();
        juce::Path curve;
        for (int v = 1; v <= 127; ++v) {
            const auto point = toScreen(v, pf::VelocityCurve::output(s, v));
            if (v == 1)
                curve.startNewSubPath(point);
            else
                curve.lineTo(point);
        }
        g.setColour(theme::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));
        for (int i = 0; i < pf::VelocityCurve::kPoints; ++i) {
            const auto p = pointPosition(s, i);
            g.setColour(i == dragged ? juce::Colours::white : theme::accent);
            g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(p));
        }
        if (const int in = processor.lastVelocityIn.load(); in > 0) {
            g.setColour(juce::Colours::white);
            g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(toScreen(in, processor.lastVelocityOut.load())));
        }
        g.setColour(theme::dim);
        g.setFont(theme::font(10.5f));
        g.drawText("keyboard", area.withTrimmedTop(area.getHeight() - 14.0f), juce::Justification::bottomRight);
        g.drawText("engine", area.withHeight(14.0f).translated(4.0f, 0.0f), juce::Justification::topLeft);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto s = processor.curveSettings();
        dragged = -1;
        float best = 12.0f;
        for (int i = 0; i < pf::VelocityCurve::kPoints; ++i)
            if (const float d = pointPosition(s, i).getDistanceFrom(e.position); d < best) {
                best = d;
                dragged = i;
            }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragged < 0)
            return;
        auto s = processor.curveSettings();
        const auto area = plot();
        s.points[(size_t) dragged] = juce::jlimit(0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight());
        processor.setCurve(s);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragged = -1;
        repaint();
    }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced(8.0f); }

    juce::Point<float> toScreen(int input, int output) const
    {
        const auto area = plot();
        return { area.getX() + area.getWidth() * (float) (input - 1) / 126.0f,
                 area.getBottom() - area.getHeight() * (float) (output - 1) / 126.0f };
    }

    juce::Point<float> pointPosition(const pf::VelocityCurve::Settings& s, int i) const
    {
        const auto area = plot();
        const float input = (float) s.inputMin + (float) (s.inputMax - s.inputMin) * (float) i / (pf::VelocityCurve::kPoints - 1);
        return { area.getX() + area.getWidth() * (input - 1.0f) / 126.0f, area.getBottom() - area.getHeight() * s.points[(size_t) i] };
    }

    Processor& processor;
    int dragged = -1;
};

namespace {

constexpr int kCustomId = 100;

const char* const kOptionIds[] = { param::body, param::residual, param::sympathetic, param::heldKeys, param::pedalCatch, param::releaseNoise };
const char* const kOptionTips[] = {
    "The resonances of the soundboard and the case, shared by every note",
    "The hammer's knock and the first instants of the strike, as recorded",
    "With the pedal down, the free strings ring with what is played",
    "The strings of keys held down ring too, without the pedal (a piano does; the reference does not)",
    "Pressing the pedal again keeps what is left of released notes (a piano does; the reference does not)",
    "The knock of the dampers landing, on the soundboard",
};

juce::String withThousands(double v)
{
    const auto n = (juce::int64) std::llround(v);
    juce::String digits(n), out;
    for (int i = 0; i < digits.length(); ++i) {
        if (i > 0 && (digits.length() - i) % 3 == 0)
            out << " ";
        out << digits[i];
    }
    return out;
}

} // namespace

Editor::Editor(Processor& p) : juce::AudioProcessorEditor(p), processor(p)
{
    setLookAndFeel(&lookAndFeel);
    logo = juce::Drawable::createFromSVGString(juce::String::fromUTF8(BinaryData::logo_svg, BinaryData::logo_svgSize));

    title.setText("PianoForge", juce::dontSendNotification);
    title.setFont(theme::font(20.0f, true));
    profileLine.setFont(theme::font(12.5f));
    chooseProfile.setTooltip("A modal-profile.bin (written by forge export) for this project; it is kept in the project, not copied into it");
    chooseProfile.onClick = [this] {
        fileChooser = std::make_unique<juce::FileChooser>("Choose a modal profile", juce::File(processor.profileStatus().path), "*.bin");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& fc) {
                                     if (const auto file = fc.getResult(); file.existsAsFile())
                                         processor.chooseProfile(file);
                                 });
    };
    applicationProfile.setTooltip("The profile of the PianoForge application's corpus (where its settings say)");
    applicationProfile.onClick = [this] { processor.chooseProfile({}); };

    setupRow(volume, param::volume, "Volume", "The engine's output, as the application's volume");
    setupRow(sympatheticLevel, param::sympatheticLevel, "Sympathetic", "The sympathetic resonance against what was measured on the reference");
    setupRow(unison, param::unison, "Unison", "How far apart the strings of each note are tuned: 100 % as measured, 0 % no beating, "
                                              "200 to 300 % an old upright's");
    setupRow(absorption, param::absorption, "Re-strike", "A note struck again strikes its string while it rings, and the strikes "
                                                         "add up; the hammer absorbs this share of the vibration (0 %: they add exactly)");
    for (size_t i = 0; i < options.size(); ++i)
        setupToggle(options[i], kOptionIds[i], kOptionTips[i]);
    setupToggle(limiterOn, param::limiter, "Nothing over -1 dBFS: a big chord with the room could otherwise clip");

    setupToggle(roomOn, param::room, "The room around the piano, heard from the listening position");
    roomPresetLabel.setText("Preset", juce::dontSendNotification);
    for (int i = 0; i < (int) app::kRoomPresets.size(); ++i)
        roomPreset.addItem(app::kRoomPresets[(size_t) i].name, i + 1);
    roomPreset.addItem("Custom", kCustomId);
    roomPreset.onChange = [this] { chooseRoomPreset(); };
    setupRow(roomSize, param::roomSize, "Volume", "The room's volume (a shoebox), 60 to 40 000 cubic metres");
    setupRow(roomDecay, param::roomDecay, "Decay", "Reverberation time (T60) in the lows");
    setupRow(roomBrightness, param::roomBrightness, "Brightness", "100 %: the highs decay as long as the lows");
    setupRow(roomWidth, param::roomWidth, "Width", "The reverberation's width");
    setupRow(roomLevel, param::roomLevel, "Level", "The room's reflections against what the physics gives (0 dB: as computed)");
    positionLabel.setText("Position", juce::dontSendNotification);
    for (int i = 0; i < (int) app::kListeningPositions.size(); ++i)
        position.addItem(juce::String(app::kListeningPositions[(size_t) i].name) + " ("
                             + juce::String(juce::roundToInt(app::kListeningPositions[(size_t) i].distance)) + " m)",
                         i + 1);
    position.addItem("Custom", kCustomId);
    position.onChange = [this] { choosePosition(); };
    setupRow(distance, param::distance, "Distance", "From the piano: 1 m is the engine's sound, heard from the keyboard");
    setupToggle(keepLoudness, param::keepLoudness, "Moving away changes the balance between the piano and the room, not the volume");
    setupChoice(heardOn, param::heardOn, "Heard on", "Headphones: the piano in front of you rather than inside your head (a spherical "
                                                     "head); room on or off");
    roomSummary.setFont(theme::font(11.5f));
    roomSummary.setColour(juce::Label::textColourId, theme::dim);
    roomSummary.setJustificationType(juce::Justification::topLeft);

    setupRow(a4, param::a4, "A4", "The reference pitch");

    // The character.
    const juce::String voicingTip = "The hammers of this register: below 0 dB needled (softer, rounder), above hardened (brighter); "
                                    "the partials above the corner move, the fundamental stays (linear between A0, C4 and C8)";
    setupRow(voicingBass, param::voicingBass, "Bass", voicingTip);
    setupRow(voicingMiddle, param::voicingMiddle, "Middle", voicingTip);
    setupRow(voicingTreble, param::voicingTreble, "Treble", voicingTip);
    setupRow(voicingFollow, param::voicingFollow, "Follows", "How much of the voicing follows the velocity: 0 % alike at every "
                                                            "velocity, 100 % in proportion (a needled felt changes the forte most)");
    setupRow(voicingCorner, param::voicingCorner, "From", "Where the voicing sets in, in partials of the note: lower reaches more of the tone");
    setupRow(dynamicRange, param::dynamicRange, "Dynamics", "Each strike's distance below the note's loudest: 100 % as measured, 50 % "
                                                             "the pianissimo half as far below the fortissimo");
    setupRow(sustainLength, param::sustainLength, "Sustain", "The strings' decay times: 1 x as measured");
    setupRow(damperSpeed, param::damperSpeed, "Dampers", "How fast the dampers stop the strings: 0.5 x a slower, longer release");
    setupRow(attackSoft, param::attackSoft, "Attack", "The strike's brightness above 1.2 kHz at the attack, fading out: below 0 dB "
                                                      "a softer attack, above a harder one");
    setupRow(attackLength, param::attackLength, "Attack time", "Over how long the attack's softness fades out");
    setupRow(lid, param::lid, "Lid", "Open as measured; on the half stick or closed, the treble (above 1.5 kHz) down to 9 dB and the "
                                     "whole by up to 2 dB");
    setupRow(toneBass, param::toneBass, "Bass", "A low shelf at 120 Hz: the bass of the whole output");
    setupRow(tonePresence, param::tonePresence, "Presence", "A broad bell at 2.5 kHz: where a piano can sound hard (down) or recessed (up)");
    setupRow(toneAir, param::toneAir, "Air", "A high shelf at 10 kHz: the air above the piano's tone");
    presetLabel.setText("Preset", juce::dontSendNotification);
    presetLabel.setColour(juce::Label::textColourId, theme::dim);
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        preset.addItem(processor.getProgramName(i), i + 1);
    preset.setTooltip("The application's factory presets: the sound only (the engine's models, the voicing, the touch, the tone, "
                      "the room and the position); the tuning, the volume, the limiter and the curve stay. Also the host's programs.");
    preset.onChange = [this] {
        if (const int id = preset.getSelectedId(); id >= 1)
            processor.applyProgram(id - 1);
    };
    setupChoice(temperament, param::temperament, "Temperament", "Historical temperaments; A keeps its frequency");
    setupChoice(root, param::root, "Root", "The temperament's root");

    curveGraph = std::make_unique<CurveGraph>(processor);
    curveGraph->setTooltip("Drag the points. Horizontally what the keyboard sends, vertically what the engine receives; "
                           "the dot is the last note played");
    for (auto* s : { &softest, &hardest }) {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
        s->setRange(1.0, 127.0, 1.0);
        s->onValueChange = [this] {
            auto c = processor.curveSettings();
            c.inputMin = juce::jlimit(1, 126, (int) softest.getValue());
            c.inputMax = juce::jlimit(c.inputMin + 1, 127, (int) hardest.getValue());
            processor.setCurve(c);
            curveGraph->repaint();
        };
    }
    softestLabel.setText("Softest", juce::dontSendNotification);
    hardestLabel.setText("Hardest", juce::dontSendNotification);
    softest.setTooltip("The softest velocity the keyboard sends");
    hardest.setTooltip("The hardest velocity the keyboard sends (a Korg B2: about 114)");
    linearCurve.onClick = [this] {
        processor.setCurve({});
        refreshCurveControls();
    };
    appCurve.setTooltip("The curve made in the PianoForge application (Keyboard > Velocity curve)");
    appCurve.onClick = [this] {
        pf::VelocityCurve::Settings c;
        if (Processor::applicationCurve(c)) {
            processor.setCurve(c);
            refreshCurveControls();
        } else {
            appCurve.setButtonText("None in the application");
        }
    };
    refreshCurveControls();

    footer.setFont(theme::font(11.5f));
    footer.setColour(juce::Label::textColourId, theme::dim);

    for (auto* c : std::initializer_list<juce::Component*> { &title, &profileLine, &chooseProfile, &applicationProfile, &presetLabel, &preset,
                                                             &roomPresetLabel,
                                                             &roomPreset, &positionLabel, &position, &roomSummary, curveGraph.get(),
                                                             &softest, &hardest, &softestLabel, &hardestLabel, &linearCurve, &appCurve, &footer })
        addAndMakeVisible(*c);

    refreshPresets();
    setSize(1320, 670);
    timerCallback();
    startTimerHz(15);
}

Editor::~Editor()
{
    setLookAndFeel(nullptr);
}

void Editor::setupRow(Row& row, const char* id, const juce::String& name, const juce::String& tooltip)
{
    row.label.setText(name, juce::dontSendNotification);
    row.slider.setSliderStyle(juce::Slider::LinearHorizontal);
    row.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 20);
    row.slider.setTooltip(tooltip);
    row.attachment = std::make_unique<SliderAttachment>(processor.parameters, id, row.slider);
    addAndMakeVisible(row.label);
    addAndMakeVisible(row.slider);
}

void Editor::setupToggle(Toggle& toggle, const char* id, const juce::String& tooltip)
{
    toggle.button.setButtonText(processor.parameters.getParameter(id)->getName(64));
    toggle.button.setTooltip(tooltip);
    toggle.attachment = std::make_unique<ButtonAttachment>(processor.parameters, id, toggle.button);
    addAndMakeVisible(toggle.button);
}

void Editor::setupChoice(Choice& choice, const char* id, const juce::String& name, const juce::String& tooltip)
{
    choice.label.setText(name, juce::dontSendNotification);
    choice.box.addItemList(dynamic_cast<juce::AudioParameterChoice*>(processor.parameters.getParameter(id))->choices, 1);
    choice.box.setTooltip(tooltip);
    choice.attachment = std::make_unique<ComboAttachment>(processor.parameters, id, choice.box);
    addAndMakeVisible(choice.label);
    addAndMakeVisible(choice.box);
}

void Editor::setParameter(const char* id, float value)
{
    auto* p = processor.parameters.getParameter(id);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(value));
    p->endChangeGesture();
}

float Editor::parameter(const char* id) const
{
    return processor.parameters.getRawParameterValue(id)->load();
}

pf::Room::Settings Editor::currentRoom() const
{
    pf::Room::Settings s;
    s.enabled = parameter(param::room) >= 0.5f;
    s.size = parameter(param::roomSize);
    s.decaySeconds = parameter(param::roomDecay);
    s.brightness = parameter(param::roomBrightness);
    s.width = parameter(param::roomWidth);
    s.levelDb = parameter(param::roomLevel);
    s.distance = parameter(param::distance);
    s.keepLoudness = parameter(param::keepLoudness) >= 0.5f;
    s.headphones = juce::roundToInt(parameter(param::heardOn)) == 1;
    return s;
}

void Editor::chooseRoomPreset()
{
    const int id = roomPreset.getSelectedId();
    if (id < 1 || id > (int) app::kRoomPresets.size())
        return;
    const auto& p = app::kRoomPresets[(size_t) (id - 1)];
    setParameter(param::roomSize, p.size);
    setParameter(param::roomDecay, p.decay);
    setParameter(param::roomBrightness, p.brightness);
    setParameter(param::roomWidth, p.width);
    processor.roomPresetName = p.name;
}

void Editor::choosePosition()
{
    const int id = position.getSelectedId();
    if (id < 1 || id > (int) app::kListeningPositions.size())
        return;
    setParameter(param::distance, app::kListeningPositions[(size_t) (id - 1)].distance);
    processor.positionName = app::kListeningPositions[(size_t) (id - 1)].name;
}

void Editor::refreshPresets()
{
    // The preset whose values the room has, else Custom; the same for the position.
    const auto close = [](float a, float b) { return std::abs(a - b) < 0.006f; };
    int presetId = kCustomId;
    for (int i = 0; i < (int) app::kRoomPresets.size(); ++i) {
        const auto& p = app::kRoomPresets[(size_t) i];
        if (close(parameter(param::roomSize), p.size) && close(parameter(param::roomDecay), p.decay)
            && close(parameter(param::roomBrightness), p.brightness) && close(parameter(param::roomWidth), p.width))
            presetId = i + 1;
    }
    if (roomPreset.getSelectedId() != presetId)
        roomPreset.setSelectedId(presetId, juce::dontSendNotification);
    processor.roomPresetName = presetId == kCustomId ? juce::String("Custom") : juce::String(app::kRoomPresets[(size_t) (presetId - 1)].name);
    int positionId = kCustomId;
    for (int i = 0; i < (int) app::kListeningPositions.size(); ++i)
        if (std::abs(parameter(param::distance) - app::kListeningPositions[(size_t) i].distance) < 0.05f)
            positionId = i + 1;
    if (position.getSelectedId() != positionId)
        position.setSelectedId(positionId, juce::dontSendNotification);
    processor.positionName = positionId == kCustomId ? juce::String("Custom")
                                                     : juce::String(app::kListeningPositions[(size_t) (positionId - 1)].name);
}

void Editor::refreshCurveControls()
{
    const auto c = processor.curveSettings();
    softest.setValue(c.inputMin, juce::dontSendNotification);
    hardest.setValue(c.inputMax, juce::dontSendNotification);
    curveGraph->repaint();
}

void Editor::timerCallback()
{
    const auto status = processor.profileStatus();
    juce::String line = status.loaded ? "Profile: " + status.text + (status.chosen ? " (chosen for this project)" : " (the application's corpus)")
                                      : status.text;
    profileLine.setText(line, juce::dontSendNotification);
    profileLine.setColour(juce::Label::textColourId, status.loaded ? theme::good
                                                     : status.text.startsWith("Loading") ? theme::busy : theme::bad);
    profileLine.setTooltip(status.path);
    applicationProfile.setEnabled(status.chosen);
    if (preset.getSelectedId() != processor.getCurrentProgram() + 1)
        preset.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);

    refreshPresets();
    const auto s = currentRoom();
    const auto a = pf::Room::acoustics(s);
    juce::String text;
    if (s.enabled) {
        text << withThousands(a.volume) << juce::String::fromUTF8(" m\xc2\xb3") << ", T60 " << juce::String(s.decaySeconds, 1)
             << " s; at " << juce::String(a.distance, 1) << " m the room " << (a.reflectionsOverDirectDb >= 0.0 ? "+" : "")
             << juce::String(a.reflectionsOverDirectDb, 1) << " dB against the direct sound, first reflection after "
             << juce::String(juce::roundToInt(a.firstReflectionMs)) << " ms.";
    } else {
        text << "Room off: the engine's sound, heard from the keyboard.";
    }
    if (s.headphones) {
        auto ears = s;
        if (!s.enabled)
            ears.distance = pf::Room::kReferenceDistance;
        const auto e = pf::Room::acoustics(ears);
        text << " On headphones: each side of the piano " << juce::String(juce::roundToInt(e.sideAngle)) << juce::String::fromUTF8("\xc2\xb0")
             << " from the front, " << juce::String(e.interauralMs, 2) << " ms between the ears.";
    }
    roomSummary.setText(text, juce::dontSendNotification);
    for (auto* r : { &roomSize, &roomDecay, &roomBrightness, &roomWidth, &roomLevel })
        r->slider.setEnabled(s.enabled);
    roomPreset.setEnabled(s.enabled);
    sympatheticLevel.slider.setEnabled(parameter(param::sympathetic) >= 0.5f);

    const float peak = processor.takePeak();
    const float db = peak > 1e-6f ? juce::Decibels::gainToDecibels(peak) : -100.0f;
    meter = std::max(db, meter - 2.0f); // 30 dB per second down
    const int k = processor.rateFactor();
    const double engine = processor.engineRate();
    juce::String f;
    if (engine > 0.0) {
        f << "Engine at " << juce::String(engine / 1000.0, 1) << " kHz";
        if (k > 1)
            f << " (the host's " << juce::String(engine * k / 1000.0, 1) << " kHz divided by " << k << ", latency "
              << processor.getLatencySamples() << " samples)";
    } else {
        f << "Not playing yet";
    }
    f << "    Voices: " << processor.activeVoices() << "    MIDI: CC64 sustain (continuous), CC66 sostenuto, CC67 soft pedal";
    footer.setText(f, juce::dontSendNotification);
    curveGraph->repaint();
    repaint(meterArea);
}

void Editor::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    g.setColour(theme::panel);
    g.fillRect(header);
    if (logo != nullptr)
        logo->drawWithin(g, header.withWidth(header.getHeight()).reduced(10).toFloat(), juce::RectanglePlacement::centred, 1.0f);
    theme::drawCard(g, soundCard, "Sound");
    theme::drawCard(g, outputCard, "Output");
    theme::drawCard(g, roomCard, "Room around the piano");
    theme::drawCard(g, positionCard, "Listening position");
    theme::drawCard(g, tuningCard, "Tuning");
    theme::drawCard(g, curveCard, "Velocity curve");
    theme::drawCard(g, voicingCard, "Voicing: the hammers");
    theme::drawCard(g, touchCard, "Touch and lid");
    theme::drawCard(g, toneCard, "Output tone");
    // The output's peak (the engine's, before the room): -60 to 0 dBFS.
    auto bar = meterArea.toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(bar, 3.0f);
    const float level = juce::jlimit(0.0f, 1.0f, (meter + 60.0f) / 60.0f);
    g.setColour(meter > -1.0f ? theme::bad : meter > -6.0f ? theme::busy : theme::good);
    g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * level), 3.0f);
}

void Editor::layoutRow(Row& row, juce::Rectangle<int>& area, int labelWidth)
{
    auto r = area.removeFromTop(30);
    row.label.setBounds(r.removeFromLeft(labelWidth));
    row.slider.setBounds(r.reduced(0, 3));
}

void Editor::resized()
{
    auto area = getLocalBounds();
    header = area.removeFromTop(60);
    {
        auto h = header.reduced(12, 8);
        h.removeFromLeft(header.getHeight() - 8);
        auto buttons = h.removeFromRight(330 + 330);
        applicationProfile.setBounds(buttons.removeFromRight(160).withSizeKeepingCentre(160, 28));
        buttons.removeFromRight(8);
        chooseProfile.setBounds(buttons.removeFromRight(150).withSizeKeepingCentre(150, 28));
        buttons.removeFromRight(24);
        preset.setBounds(buttons.removeFromRight(250).withSizeKeepingCentre(250, 28));
        presetLabel.setBounds(buttons.removeFromRight(54).withSizeKeepingCentre(54, 28));
        title.setBounds(h.removeFromTop(24));
        profileLine.setBounds(h);
    }
    footer.setBounds(area.removeFromBottom(26).reduced(12, 0));
    area.reduce(12, 10);
    const int gap = 12;
    const int w1 = 300, w2 = 340, w4 = 330;
    auto col4 = area.removeFromRight(w4);
    area.removeFromRight(gap);
    auto col1 = area.removeFromLeft(w1);
    area.removeFromLeft(gap);
    auto col2 = area.removeFromLeft(w2);
    area.removeFromLeft(gap);
    auto col3 = area;

    // The character: the voicing, the touch and the lid, the output's tone.
    const auto card = [](int rows) { return 16 + 22 + rows * 30; };
    voicingCard = col4.removeFromTop(card(5));
    col4.removeFromTop(gap);
    touchCard = col4.removeFromTop(card(6));
    col4.removeFromTop(gap);
    toneCard = col4.removeFromTop(card(3));
    {
        auto in = voicingCard.reduced(12, 8);
        in.removeFromTop(22);
        for (auto* r : { &voicingBass, &voicingMiddle, &voicingTreble, &voicingFollow, &voicingCorner })
            layoutRow(*r, in, 90);
        in = touchCard.reduced(12, 8);
        in.removeFromTop(22);
        for (auto* r : { &dynamicRange, &sustainLength, &damperSpeed, &attackSoft, &attackLength, &lid })
            layoutRow(*r, in, 90);
        in = toneCard.reduced(12, 8);
        in.removeFromTop(22);
        for (auto* r : { &toneBass, &tonePresence, &toneAir })
            layoutRow(*r, in, 90);
    }

    // Sound and output.
    outputCard = col1.removeFromBottom(84);
    col1.removeFromBottom(gap);
    soundCard = col1;
    {
        auto in = soundCard.reduced(12, 8);
        in.removeFromTop(22);
        for (auto* r : { &volume, &sympatheticLevel, &unison, &absorption })
            layoutRow(*r, in);
        in.removeFromTop(6);
        for (auto& t : options)
            t.button.setBounds(in.removeFromTop(26));
    }
    {
        auto in = outputCard.reduced(12, 8);
        in.removeFromTop(22);
        limiterOn.button.setBounds(in.removeFromTop(26));
        meterArea = in.removeFromTop(10).withTrimmedTop(2);
    }

    // The room and the listening position.
    roomCard = col2.removeFromTop(236);
    col2.removeFromTop(gap);
    positionCard = col2;
    {
        auto in = roomCard.reduced(12, 8);
        in.removeFromTop(22);
        auto r = in.removeFromTop(30);
        roomOn.button.setBounds(r.removeFromLeft(84));
        roomPresetLabel.setBounds(r.removeFromLeft(54));
        roomPreset.setBounds(r.reduced(0, 3));
        in.removeFromTop(4);
        for (auto* row : { &roomSize, &roomDecay, &roomBrightness, &roomWidth, &roomLevel })
            layoutRow(*row, in);
    }
    {
        auto in = positionCard.reduced(12, 8);
        in.removeFromTop(22);
        auto r = in.removeFromTop(30);
        positionLabel.setBounds(r.removeFromLeft(84));
        position.setBounds(r.reduced(0, 3));
        layoutRow(distance, in);
        r = in.removeFromTop(30);
        heardOn.label.setBounds(r.removeFromLeft(84));
        heardOn.box.setBounds(r.reduced(0, 3));
        keepLoudness.button.setBounds(in.removeFromTop(26));
        in.removeFromTop(4);
        roomSummary.setBounds(in);
    }

    // Tuning and the velocity curve.
    tuningCard = col3.removeFromTop(150);
    col3.removeFromTop(gap);
    curveCard = col3;
    {
        auto in = tuningCard.reduced(12, 8);
        in.removeFromTop(22);
        layoutRow(a4, in);
        for (auto* c : { &temperament, &root }) {
            auto r = in.removeFromTop(30);
            c->label.setBounds(r.removeFromLeft(84));
            c->box.setBounds(r.reduced(0, 3));
        }
    }
    {
        auto in = curveCard.reduced(12, 8);
        in.removeFromTop(22);
        auto buttons = in.removeFromBottom(28);
        linearCurve.setBounds(buttons.removeFromLeft(80));
        buttons.removeFromLeft(8);
        appCurve.setBounds(buttons);
        in.removeFromBottom(6);
        auto r = in.removeFromBottom(28);
        hardestLabel.setBounds(r.removeFromLeft(60));
        hardest.setBounds(r.reduced(0, 3));
        r = in.removeFromBottom(28);
        softestLabel.setBounds(r.removeFromLeft(60));
        softest.setBounds(r.reduced(0, 3));
        in.removeFromBottom(4);
        curveGraph->setBounds(in);
    }
}

} // namespace pf::plugin
