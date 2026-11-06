#include "RoomPanel.h"

#include "Theme.h"

#include <cmath>

namespace pf::app {

namespace {

constexpr int kCustomId = 100;
constexpr int kDefaultPreset = 2; // Concert hall

struct ControlInfo {
    const char* key;
    const char* name;
    double min, max, step, mid; // mid: the skew's middle (0: linear)
    const char* suffix;
    double scale; // shown value = stored value x scale (fractions as percentages)
};

constexpr ControlInfo kControls[] = {
    { "roomLevel", "Level", -40.0, 0.0, 0.5, 0.0, " dB", 1.0 },
    { "roomSize", "Size", 0.0, 100.0, 1.0, 0.0, " %", 100.0 },
    { "roomDecay", "Decay", 0.3, 8.0, 0.05, 2.0, " s", 1.0 },
    { "roomBrightness", "Brightness", 0.0, 100.0, 1.0, 0.0, " %", 100.0 },
    { "roomPredelay", "Pre-delay", 0.0, 100.0, 1.0, 0.0, " ms", 1.0 },
    { "roomWidth", "Width", 0.0, 100.0, 1.0, 0.0, " %", 100.0 },
};

} // namespace

const std::array<RoomPanel::Preset, 5>& RoomPanel::presets()
{
    // name, size, decay s, brightness, pre-delay ms, level dB, width
    static const std::array<Preset, 5> list { {
        { "Studio", 0.15f, 0.8f, 0.60f, 4.0f, -18.0f, 0.8f },
        { "Chamber", 0.35f, 1.4f, 0.55f, 10.0f, -16.0f, 1.0f },
        { "Concert hall", 0.65f, 2.2f, 0.50f, 20.0f, -15.0f, 1.0f },
        { "Large hall", 0.85f, 3.0f, 0.45f, 28.0f, -15.0f, 1.0f },
        { "Cathedral", 1.0f, 5.5f, 0.35f, 45.0f, -16.0f, 1.0f },
    } };
    return list;
}

RoomPanel::RoomPanel(pf::Room& r, juce::PropertiesFile& s) : room(r), settings(s)
{
    loading = true;
    onButton.setTooltip("The room around the piano, whichever engine plays (also in the toolbar)");
    onButton.setToggleState(settings.getBoolValue("roomOn", true), juce::dontSendNotification);
    onButton.onClick = [this] {
        apply();
        save();
        if (onChange)
            onChange();
    };
    presetLabel.setText("Preset", juce::dontSendNotification);
    for (int i = 0; i < (int) presets().size(); ++i)
        preset.addItem(presets()[(size_t) i].name, i + 1);
    preset.addItem("Custom", kCustomId);
    preset.onChange = [this] {
        if (loading)
            return;
        if (const int id = preset.getSelectedId(); id >= 1 && id <= (int) presets().size())
            loadPreset(id - 1);
        save();
        if (onChange)
            onChange();
    };

    for (int c = 0; c < count; ++c) {
        const auto& info = kControls[c];
        auto& slider = sliders[(size_t) c];
        slider.setRange(info.min, info.max, info.step);
        if (info.mid > 0.0)
            slider.setSkewFactorFromMidPoint(info.mid);
        slider.setTextValueSuffix(info.suffix);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 20);
        slider.onValueChange = [this, c] {
            if (loading)
                return;
            apply();
            edited();
            if (!sliders[(size_t) c].isMouseButtonDown()) // a typed value; a drag saves at its end
                save();
        };
        slider.onDragEnd = [this] { save(); };
        labels[(size_t) c].setText(info.name, juce::dontSendNotification);
        addAndMakeVisible(slider);
        addAndMakeVisible(labels[(size_t) c]);
    }
    sliders[level].setTooltip("The reverberation against the piano's own sound");
    sliders[size].setTooltip("From a small room (4.7 x 3.4 m) to a large hall (21 x 15 m): the reflections and the density");
    sliders[decay].setTooltip("The time the reverberation takes to fall 60 dB, under 3 kHz");
    sliders[brightness].setTooltip("How long the highs last against the lows (100 %: as long; 0 %: 0.15 times)");
    sliders[predelay].setTooltip("The gap between the piano and its reverberation");
    sliders[width].setTooltip("0 %: a mono reverberation, 100 %: full width");

    note.setText("Added to the piano's sound, which goes through untouched (no latency): heard, recorded and "
                 "streamed alike, with either engine. A change of size or pre-delay fades the reverberation for 10 ms.",
                 juce::dontSendNotification);
    note.setColour(juce::Label::textColourId, theme::dim);
    note.setFont(theme::font(12.0f));
    note.setJustificationType(juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*> { &onButton, &presetLabel, &preset, &note })
        addAndMakeVisible(*c);

    // The saved settings, or the default preset.
    const int saved = settings.getIntValue("roomPreset", kDefaultPreset + 1);
    if (saved >= 1 && saved <= (int) presets().size() && !settings.containsKey("roomLevel"))
        loadPreset(saved - 1);
    else {
        const auto& p = presets()[(size_t) kDefaultPreset];
        const float defaults[count] = { p.levelDb, p.size, p.decay, p.brightness, p.predelayMs, p.width };
        for (int c = 0; c < count; ++c)
            sliders[(size_t) c].setValue(settings.getDoubleValue(kControls[c].key, defaults[c]) * kControls[c].scale,
                                         juce::dontSendNotification);
    }
    preset.setSelectedId(saved == kCustomId || (saved >= 1 && saved <= (int) presets().size()) ? saved : kDefaultPreset + 1,
                         juce::dontSendNotification);
    loading = false;
    apply();
    setSize(560, 360);
}

void RoomPanel::setOn(bool on)
{
    onButton.setToggleState(on, juce::sendNotification);
}

juce::String RoomPanel::presetName() const
{
    return preset.getText();
}

void RoomPanel::loadPreset(int index)
{
    const auto& p = presets()[(size_t) index];
    const float values[count] = { p.levelDb, p.size, p.decay, p.brightness, p.predelayMs, p.width };
    const bool was = loading;
    loading = true;
    for (int c = 0; c < count; ++c)
        sliders[(size_t) c].setValue(values[c] * kControls[c].scale, juce::dontSendNotification);
    loading = was;
    apply();
}

void RoomPanel::edited()
{
    if (preset.getSelectedId() != kCustomId) {
        preset.setSelectedId(kCustomId, juce::dontSendNotification);
        if (onChange)
            onChange();
    }
}

void RoomPanel::apply()
{
    pf::Room::Settings s;
    s.enabled = onButton.getToggleState();
    s.levelDb = (float) sliders[level].getValue();
    s.size = (float) (sliders[size].getValue() / 100.0);
    s.decaySeconds = (float) sliders[decay].getValue();
    s.brightness = (float) (sliders[brightness].getValue() / 100.0);
    s.predelayMs = (float) sliders[predelay].getValue();
    s.width = (float) (sliders[width].getValue() / 100.0);
    room.applySettings(s);
    for (int c = 0; c < count; ++c)
        sliders[(size_t) c].setEnabled(s.enabled);
    preset.setEnabled(s.enabled);
}

void RoomPanel::save()
{
    settings.setValue("roomOn", onButton.getToggleState());
    settings.setValue("roomPreset", preset.getSelectedId());
    for (int c = 0; c < count; ++c)
        settings.setValue(kControls[c].key, sliders[(size_t) c].getValue() / kControls[c].scale);
    settings.saveIfNeeded();
}

void RoomPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, card, "Room around the piano");
}

void RoomPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    card = area;
    auto inner = area.reduced(12, 8);
    inner.removeFromTop(20);
    auto row = inner.removeFromTop(28);
    onButton.setBounds(row.removeFromLeft(110));
    row.removeFromLeft(20);
    presetLabel.setBounds(row.removeFromLeft(56));
    preset.setBounds(row.removeFromLeft(200).reduced(0, 2));
    inner.removeFromTop(10);
    for (int c = 0; c < count; ++c) {
        row = inner.removeFromTop(32);
        labels[(size_t) c].setBounds(row.removeFromLeft(110));
        sliders[(size_t) c].setBounds(row.reduced(0, 2));
    }
    inner.removeFromTop(8);
    note.setBounds(inner);
}

} // namespace pf::app
