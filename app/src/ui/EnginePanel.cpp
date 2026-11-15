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

    // The unison's tuning: the strings of each note detuned as measured (100 %), drawn together (0 %: no beating, a
    // single decay) or apart (an old upright).
    unisonLabel.setText("Unison tuning", juce::dontSendNotification);
    unison.setRange(0.0, 300.0, 1.0);
    unison.setTextValueSuffix(" %");
    unison.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    unison.setDoubleClickReturnValue(true, 100.0);
    unison.setTooltip("How far apart the strings of each note are tuned: 100 % as measured, 0 % in perfect unison (no "
                      "beating), 200 to 300 % an old upright's. The notes struck from then on.");
    unison.setValue(settings.getDoubleValue("unisonSpread", 1.0) * 100.0, juce::dontSendNotification);
    unison.onValueChange = [this] { modal.unisonSpread = (float) (unison.getValue() / 100.0); };
    unison.onDragEnd = [this] { save(); };
    modal.unisonSpread = (float) (unison.getValue() / 100.0);

    // A re-strike: what the hammer absorbs of the string's vibration over its contact (fitted on the reference).
    absorptionLabel.setText("Re-strike absorption", juce::dontSendNotification);
    absorption.setRange(0.0, 95.0, 1.0);
    absorption.setTextValueSuffix(" %");
    absorption.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    absorption.setDoubleClickReturnValue(true, 100.0 * pf::kDefaultRestrikeAbsorption);
    absorption.setTooltip("A note struck again strikes its string while it rings, and the strikes add up; the hammer, pressed on "
                          "the string, absorbs this share of the vibration's energy (0 %: they add exactly). Fitted on the "
                          "reference's repeated notes: 50 %.");
    absorption.setValue(settings.getDoubleValue("restrikeAbsorption", pf::kDefaultRestrikeAbsorption) * 100.0, juce::dontSendNotification);
    absorption.onValueChange = [this] { modal.restrikeAbsorption = (float) (absorption.getValue() / 100.0); };
    absorption.onDragEnd = [this] { save(); };
    modal.restrikeAbsorption = (float) (absorption.getValue() / 100.0);

    restrikeLabel.setText("The previous note fades to -40 dB in", juce::dontSendNotification);
    restrike.setRange(0.015, 5.0, 0.005);
    restrike.setSkewFactorFromMidPoint(0.8);
    restrike.setTextValueSuffix(" s");
    restrike.setNumDecimalPlacesToDisplay(2);
    restrike.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    restrike.setDoubleClickReturnValue(true, 0.8);
    restrike.setValue(settings.getDoubleValue("restrikeFade", 0.8), juce::dontSendNotification);
    restrike.setTooltip("The reference player only: the modal engine strikes the string again while it rings, as a piano does");
    restrike.onValueChange = [this] { player.restrikeFadeSeconds = (float) restrike.getValue(); };
    restrike.onDragEnd = [this] { save(); };
    player.restrikeFadeSeconds = (float) restrike.getValue();

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

    // Voicing: what a technician does to the hammers, by register. Negative: needled, softer (a rounder tone);
    // positive: hardened (brighter). The partials above the corner move; the fundamental stays.
    const char* registers[] = { "Bass (A0)", "Middle (C4)", "Treble (C8)" };
    const char* voicingKeys[] = { "voicingBassDb", "voicingMiddleDb", "voicingTrebleDb" };
    for (size_t i = 0; i < voicing.size(); ++i) {
        voicingLabels[i].setText(registers[i], juce::dontSendNotification);
        auto& slider = voicing[i];
        slider.setRange(-18.0, 12.0, 0.5);
        slider.setTextValueSuffix(" dB");
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        slider.setDoubleClickReturnValue(true, 0.0);
        slider.setTooltip("The hammers of this register: below 0 dB needled (softer, rounder), above hardened (brighter). The upper "
                     "partials move, the fundamental stays; linear between A0, C4 and C8. The notes struck from then on.");
        slider.setValue(settings.getDoubleValue(voicingKeys[i], 0.0), juce::dontSendNotification);
        slider.onValueChange = [this] { applyVoicing(); };
        slider.onDragEnd = [this] { save(); };
    }
    voicingFollowLabel.setText("Follows the velocity", juce::dontSendNotification);
    voicingFollow.setRange(0.0, 100.0, 1.0);
    voicingFollow.setTextValueSuffix(" %");
    voicingFollow.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    voicingFollow.setDoubleClickReturnValue(true, 100.0 * pf::ModalEngine::kDefaultVoicingVelocity);
    voicingFollow.setTooltip("0 %: the same at every velocity; 100 %: in proportion to it (a needled felt changes the forte most)");
    voicingFollow.setValue(settings.getDoubleValue("voicingVelocity", pf::ModalEngine::kDefaultVoicingVelocity) * 100.0,
                           juce::dontSendNotification);
    voicingFollow.onValueChange = [this] { applyVoicing(); };
    voicingFollow.onDragEnd = [this] { save(); };
    voicingCornerLabel.setText("From partial", juce::dontSendNotification);
    voicingCorner.setRange(2.0, 10.0, 0.5);
    voicingCorner.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    voicingCorner.setDoubleClickReturnValue(true, pf::ModalEngine::kDefaultVoicingCorner);
    voicingCorner.setTooltip("Where the change sets in, in partials of the note (the shelf's midpoint): lower reaches more of the tone");
    voicingCorner.setValue(settings.getDoubleValue("voicingCorner", pf::ModalEngine::kDefaultVoicingCorner), juce::dontSendNotification);
    voicingCorner.onValueChange = [this] { applyVoicing(); };
    voicingCorner.onDragEnd = [this] { save(); };
    resetVoicing.onClick = [this] {
        for (auto& slider : voicing)
            slider.setValue(0.0, juce::dontSendNotification);
        voicingFollow.setValue(100.0 * pf::ModalEngine::kDefaultVoicingVelocity, juce::dontSendNotification);
        voicingCorner.setValue(pf::ModalEngine::kDefaultVoicingCorner, juce::dontSendNotification);
        applyVoicing();
        save();
    };
    applyVoicing();

    for (auto* c : std::initializer_list<juce::Component*> { &sympatheticLabel, &sympatheticLevel, &unisonLabel, &unison, &absorptionLabel, &absorption, &restrikeLabel, &restrike,
                                                             &a4Label, &referenceA4, &temperamentLabel, &temperament, &root,
                                                             &resetTuning, &voicingFollowLabel, &voicingFollow, &voicingCornerLabel,
                                                             &voicingCorner, &resetVoicing })
        addAndMakeVisible(*c);
    for (size_t i = 0; i < voicing.size(); ++i) {
        addAndMakeVisible(voicingLabels[i]);
        addAndMakeVisible(voicing[i]);
    }
    setSize(640, 820);
}

void EnginePanel::reload()
{
    for (size_t i = 0; i < toggles.size(); ++i) {
        toggles[i].setToggleState(settings.getBoolValue(kOptions[i].key, kOptions[i].defaultOn), juce::dontSendNotification);
        apply((Option) i);
    }
    sympatheticLevel.setValue(settings.getDoubleValue("sympatheticDb", 0.0), juce::dontSendNotification);
    modal.sympatheticGain = juce::Decibels::decibelsToGain((float) sympatheticLevel.getValue());
    unison.setValue(settings.getDoubleValue("unisonSpread", 1.0) * 100.0, juce::dontSendNotification);
    modal.unisonSpread = (float) (unison.getValue() / 100.0);
    absorption.setValue(settings.getDoubleValue("restrikeAbsorption", pf::kDefaultRestrikeAbsorption) * 100.0, juce::dontSendNotification);
    modal.restrikeAbsorption = (float) (absorption.getValue() / 100.0);
    restrike.setValue(settings.getDoubleValue("restrikeFade", 0.8), juce::dontSendNotification);
    player.restrikeFadeSeconds = (float) restrike.getValue();
    referenceA4.setValue(settings.getDoubleValue("referenceA4", 440.0), juce::dontSendNotification);
    temperament.setSelectedId(juce::jlimit(1, (int) kTemperaments.size(), settings.getIntValue("temperament", 1)), juce::dontSendNotification);
    root.setSelectedId(juce::jlimit(1, 12, settings.getIntValue("temperamentRoot", 1)), juce::dontSendNotification);
    applyTuning();
    voicing[0].setValue(settings.getDoubleValue("voicingBassDb", 0.0), juce::dontSendNotification);
    voicing[1].setValue(settings.getDoubleValue("voicingMiddleDb", 0.0), juce::dontSendNotification);
    voicing[2].setValue(settings.getDoubleValue("voicingTrebleDb", 0.0), juce::dontSendNotification);
    voicingFollow.setValue(settings.getDoubleValue("voicingVelocity", pf::ModalEngine::kDefaultVoicingVelocity) * 100.0,
                           juce::dontSendNotification);
    voicingCorner.setValue(settings.getDoubleValue("voicingCorner", pf::ModalEngine::kDefaultVoicingCorner), juce::dontSendNotification);
    applyVoicing();
}

void EnginePanel::applyVoicing()
{
    modal.voicingBassDb = (float) voicing[0].getValue();
    modal.voicingMiddleDb = (float) voicing[1].getValue();
    modal.voicingTrebleDb = (float) voicing[2].getValue();
    modal.voicingVelocity = (float) (voicingFollow.getValue() / 100.0);
    modal.voicingCorner = (float) voicingCorner.getValue();
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
    settings.setValue("unisonSpread", unison.getValue() / 100.0);
    settings.setValue("restrikeAbsorption", absorption.getValue() / 100.0);
    settings.setValue("restrikeFade", restrike.getValue());
    settings.setValue("referenceA4", referenceA4.getValue());
    settings.setValue("temperament", temperament.getSelectedId());
    settings.setValue("temperamentRoot", root.getSelectedId());
    settings.setValue("voicingBassDb", voicing[0].getValue());
    settings.setValue("voicingMiddleDb", voicing[1].getValue());
    settings.setValue("voicingTrebleDb", voicing[2].getValue());
    settings.setValue("voicingVelocity", voicingFollow.getValue() / 100.0);
    settings.setValue("voicingCorner", voicingCorner.getValue());
    settings.saveIfNeeded();
}

void EnginePanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, modalArea, "Modal engine: what it models");
    theme::drawCard(g, voicingArea, "Voicing: the hammers (modal engine)");
    theme::drawCard(g, voicesArea, "Reference player: re-strike");
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
    auto inner = place(modalArea, 18 + 6 * 40 + 34 + 30 + 30);
    for (int i = 0; i < (int) Option::wholeTake; ++i) {
        auto row = inner.removeFromTop(40);
        toggles[(size_t) i].setBounds(row.removeFromTop(22));
        notes[(size_t) i].setBounds(row.withTrimmedLeft(26));
    }
    auto row = inner.removeFromTop(28);
    sympatheticLabel.setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
    sympatheticLevel.setBounds(row.removeFromLeft(300));
    inner.removeFromTop(2);
    row = inner.removeFromTop(28);
    unisonLabel.setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
    unison.setBounds(row.removeFromLeft(300));
    inner.removeFromTop(2);
    row = inner.removeFromTop(28);
    absorptionLabel.setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
    absorption.setBounds(row.removeFromLeft(300));

    inner = place(voicingArea, 18 + 3 * 30 + 2 * 30 + 4);
    for (size_t i = 0; i < voicing.size(); ++i) {
        row = inner.removeFromTop(28);
        voicingLabels[i].setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
        voicing[i].setBounds(row.removeFromLeft(300));
        if (i == 0) {
            row.removeFromLeft(10);
            resetVoicing.setBounds(row.removeFromLeft(110).reduced(0, 2));
        }
        inner.removeFromTop(2);
    }
    row = inner.removeFromTop(28);
    voicingFollowLabel.setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
    voicingFollow.setBounds(row.removeFromLeft(300));
    inner.removeFromTop(2);
    row = inner.removeFromTop(28);
    voicingCornerLabel.setBounds(row.removeFromLeft(196).withTrimmedLeft(26));
    voicingCorner.setBounds(row.removeFromLeft(300));

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
