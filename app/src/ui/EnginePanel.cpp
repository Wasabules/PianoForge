#include "EnginePanel.h"

#include "Theme.h"

namespace pf::app {

namespace {

struct OptionInfo {
    const char* key;
    const char* name;
    const char* note;
    bool defaultOn;
};

constexpr OptionInfo kOptions[] = {
    { "modalBody", "Soundboard body", "The resonances of the soundboard and the case, shared by every note.", true },
    { "modalResidual", "Attack residual", "The hammer's knock and the first instants of the strike, as recorded.", true },
    { "modalSympathetic", "Sympathetic resonance", "With the pedal down, the free strings ring with what is played.", true },
    { "modalHeldKeys", "Held keys resonate", "The strings of keys held down ring too, without the pedal (a piano does; the reference does not).", true },
    { "modalPedalCatch", "Pedal catches releases", "Pressing the pedal again keeps what is left of released notes (a piano does; the reference does not).", true },
    { "modalReleaseNoise", "Release noise", "The knock of the dampers landing, on the soundboard.", true },
    { "playWholeTake", "Reference: play the whole take", "The reference player ignores the key release and plays each recording to its end.", false },
};

} // namespace

juce::String EnginePanel::name(Option o)
{
    return kOptions[(size_t) o].name;
}

EnginePanel::EnginePanel(pf::ModalEngine& m, ReferencePlayer& p, juce::PropertiesFile& s)
    : modal(m), player(p), settings(s)
{
    for (size_t i = 0; i < toggles.size(); ++i) {
        const auto& info = kOptions[i];
        toggles[i].setButtonText(info.name);
        toggles[i].setToggleState(settings.getBoolValue(info.key, info.defaultOn), juce::dontSendNotification);
        toggles[i].onClick = [this, i] {
            apply((Option) i);
            save();
        };
        notes[i].setText(info.note, juce::dontSendNotification);
        notes[i].setColour(juce::Label::textColourId, theme::dim);
        notes[i].setFont(theme::font(12.0f));
        addAndMakeVisible(toggles[i]);
        addAndMakeVisible(notes[i]);
        apply((Option) i);
    }

    sympatheticLabel.setText("Sympathetic level", juce::dontSendNotification);
    sympatheticLevel.setRange(-24.0, 12.0, 0.5);
    sympatheticLevel.setTextValueSuffix(" dB");
    sympatheticLevel.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    sympatheticLevel.setDoubleClickReturnValue(true, 0.0);
    sympatheticLevel.setValue(settings.getDoubleValue("sympatheticDb", 0.0), juce::dontSendNotification);
    sympatheticLevel.onValueChange = [this] { modal.sympatheticGain = juce::Decibels::decibelsToGain((float) sympatheticLevel.getValue()); };
    sympatheticLevel.onDragEnd = [this] { save(); };
    modal.sympatheticGain = juce::Decibels::decibelsToGain((float) sympatheticLevel.getValue());

    restrikeLabel.setText("Re-strike: the previous note fades to -40 dB in", juce::dontSendNotification);
    restrike.setRange(0.015, 5.0, 0.005);
    restrike.setSkewFactorFromMidPoint(0.8);
    restrike.setTextValueSuffix(" s");
    restrike.setNumDecimalPlacesToDisplay(2);
    restrike.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    restrike.setDoubleClickReturnValue(true, 0.8);
    restrike.setValue(settings.getDoubleValue("restrikeFade", 0.8), juce::dontSendNotification);
    restrike.onValueChange = [this] { player.restrikeFadeSeconds = modal.restrikeFadeSeconds = (float) restrike.getValue(); };
    restrike.onDragEnd = [this] { save(); };
    player.restrikeFadeSeconds = modal.restrikeFadeSeconds = (float) restrike.getValue();

    a4Label.setText("Reference A4", juce::dontSendNotification);
    referenceA4.setRange(415.0, 466.0, 0.1);
    referenceA4.setTextValueSuffix(" Hz");
    referenceA4.setNumDecimalPlacesToDisplay(1);
    referenceA4.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 20);
    referenceA4.setDoubleClickReturnValue(true, 440.0);
    referenceA4.setValue(settings.getDoubleValue("referenceA4", 440.0), juce::dontSendNotification);
    referenceA4.onValueChange = [this] { applyTuning(); };
    referenceA4.onDragEnd = [this] { save(); };
    temperamentLabel.setText("Temperament", juce::dontSendNotification);
    for (int i = 0; i < (int) kTemperaments.size(); ++i)
        temperament.addItem(kTemperaments[(size_t) i].name, i + 1);
    temperament.setSelectedId(juce::jlimit(1, (int) kTemperaments.size(), settings.getIntValue("temperament", 1)), juce::dontSendNotification);
    const char* keys[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    for (int i = 0; i < 12; ++i)
        root.addItem(juce::String("on ") + keys[i], i + 1);
    root.setSelectedId(juce::jlimit(1, 12, settings.getIntValue("temperamentRoot", 1)), juce::dontSendNotification);
    temperament.onChange = root.onChange = [this] { applyTuning(); save(); };
    resetTuning.onClick = [this] {
        referenceA4.setValue(440.0, juce::dontSendNotification);
        temperament.setSelectedId(1, juce::dontSendNotification);
        root.setSelectedId(1, juce::dontSendNotification);
        applyTuning();
        save();
    };
    applyTuning();

    for (auto* c : std::initializer_list<juce::Component*> { &sympatheticLabel, &sympatheticLevel, &restrikeLabel, &restrike,
                                                             &a4Label, &referenceA4, &temperamentLabel, &temperament, &root,
                                                             &resetTuning })
        addAndMakeVisible(*c);
    setSize(640, 590);
}

void EnginePanel::apply(Option o)
{
    const bool on = isOn(o);
    switch (o) {
    case Option::body: modal.bodyEnabled = on; break;
    case Option::residual: modal.residualEnabled = on; break;
    case Option::sympathetic: modal.sympatheticEnabled = on; break;
    case Option::heldKeys: modal.heldKeysResonate = on; break;
    case Option::pedalCatch: modal.pedalCatch = on; break;
    case Option::releaseNoise: modal.releaseNoiseEnabled = on; break;
    case Option::wholeTake: player.playWholeTake = on; break;
    case Option::count: break;
    }
    sympatheticLevel.setEnabled(isOn(Option::sympathetic));
}

void EnginePanel::applyTuning()
{
    const auto& t = kTemperaments[(size_t) juce::jlimit(0, (int) kTemperaments.size() - 1, temperament.getSelectedId() - 1)];
    const auto offsets = pitchClassOffsets(t, juce::jlimit(0, 11, root.getSelectedId() - 1));
    modal.referenceA4 = (float) referenceA4.getValue();
    for (int pc = 0; pc < 12; ++pc)
        modal.pitchClassCents[(size_t) pc] = (float) offsets[(size_t) pc];
    modal.tuningVersion.fetch_add(1); // the free strings follow
}

void EnginePanel::save()
{
    for (size_t i = 0; i < toggles.size(); ++i)
        settings.setValue(kOptions[i].key, toggles[i].getToggleState());
    settings.setValue("sympatheticDb", sympatheticLevel.getValue());
    settings.setValue("restrikeFade", restrike.getValue());
    settings.setValue("referenceA4", referenceA4.getValue());
    settings.setValue("temperament", temperament.getSelectedId());
    settings.setValue("temperamentRoot", root.getSelectedId());
    settings.saveIfNeeded();
}

void EnginePanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, modalArea, "Modal engine: what it models");
    theme::drawCard(g, voicesArea, "Voices");
    theme::drawCard(g, tuningArea, "Tuning (modal engine)");
    theme::drawCard(g, referenceArea, "Reference player");
}

void EnginePanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto place = [&](juce::Rectangle<int>& card, int height) {
        card = area.removeFromTop(height);
        area.removeFromTop(10);
        auto inner = card.reduced(12, 8);
        inner.removeFromTop(18);
        return inner;
    };
    auto inner = place(modalArea, 18 + 6 * 40 + 34);
    for (int i = 0; i < (int) Option::wholeTake; ++i) {
        auto row = inner.removeFromTop(40);
        toggles[(size_t) i].setBounds(row.removeFromTop(22));
        notes[(size_t) i].setBounds(row.withTrimmedLeft(26));
    }
    auto row = inner.removeFromTop(28);
    sympatheticLabel.setBounds(row.removeFromLeft(150).withTrimmedLeft(26));
    sympatheticLevel.setBounds(row.removeFromLeft(300));

    inner = place(voicesArea, 18 + 36);
    row = inner.removeFromTop(28);
    restrikeLabel.setBounds(row.removeFromLeft(300));
    restrike.setBounds(row);

    inner = place(tuningArea, 18 + 2 * 32 + 4);
    row = inner.removeFromTop(28);
    a4Label.setBounds(row.removeFromLeft(110));
    referenceA4.setBounds(row.removeFromLeft(300));
    row.removeFromLeft(10);
    resetTuning.setBounds(row.removeFromLeft(120).reduced(0, 2));
    inner.removeFromTop(4);
    row = inner.removeFromTop(28);
    temperamentLabel.setBounds(row.removeFromLeft(110));
    temperament.setBounds(row.removeFromLeft(240).reduced(0, 2));
    row.removeFromLeft(8);
    root.setBounds(row.removeFromLeft(90).reduced(0, 2));

    inner = place(referenceArea, 18 + 52);
    const auto i = (size_t) Option::wholeTake;
    toggles[i].setBounds(inner.removeFromTop(22));
    notes[i].setBounds(inner.withTrimmedLeft(26));
}

} // namespace pf::app
