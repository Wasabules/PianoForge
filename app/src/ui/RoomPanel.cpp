#include "RoomPanel.h"

#include "Theme.h"

#include <cmath>

namespace pf::app {

namespace {

constexpr int kCustomId = 100;
constexpr int kUserBase = 200;      // the user's presets: kUserBase + their index
constexpr int kDefaultPreset = 2;   // Concert hall
constexpr int kDefaultPosition = 0; // at the keyboard
constexpr int kModel = 2;           // the settings' model: 2, the listening position (1: a level against the dry sound)

struct ControlInfo {
    const char* key;
    const char* name;
    double min, max, step, mid; // mid: the skew's middle (0: linear)
    const char* suffix;
    double scale; // shown value = stored value x scale (fractions as percentages)
};

constexpr ControlInfo kControls[] = {
    { "roomLevel", "Level", -20.0, 20.0, 0.5, 0.0, " dB", 1.0 },
    { "roomSize", "Volume", 0.0, 100.0, 0.5, 0.0, "", 100.0 },
    { "roomDecay", "Decay", 0.3, 8.0, 0.05, 2.0, " s", 1.0 },
    { "roomBrightness", "Brightness", 0.0, 100.0, 1.0, 0.0, " %", 100.0 },
    { "roomWidth", "Width", 0.0, 100.0, 1.0, 0.0, " %", 100.0 },
    { "roomDistance", "Distance", 1.0, 30.0, 0.1, 6.0, " m", 1.0 },
};

pf::Room::Settings presetSettings(const RoomPanel::Preset& p)
{
    pf::Room::Settings s;
    s.size = p.size;
    s.decaySeconds = p.decay;
    s.brightness = p.brightness;
    s.width = p.width;
    return s;
}

juce::String cubicMetres()
{
    return juce::String::fromUTF8(" m\xc2\xb3"); // m, superscript 3
}

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

const std::array<RoomPanel::Preset, 5>& RoomPanel::presets()
{
    // name, size (60 m3 at 0, 40 000 m3 at 1, geometric), decay s, brightness, width
    static const std::array<Preset, 5> list { {
        { "Studio", 0.14f, 0.6f, 0.60f, 0.8f },        // 150 m3
        { "Chamber", 0.50f, 1.4f, 0.55f, 1.0f },       // 1 600 m3
        { "Concert hall", 0.85f, 2.2f, 0.50f, 1.0f },  // 15 000 m3
        { "Large hall", 0.93f, 3.0f, 0.45f, 1.0f },    // 25 000 m3
        { "Cathedral", 1.0f, 5.5f, 0.35f, 1.0f },      // 40 000 m3
    } };
    return list;
}

const std::array<RoomPanel::Position, 5>& RoomPanel::positions()
{
    static const std::array<Position, 5> list { {
        { "At the keyboard", 1.0f },
        { "On stage", 3.0f },
        { "Front row", 6.0f },
        { "Middle of the hall", 12.0f },
        { "Back of the hall", 20.0f },
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
    keepLoudness.setTooltip("Moving away changes the balance between the piano and the room, not the volume");
    keepLoudness.onClick = [this] {
        apply();
        if (preset.getSelectedId() >= kUserBase) // a preset of the user's holds it
            edited();
        save();
    };
    presetLabel.setText("Preset", juce::dontSendNotification);
    loadUserPresets();
    rebuildPresetList(kCustomId);
    preset.onChange = [this] {
        if (loading)
            return;
        if (const int id = preset.getSelectedId(); id >= 1 && id <= (int) presets().size())
            loadPreset(id - 1);
        else if (id >= kUserBase && id < kUserBase + (int) userPresets.size())
            loadUserPreset(id - kUserBase);
        deletePreset.setEnabled(preset.getSelectedId() >= kUserBase);
        save();
        if (onChange)
            onChange();
    };
    savePreset.setTooltip("Keep this room and this listening position under a name of your own");
    savePreset.onClick = [this] { askSavePreset(); };
    deletePreset.setTooltip("Delete the preset of your own selected");
    deletePreset.onClick = [this] { askDeletePreset(); };
    heardOnLabel.setText("Heard on", juce::dontSendNotification);
    heardOn.addItem("Speakers", kSpeakersId);
    heardOn.addItem("Headphones (binaural)", kHeadphonesId);
    heardOn.setTooltip("Headphones: the piano in front of you rather than inside your head. Each side of it reaches both ears, "
                       "the farther one later and duller (a spherical head); so do the room's reflections. Room on or off. "
                       "Ctrl+H");
    heardOn.onChange = [this] {
        if (loading)
            return;
        apply();
        save();
        if (onChange)
            onChange();
    };
    positionLabel.setText("Position", juce::dontSendNotification);
    for (int i = 0; i < (int) positions().size(); ++i)
        position.addItem(juce::String(positions()[(size_t) i].name) + " (" + juce::String(juce::roundToInt(positions()[(size_t) i].distance)) + " m)", i + 1);
    position.addItem("Custom", kCustomId);
    position.onChange = [this] {
        if (loading)
            return;
        if (const int id = position.getSelectedId(); id >= 1 && id <= (int) positions().size()) {
            const bool was = loading;
            loading = true;
            sliders[distance].setValue(positions()[(size_t) (id - 1)].distance, juce::dontSendNotification);
            loading = was;
            apply();
        }
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
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 20);
        slider.onValueChange = [this, c] {
            if (loading)
                return;
            apply();
            if (c == distance)
                positionEdited();
            else
                edited();
            if (!sliders[(size_t) c].isMouseButtonDown()) // a typed value; a drag saves at its end
                save();
        };
        slider.onDragEnd = [this] { save(); };
        labels[(size_t) c].setText(info.name, juce::dontSendNotification);
        addAndMakeVisible(slider);
        addAndMakeVisible(labels[(size_t) c]);
    }
    // The volume shown in cubic metres (the size: 60 to 40 000 m3, geometric).
    sliders[size].textFromValueFunction = [](double v) {
        pf::Room::Settings t;
        t.size = (float) (v / 100.0);
        return withThousands(pf::Room::acoustics(t).volume) + cubicMetres();
    };
    sliders[size].valueFromTextFunction = [](const juce::String& text) {
        const double m3 = juce::jlimit(60.0, 40000.0, text.retainCharacters("0123456789.").getDoubleValue());
        return 100.0 * std::log(m3 / 60.0) / std::log(40000.0 / 60.0);
    };
    sliders[level].setTooltip("The reflections against what the physics gives (0 dB: as computed)");
    sliders[size].setTooltip("The room's volume, a shoebox of proportions 10.5 : 7.5 : 4.5 (from 6 x 4 x 2.6 m to 48 x 35 x 21 m)");
    sliders[decay].setTooltip("The time the reverberation takes to fall 60 dB, under 3 kHz");
    sliders[brightness].setTooltip("How long the highs last against the lows (100 %: as long; 0 %: 0.15 times)");
    sliders[width].setTooltip("The reverberation's width: 0 %, mono; 100 %, full width");
    sliders[distance].setTooltip("From the piano to the listener (1 m: the engine's sound, heard from the keyboard)");

    summary.setColour(juce::Label::textColourId, theme::text);
    summary.setFont(theme::font(12.5f));
    summary.setJustificationType(juce::Justification::topLeft);
    note.setText("The engine's sound is the piano heard from the keyboard. Further away, the direct sound falls as 1/r, "
                 "loses its highs to the air and narrows; the room's energy follows Barron's revised theory (Sabine), "
                 "for a piano of directivity 2. Nothing delays the direct sound. On headphones, a spherical head "
                 "(Woodworth, Brown and Duda) and a tail as coherent between the ears as a diffuse field.",
                 juce::dontSendNotification);
    note.setColour(juce::Label::textColourId, theme::dim);
    note.setFont(theme::font(12.0f));
    note.setJustificationType(juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*> { &onButton, &presetLabel, &preset, &savePreset, &deletePreset,
                                                             &positionLabel, &position, &keepLoudness, &heardOnLabel, &heardOn,
                                                             &summary, &note })
        addAndMakeVisible(*c);

    reload();
    setSize(600, 576);
}

void RoomPanel::reload()
{
    loading = true;
    onButton.setToggleState(settings.getBoolValue("roomOn", true), juce::dontSendNotification);
    loadUserPresets();
    rebuildPresetList(kCustomId);
    // The saved settings, or the defaults. Settings of the earlier model (a level against the dry sound, other
    // sizes): their preset, at the keyboard.
    const bool current = settings.getIntValue("roomModel", 1) == kModel;
    int saved = settings.getIntValue("roomPreset", kDefaultPreset + 1);
    if (!current && !(saved >= 1 && saved <= (int) presets().size()))
        saved = kDefaultPreset + 1;
    int selected = saved;
    if (current && saved >= kUserBase) { // a preset of the user's, found by its name
        const auto name = settings.getValue("roomPresetName");
        selected = kCustomId;
        for (int i = 0; i < (int) userPresets.size(); ++i)
            if (userPresets[(size_t) i].name == name)
                selected = kUserBase + i;
        saved = kCustomId;
    }
    if (current && saved == kCustomId) {
        const auto d = presetSettings(presets()[(size_t) kDefaultPreset]);
        const float defaults[count - 1] = { 0.0f, d.size, d.decaySeconds, d.brightness, d.width };
        for (int c = 0; c < count - 1; ++c)
            sliders[(size_t) c].setValue(settings.getDoubleValue(kControls[c].key, defaults[c]) * kControls[c].scale,
                                         juce::dontSendNotification);
    } else {
        loadPreset(saved - 1);
    }
    preset.setSelectedId(selected, juce::dontSendNotification);
    deletePreset.setEnabled(selected >= kUserBase);
    const int savedPosition = current ? settings.getIntValue("roomPosition", kDefaultPosition + 1) : kDefaultPosition + 1;
    const double defaultDistance = savedPosition >= 1 && savedPosition <= (int) positions().size()
                                     ? positions()[(size_t) (savedPosition - 1)].distance
                                     : 1.0;
    sliders[distance].setValue(current ? settings.getDoubleValue("roomDistance", defaultDistance) : defaultDistance, juce::dontSendNotification);
    position.setSelectedId(savedPosition, juce::dontSendNotification);
    keepLoudness.setToggleState(current ? settings.getBoolValue("roomKeepLoudness", true) : true, juce::dontSendNotification);
    heardOn.setSelectedId(settings.getBoolValue("roomHeadphones", false) ? kHeadphonesId : kSpeakersId, juce::dontSendNotification);
    if (selected >= kUserBase) // a preset of the user's: its own room and position
        loadUserPreset(selected - kUserBase);
    loading = false;
    apply();
    describe();
    if (!current)
        save();
}

void RoomPanel::setOn(bool on)
{
    onButton.setToggleState(on, juce::sendNotification);
}

void RoomPanel::setHeadphones(bool headphones)
{
    heardOn.setSelectedId(headphones ? kHeadphonesId : kSpeakersId, juce::sendNotificationSync);
}

juce::String RoomPanel::presetName() const
{
    const double d = sliders[distance].getValue();
    return preset.getText() + ", " + (d < 10.0 ? juce::String(d, 1) : juce::String(juce::roundToInt(d))) + " m";
}

void RoomPanel::loadPreset(int index)
{
    const auto& p = presets()[(size_t) index];
    const float values[count - 1] = { 0.0f, p.size, p.decay, p.brightness, p.width };
    const bool was = loading;
    loading = true;
    for (int c = 0; c < count - 1; ++c)
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

void RoomPanel::positionEdited()
{
    const int id = position.getSelectedId();
    if (id >= 1 && id <= (int) positions().size() && std::abs(positions()[(size_t) (id - 1)].distance - sliders[distance].getValue()) < 0.05)
        return;
    if (id != kCustomId)
        position.setSelectedId(kCustomId, juce::dontSendNotification);
    if (preset.getSelectedId() >= kUserBase) // a preset of the user's holds the position too
        edited();
    if (onChange)
        onChange();
}

juce::StringArray RoomPanel::presetNames() const
{
    juce::StringArray names;
    for (const auto& p : presets())
        names.add(p.name);
    for (const auto& p : userPresets)
        names.add(p.name);
    return names;
}

int RoomPanel::presetIndex() const
{
    const int id = preset.getSelectedId();
    if (id >= 1 && id <= (int) presets().size())
        return id - 1;
    if (id >= kUserBase && id < kUserBase + (int) userPresets.size())
        return (int) presets().size() + id - kUserBase;
    return -1;
}

void RoomPanel::selectPreset(int index)
{
    const int builtIn = (int) presets().size();
    if (index >= 0 && index < builtIn)
        preset.setSelectedId(index + 1, juce::sendNotificationSync);
    else if (index >= builtIn && index < builtIn + (int) userPresets.size())
        preset.setSelectedId(kUserBase + index - builtIn, juce::sendNotificationSync);
}

void RoomPanel::loadUserPresets()
{
    // Saved as JSON: [{ name, level, size, decay, brightness, width, distance (the settings' units), keepLoudness }].
    userPresets.clear();
    const auto list = juce::JSON::parse(settings.getValue("roomUserPresets", "[]"));
    if (const auto* array = list.getArray())
        for (const auto& item : *array) {
            UserPreset p;
            p.name = item.getProperty("name", juce::String()).toString().trim();
            if (p.name.isEmpty())
                continue;
            for (int c = 0; c < count; ++c)
                p.values[(size_t) c] = (double) item.getProperty(kControls[c].key, 0.0) * kControls[c].scale;
            p.keepLoudness = (bool) item.getProperty("keepLoudness", true);
            userPresets.push_back(p);
        }
}

void RoomPanel::storeUserPresets()
{
    juce::Array<juce::var> list;
    for (const auto& p : userPresets) {
        auto* item = new juce::DynamicObject();
        item->setProperty("name", p.name);
        for (int c = 0; c < count; ++c)
            item->setProperty(kControls[c].key, p.values[(size_t) c] / kControls[c].scale);
        item->setProperty("keepLoudness", p.keepLoudness);
        list.add(juce::var(item));
    }
    settings.setValue("roomUserPresets", juce::JSON::toString(juce::var(list), true));
    settings.saveIfNeeded();
}

void RoomPanel::rebuildPresetList(int selectId)
{
    preset.clear(juce::dontSendNotification);
    for (int i = 0; i < (int) presets().size(); ++i)
        preset.addItem(presets()[(size_t) i].name, i + 1);
    preset.addItem("Custom", kCustomId);
    if (!userPresets.empty()) {
        preset.addSectionHeading("My presets (room and position)");
        for (int i = 0; i < (int) userPresets.size(); ++i)
            preset.addItem(userPresets[(size_t) i].name, kUserBase + i);
    }
    preset.setSelectedId(selectId, juce::dontSendNotification);
    deletePreset.setEnabled(selectId >= kUserBase);
}

void RoomPanel::loadUserPreset(int index)
{
    const auto& p = userPresets[(size_t) index];
    const bool was = loading;
    loading = true;
    for (int c = 0; c < count; ++c)
        sliders[(size_t) c].setValue(p.values[(size_t) c], juce::dontSendNotification);
    keepLoudness.setToggleState(p.keepLoudness, juce::dontSendNotification);
    int pos = kCustomId;
    for (int i = 0; i < (int) positions().size(); ++i)
        if (std::abs(positions()[(size_t) i].distance - p.values[distance]) < 0.05)
            pos = i + 1;
    position.setSelectedId(pos, juce::dontSendNotification);
    loading = was;
    apply();
}

void RoomPanel::askSavePreset()
{
    const int id = preset.getSelectedId();
    const auto suggested = id >= kUserBase ? preset.getText() : juce::String();
    auto* window = new juce::AlertWindow("Save the room", "A name for this room and this listening position "
                                                          "(an existing name is replaced):",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", suggested, "Name");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([this, window](int result) {
        const auto name = window->getTextEditorContents("name").trim();
        if (result != 1 || name.isEmpty())
            return;
        UserPreset p;
        p.name = name;
        for (int c = 0; c < count; ++c)
            p.values[(size_t) c] = sliders[(size_t) c].getValue();
        p.keepLoudness = keepLoudness.getToggleState();
        int index = -1;
        for (int i = 0; i < (int) userPresets.size(); ++i)
            if (userPresets[(size_t) i].name == name)
                index = i;
        if (index >= 0)
            userPresets[(size_t) index] = p;
        else {
            userPresets.push_back(p);
            index = (int) userPresets.size() - 1;
        }
        storeUserPresets();
        rebuildPresetList(kUserBase + index);
        save();
        if (onChange)
            onChange();
    }), true);
}

void RoomPanel::askDeletePreset()
{
    const int index = preset.getSelectedId() - kUserBase;
    if (index < 0 || index >= (int) userPresets.size())
        return;
    const auto name = userPresets[(size_t) index].name;
    juce::AlertWindow::showAsync(juce::MessageBoxOptions {}
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Delete a preset")
                                     .withMessage("Delete the preset \"" + name + "\"? The room as it sounds now stays.")
                                     .withButton("Delete", 1)
                                     .withButton("Cancel", 0)
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<RoomPanel>(this), name](int result) {
                                     if (result == 0 || safe == nullptr)
                                         return;
                                     auto& self = *safe;
                                     for (auto it = self.userPresets.begin(); it != self.userPresets.end(); ++it)
                                         if (it->name == name) {
                                             self.userPresets.erase(it);
                                             break;
                                         }
                                     self.storeUserPresets();
                                     self.rebuildPresetList(kCustomId);
                                     self.save();
                                     if (self.onChange)
                                         self.onChange();
                                 });
}

void RoomPanel::apply()
{
    pf::Room::Settings s;
    s.enabled = onButton.getToggleState();
    s.levelDb = (float) sliders[level].getValue();
    s.size = (float) (sliders[size].getValue() / 100.0);
    s.decaySeconds = (float) sliders[decay].getValue();
    s.brightness = (float) (sliders[brightness].getValue() / 100.0);
    s.width = (float) (sliders[width].getValue() / 100.0);
    s.distance = (float) sliders[distance].getValue();
    s.keepLoudness = keepLoudness.getToggleState();
    s.headphones = onHeadphones();
    room.applySettings(s);
    for (int c = 0; c < count; ++c)
        sliders[(size_t) c].setEnabled(s.enabled);
    for (auto* c : std::initializer_list<juce::Component*> { &preset, &position, &keepLoudness })
        c->setEnabled(s.enabled);
    startTimer(60); // the summary (not on every pixel of a drag)
}

void RoomPanel::timerCallback()
{
    stopTimer();
    describe();
}

void RoomPanel::describe()
{
    const auto s = room.settings();
    const auto a = pf::Room::acoustics(s);
    juce::String text;
    text << withThousands(a.volume) << cubicMetres() << " (" << juce::String(juce::roundToInt(a.length)) << " x " << juce::String(juce::roundToInt(a.widthM)) << " x "
         << juce::String(a.height, 1) << " m), T60 " << juce::String(s.decaySeconds, 1) << " s: the room carries as much energy as "
         << "the direct sound " << juce::String(a.criticalDistance, 1) << " m from the piano.\n";
    text << "At " << juce::String(a.distance, 1) << " m";
    if (a.distance < s.distance - 0.05)
        text << " (the room is " << juce::String(juce::roundToInt(a.length)) << " m long: as far as it goes)";
    text << ": the room " << (a.reflectionsOverDirectDb >= 0.0 ? "+" : "") << juce::String(a.reflectionsOverDirectDb, 1)
         << " dB against the direct sound, first reflection after " << juce::String(juce::roundToInt(a.firstReflectionMs)) << " ms; ";
    text << "direct sound " << juce::String(a.directDb + a.loudnessDb, 1) << " dB";
    if (a.airDbAt10k >= 0.05)
        text << ", -" << juce::String(a.airDbAt10k, 1) << " dB at 10 kHz (air)";
    if (!s.headphones) // on headphones, the angle below instead
        text << ", width " << juce::String(juce::roundToInt(100.0 * a.directWidth)) << " %";
    if (s.keepLoudness && std::abs(a.loudnessDb) >= 0.05)
        text << " (keep loudness: +" << juce::String(a.loudnessDb, 1) << " dB on everything)";
    text << ".";
    // On headphones, where each side of the piano is (from the keyboard with the room off).
    auto keyboard = s;
    keyboard.distance = pf::Room::kReferenceDistance;
    const auto ears = pf::Room::acoustics(s.enabled ? s : keyboard);
    const juce::String phones = "\nOn headphones: each side of the piano " + juce::String(juce::roundToInt(ears.sideAngle))
                              + juce::String::fromUTF8("\xc2\xb0") + " from the front, "
                              + juce::String(ears.interauralMs, 2) + " ms from one ear to the other; the reflections from where they come.";
    juce::String off = "Room off: the engine's sound, heard from the keyboard.";
    if (s.headphones)
        off << phones.replace("; the reflections from where they come.", ".");
    summary.setText(s.enabled ? text + (s.headphones ? phones : juce::String()) : off, juce::dontSendNotification);
}

void RoomPanel::save()
{
    settings.setValue("roomModel", kModel);
    settings.setValue("roomOn", onButton.getToggleState());
    settings.setValue("roomPreset", preset.getSelectedId());
    settings.setValue("roomPresetName", preset.getSelectedId() >= kUserBase ? preset.getText() : juce::String());
    settings.setValue("roomPosition", position.getSelectedId());
    settings.setValue("roomKeepLoudness", keepLoudness.getToggleState());
    settings.setValue("roomHeadphones", onHeadphones());
    for (int c = 0; c < count; ++c)
        settings.setValue(kControls[c].key, sliders[(size_t) c].getValue() / kControls[c].scale);
    settings.saveIfNeeded();
}

void RoomPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, roomCard, "Room around the piano");
    theme::drawCard(g, positionCard, "Listening position");
}

void RoomPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    roomCard = area.removeFromTop(248);
    area.removeFromTop(10);
    positionCard = area;

    auto inner = roomCard.reduced(12, 8);
    inner.removeFromTop(20);
    auto row = inner.removeFromTop(28);
    onButton.setBounds(row.removeFromLeft(110));
    row.removeFromLeft(20);
    presetLabel.setBounds(row.removeFromLeft(60));
    preset.setBounds(row.removeFromLeft(196).reduced(0, 2));
    row.removeFromLeft(8);
    savePreset.setBounds(row.removeFromLeft(74).reduced(0, 2));
    row.removeFromLeft(4);
    deletePreset.setBounds(row.removeFromLeft(70).reduced(0, 2));
    inner.removeFromTop(8);
    for (int c = 0; c < distance; ++c) {
        row = inner.removeFromTop(32);
        labels[(size_t) c].setBounds(row.removeFromLeft(110));
        sliders[(size_t) c].setBounds(row.reduced(0, 2));
    }

    inner = positionCard.reduced(12, 8);
    inner.removeFromTop(20);
    row = inner.removeFromTop(28);
    positionLabel.setBounds(row.removeFromLeft(110));
    position.setBounds(row.removeFromLeft(240).reduced(0, 2));
    row.removeFromLeft(16);
    keepLoudness.setBounds(row);
    inner.removeFromTop(4);
    row = inner.removeFromTop(32);
    labels[distance].setBounds(row.removeFromLeft(110));
    sliders[distance].setBounds(row.reduced(0, 2));
    inner.removeFromTop(4);
    row = inner.removeFromTop(28);
    heardOnLabel.setBounds(row.removeFromLeft(110));
    heardOn.setBounds(row.removeFromLeft(240).reduced(0, 2));
    inner.removeFromTop(6);
    summary.setBounds(inner.removeFromTop(96));
    note.setBounds(inner);
}

} // namespace pf::app
