// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TonePanel.h"

#include "Theme.h"

namespace pf::app {

namespace {

constexpr int kRowHeight = 28, kRowGap = 2, kLabelWidth = 196, kSliderWidth = 300;

const pf::Compressor::Settings kCompressorDefaults {};

juce::String lidText(double v)
{
    if (v >= 99.5)
        return "Open";
    if (std::abs(v - 50.0) < 0.5)
        return "Half stick";
    if (v <= 0.5)
        return "Closed";
    return juce::String(juce::roundToInt(v)) + " % open";
}

} // namespace

TonePanel::TonePanel(pf::ModalEngine& m, AudioHost& h, juce::PropertiesFile& s) : modal(m), host(h), settings(s)
{
    // Touch and resonance: what the modal engine plays, set as a technician or a player would want it.
    setup(dynamicRange, "Dynamic range", 25.0, 150.0, 1.0, " %", 100.0,
          "The distance of each strike below the note's loudest, in dB: 100 % as measured; 50 % the pianissimo half as far "
          "below the fortissimo (a quiet room, a stream); above 100 % further. The notes struck from then on.");
    setup(sustain, "Sustain length", 0.25, 4.0, 0.01, " x", 1.0,
          "The strings' decay times: 1 x as measured, 2 x twice as long, 0.5 x half (a drier, shorter tone). The notes "
          "struck from then on.");
    sustain.slider.setSkewFactorFromMidPoint(1.0);
    setup(dampers, "Damper speed", 0.2, 5.0, 0.01, " x", 1.0,
          "How fast the dampers stop the strings once a key is let go: 1 x as measured, 0.5 x a slower, longer release, "
          "2 x a quicker one.");
    dampers.slider.setSkewFactorFromMidPoint(1.0);
    setup(attackSoft, "Attack", -24.0, 12.0, 0.5, " dB", 0.0,
          "The strike's brightness above 1.2 kHz at the attack, fading out over the attack's length: below 0 dB a softer, "
          "rounder attack; above, a harder one. The sound after it as measured.");
    setup(attackLength, "Attack length", 5.0, 300.0, 1.0, " ms", pf::ModalEngine::kDefaultAttackSoftMs,
          "Over how long the attack's softness fades out.");
    attackLength.slider.setSkewFactorFromMidPoint(40.0);
    setup(lid, "Lid", 0.0, 100.0, 1.0, {}, 100.0,
          "Open as measured; on the half stick or closed, the treble (above 1.5 kHz) goes down, up to 9 dB, and the whole "
          "sound by up to 2 dB, as when the lid no longer sends the sound out to the listener. At once.");
    lid.slider.textFromValueFunction = lidText;
    lid.slider.valueFromTextFunction = [](const juce::String& t) {
        if (t.containsIgnoreCase("open"))
            return t.containsAnyOf("0123456789") ? t.getDoubleValue() : 100.0;
        if (t.containsIgnoreCase("half"))
            return 50.0;
        if (t.containsIgnoreCase("closed"))
            return 0.0;
        return t.getDoubleValue();
    };
    lid.slider.updateText();
    for (auto* r : { &dynamicRange, &sustain, &dampers, &attackSoft, &attackLength, &lid })
        r->slider.onValueChange = [this] { applyEngine(); };
    resetEngine.setTooltip("Every setting of this card back to the sound as measured");
    resetEngine.onClick = [this] {
        for (auto* r : { &dynamicRange, &sustain, &dampers, &attackSoft, &attackLength, &lid })
            r->slider.setValue(r->slider.getDoubleClickReturnValue(), juce::dontSendNotification);
        applyEngine();
        save();
    };

    // The output's tone.
    setup(bass, "Bass (120 Hz)", -12.0, 12.0, 0.5, " dB", 0.0, "A low shelf: the bass of the whole output");
    setup(presence, "Presence (2.5 kHz)", -12.0, 12.0, 0.5, " dB", 0.0,
          "A broad bell: the presence, where a piano can sound hard (down) or recessed (up)");
    setup(air, "Air (10 kHz)", -12.0, 12.0, 0.5, " dB", 0.0, "A high shelf: the air above the piano's tone");
    for (auto* r : { &bass, &presence, &air })
        r->slider.onValueChange = [this] { applyTone(); };
    resetTone.onClick = [this] {
        for (auto* r : { &bass, &presence, &air })
            r->slider.setValue(0.0, juce::dontSendNotification);
        applyTone();
        save();
    };

    // The convolution reverb.
    reverbOn.setTooltip("The piano's sound before the room through an impulse response (a real hall's, a church's, a "
                        "studio's: a WAV file), added to the room's output. Turn the room off to hear the hall alone.");
    reverbOn.onClick = [this] {
        applyTone();
        save();
    };
    chooseFile.onClick = [this] { chooseImpulse(); };
    clearFile.setTooltip("No impulse response");
    clearFile.onClick = [this] {
        impulseFile = juce::File();
        host.setImpulse(nullptr);
        fileLabel.setText("No impulse response", juce::dontSendNotification);
        fileLabel.setColour(juce::Label::textColourId, theme::dim);
        save();
    };
    fileLabel.setColour(juce::Label::textColourId, theme::dim);
    setup(wet, "Reverb level", -40.0, 6.0, 0.5, " dB", -12.0, "The reverb's level (the impulse response normalised)");
    setup(dry, "Room level", -40.0, 0.0, 0.5, {}, 0.0,
          "The room's output (the piano and the room) beside the reverb; at -40 dB, none: the reverb alone");
    dry.slider.textFromValueFunction = [](double v) { return v <= kDryOffDb ? juce::String("None") : juce::String(v, 1) + " dB"; };
    dry.slider.updateText();
    for (auto* r : { &wet, &dry })
        r->slider.onValueChange = [this] { applyTone(); };

    // The stream's compressor.
    compressorOn.setTooltip("What leaves for OBS (the PianoForge source, every mix) and the mirror output, compressed: "
                            "the quiet passages come up for listeners on small speakers. What you hear and record stays as it is.");
    compressorOn.onClick = [this] {
        applyCompressor();
        save();
    };
    reduction.setColour(juce::Label::textColourId, theme::dim);
    reduction.setJustificationType(juce::Justification::centredRight);
    setup(threshold, "Threshold", -60.0, 0.0, 0.5, " dBFS", kCompressorDefaults.thresholdDb, "Above it, the level is reduced");
    setup(ratio, "Ratio", 1.0, 20.0, 0.1, " : 1", kCompressorDefaults.ratio, "Above the threshold, 1 dB out for this many dB in");
    ratio.slider.setSkewFactorFromMidPoint(4.0);
    setup(attack, "Attack", 0.1, 200.0, 0.1, " ms", kCompressorDefaults.attackMs, "How fast the reduction sets in");
    attack.slider.setSkewFactorFromMidPoint(10.0);
    setup(release, "Release", 5.0, 2000.0, 1.0, " ms", kCompressorDefaults.releaseMs, "How fast it lets go");
    release.slider.setSkewFactorFromMidPoint(200.0);
    setup(makeup, "Make-up", 0.0, 24.0, 0.5, " dB", kCompressorDefaults.makeupDb, "The level given back after the reduction");
    for (auto* r : { &threshold, &ratio, &attack, &release, &makeup })
        r->slider.onValueChange = [this] { applyCompressor(); };
    resetCompressor.onClick = [this] {
        for (auto* r : { &threshold, &ratio, &attack, &release, &makeup })
            r->slider.setValue(r->slider.getDoubleClickReturnValue(), juce::dontSendNotification);
        applyCompressor();
        save();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &resetEngine, &resetTone, &reverbOn, &chooseFile, &clearFile, &fileLabel,
                                                             &compressorOn, &reduction, &resetCompressor })
        addAndMakeVisible(*c);
    reload();
    setSize(640, 760);
    startTimerHz(8);
}

TonePanel::~TonePanel()
{
    stopTimer();
}

void TonePanel::setup(Row& row, const juce::String& name, double min, double max, double step, const juce::String& suffix,
                      double reset, const juce::String& tooltip)
{
    row.label.setText(name, juce::dontSendNotification);
    row.slider.setRange(min, max, step);
    row.slider.setTextValueSuffix(suffix);
    row.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    row.slider.setDoubleClickReturnValue(true, reset);
    row.slider.setValue(reset, juce::dontSendNotification);
    row.slider.setTooltip(tooltip);
    row.slider.onDragEnd = [this] { save(); };
    addAndMakeVisible(row.label);
    addAndMakeVisible(row.slider);
}

void TonePanel::reload()
{
    auto value = [&](Row& r, const char* key, double scale = 1.0) {
        r.slider.setValue(settings.getDoubleValue(key, r.slider.getDoubleClickReturnValue() / scale) * scale, juce::dontSendNotification);
    };
    value(dynamicRange, "dynamicRange", 100.0);
    value(sustain, "sustainLength");
    value(dampers, "damperRate");
    value(attackSoft, "attackSoftDb");
    value(attackLength, "attackSoftMs");
    value(lid, "lidOpen", 100.0);
    value(bass, "toneBassDb");
    value(presence, "tonePresenceDb");
    value(air, "toneAirDb");
    reverbOn.setToggleState(settings.getBoolValue("convolutionOn", false), juce::dontSendNotification);
    value(wet, "convolutionWetDb");
    value(dry, "convolutionDryDb");
    compressorOn.setToggleState(settings.getBoolValue("streamCompressorOn", false), juce::dontSendNotification);
    value(threshold, "streamCompressorThreshold");
    value(ratio, "streamCompressorRatio");
    value(attack, "streamCompressorAttack");
    value(release, "streamCompressorRelease");
    value(makeup, "streamCompressorMakeup");
    applyEngine();
    applyTone();
    applyCompressor();
    const juce::File file(settings.getValue("convolutionFile"));
    if (file != impulseFile || host.impulse() == nullptr) {
        if (file.existsAsFile())
            loadImpulse(file, false);
        else {
            fileLabel.setText(settings.getValue("convolutionFile").isEmpty() ? "No impulse response"
                                                                              : "Not found: " + file.getFileName(),
                              juce::dontSendNotification);
            fileLabel.setColour(juce::Label::textColourId, theme::dim);
        }
    }
}

void TonePanel::applyEngine()
{
    modal.dynamicRange = (float) (dynamicRange.slider.getValue() / 100.0);
    modal.sustainLength = (float) sustain.slider.getValue();
    modal.damperRate = (float) dampers.slider.getValue();
    modal.attackSoftDb = (float) attackSoft.slider.getValue();
    modal.attackSoftMs = (float) attackLength.slider.getValue();
    modal.lidOpen = (float) (lid.slider.getValue() / 100.0);
}

void TonePanel::applyTone()
{
    host.toneBassDb = (float) bass.slider.getValue();
    host.tonePresenceDb = (float) presence.slider.getValue();
    host.toneAirDb = (float) air.slider.getValue();
    host.convolutionWetDb = (float) wet.slider.getValue();
    host.convolutionDryDb = (float) dry.slider.getValue();
    host.convolutionOn = reverbOn.getToggleState();
}

void TonePanel::applyCompressor()
{
    pf::Compressor::Settings c;
    c.enabled = compressorOn.getToggleState();
    c.thresholdDb = (float) threshold.slider.getValue();
    c.ratio = (float) ratio.slider.getValue();
    c.attackMs = (float) attack.slider.getValue();
    c.releaseMs = (float) release.slider.getValue();
    c.makeupDb = (float) makeup.slider.getValue();
    host.setStreamCompressor(c);
}

void TonePanel::save()
{
    settings.setValue("dynamicRange", dynamicRange.slider.getValue() / 100.0);
    settings.setValue("sustainLength", sustain.slider.getValue());
    settings.setValue("damperRate", dampers.slider.getValue());
    settings.setValue("attackSoftDb", attackSoft.slider.getValue());
    settings.setValue("attackSoftMs", attackLength.slider.getValue());
    settings.setValue("lidOpen", lid.slider.getValue() / 100.0);
    settings.setValue("toneBassDb", bass.slider.getValue());
    settings.setValue("tonePresenceDb", presence.slider.getValue());
    settings.setValue("toneAirDb", air.slider.getValue());
    settings.setValue("convolutionOn", reverbOn.getToggleState());
    settings.setValue("convolutionFile", impulseFile.getFullPathName());
    settings.setValue("convolutionWetDb", wet.slider.getValue());
    settings.setValue("convolutionDryDb", dry.slider.getValue());
    settings.setValue("streamCompressorOn", compressorOn.getToggleState());
    settings.setValue("streamCompressorThreshold", threshold.slider.getValue());
    settings.setValue("streamCompressorRatio", ratio.slider.getValue());
    settings.setValue("streamCompressorAttack", attack.slider.getValue());
    settings.setValue("streamCompressorRelease", release.slider.getValue());
    settings.setValue("streamCompressorMakeup", makeup.slider.getValue());
    settings.saveIfNeeded();
}

void TonePanel::chooseImpulse()
{
    const auto start = impulseFile.existsAsFile() ? impulseFile.getParentDirectory()
                                                  : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser>("Choose an impulse response", start, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [safe = juce::Component::SafePointer<TonePanel>(this)](const juce::FileChooser& fc) {
                             if (auto* self = safe.getComponent(); self != nullptr && fc.getResult().existsAsFile()) {
                                 self->loadImpulse(fc.getResult(), true);
                                 if (self->host.impulse() != nullptr && !self->reverbOn.getToggleState()) {
                                     self->reverbOn.setToggleState(true, juce::dontSendNotification); // a file chosen: heard
                                     self->applyTone();
                                 }
                                 self->save();
                             }
                         });
}

void TonePanel::loadImpulse(const juce::File& file, bool report)
{
    juce::String error;
    auto ir = readImpulse(file, error);
    if (ir == nullptr) {
        fileLabel.setText(error, juce::dontSendNotification);
        fileLabel.setColour(juce::Label::textColourId, report ? theme::bad : theme::dim);
        return;
    }
    impulseFile = file;
    fileLabel.setText(ir->name + "  (" + juce::String(ir->samples.getNumSamples() / ir->sampleRate, 2) + " s, "
                          + (ir->stereo() ? "stereo" : "mono") + ", " + juce::String(ir->sampleRate / 1000.0, 1) + " kHz)",
                      juce::dontSendNotification);
    fileLabel.setColour(juce::Label::textColourId, theme::text);
    host.setImpulse(std::move(ir));
}

void TonePanel::timerCallback()
{
    host.collectGarbage();
    if (!isShowing())
        return;
    const bool on = compressorOn.getToggleState();
    reduction.setText(on ? "Reduction " + juce::String(host.streamReduction(), 1) + " dB" : juce::String(),
                      juce::dontSendNotification);
}

void TonePanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, engineArea, "Touch and resonance (modal engine)");
    theme::drawCard(g, toneArea, "Output tone: what is heard, recorded and streamed");
    theme::drawCard(g, reverbArea, "Convolution reverb, beside the room");
    theme::drawCard(g, compressorArea, "Stream compressor: OBS and the mirror only");
}

void TonePanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto place = [&](juce::Rectangle<int>& card, int rows) {
        card = area.removeFromTop(18 + 16 + rows * (kRowHeight + kRowGap));
        area.removeFromTop(10);
        auto inner = card.reduced(12, 8);
        inner.removeFromTop(18);
        return inner;
    };
    auto put = [&](juce::Rectangle<int>& inner, Row& r, juce::Component* side = nullptr) {
        auto row = inner.removeFromTop(kRowHeight);
        inner.removeFromTop(kRowGap);
        r.label.setBounds(row.removeFromLeft(kLabelWidth).withTrimmedLeft(26));
        r.slider.setBounds(row.removeFromLeft(kSliderWidth));
        if (side != nullptr) {
            row.removeFromLeft(10);
            side->setBounds(row.removeFromLeft(100).reduced(0, 2));
        }
    };

    auto inner = place(engineArea, 6);
    put(inner, dynamicRange, &resetEngine);
    put(inner, sustain);
    put(inner, dampers);
    put(inner, attackSoft);
    put(inner, attackLength);
    put(inner, lid);

    inner = place(toneArea, 3);
    put(inner, bass, &resetTone);
    put(inner, presence);
    put(inner, air);

    inner = place(reverbArea, 4);
    auto row = inner.removeFromTop(kRowHeight);
    inner.removeFromTop(kRowGap);
    reverbOn.setBounds(row.removeFromLeft(kLabelWidth));
    chooseFile.setBounds(row.removeFromLeft(150).reduced(0, 2));
    row.removeFromLeft(6);
    clearFile.setBounds(row.removeFromLeft(60).reduced(0, 2));
    row = inner.removeFromTop(kRowHeight);
    inner.removeFromTop(kRowGap);
    fileLabel.setBounds(row.withTrimmedLeft(26));
    put(inner, wet);
    put(inner, dry);

    inner = place(compressorArea, 6);
    row = inner.removeFromTop(kRowHeight);
    inner.removeFromTop(kRowGap);
    compressorOn.setBounds(row.removeFromLeft(kLabelWidth + kSliderWidth - 140));
    reduction.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(10);
    resetCompressor.setBounds(row.removeFromLeft(100).reduced(0, 2));
    put(inner, threshold);
    put(inner, ratio);
    put(inner, attack);
    put(inner, release);
    put(inner, makeup);
}

} // namespace pf::app
