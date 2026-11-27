// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../ReferencePlayer.h"
#include "../Tuning.h"

#include <pianoforge/modal_engine.hpp>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// The engines' settings, in their own window: what the modal engine models (each switchable for A/B listening
/// against the reference), the re-strike, the tuning, and the reference player's option. Created with the
/// application (the settings apply from the start, the window or not), each setting saved as it changes.
class EnginePanel final : public juce::Component {
public:
    enum class Option { body, residual, sympathetic, heldKeys, pedalCatch, releaseNoise, wholeTake, count };

    EnginePanel(pf::ModalEngine& modal, ReferencePlayer& player, juce::PropertiesFile& settings);

    bool isOn(Option o) const { return toggles[(size_t) o].getToggleState(); }
    void setOn(Option o, bool on) { toggles[(size_t) o].setToggleState(on, juce::sendNotification); }
    static juce::String name(Option o);
    float restrikeSeconds() const { return (float) restrike.getValue(); }
    /// Reads every setting back from the settings file and applies it (a preset, an import).
    void reload();
    /// Writes every setting of the panel to the settings file now (a preset's snapshot).
    void saveNow() { save(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void apply(Option o);
    void applyTuning();
    void applyVoicing();
    void save();

    pf::ModalEngine& modal;
    ReferencePlayer& player;
    juce::PropertiesFile& settings;
    std::array<juce::ToggleButton, (size_t) Option::count> toggles;
    std::array<juce::Label, (size_t) Option::count> notes;
    juce::Slider sympatheticLevel, restrike, referenceA4, unison, absorption;
    juce::Label sympatheticLabel, restrikeLabel, a4Label, temperamentLabel, unisonLabel, absorptionLabel;
    juce::ComboBox temperament, root;
    juce::TextButton resetTuning { "440 Hz, equal" };
    // Voicing: the hammers needled (softer) or hardened (brighter), per register, following the velocity.
    std::array<juce::Slider, 3> voicing;  // bass (A0), middle (C4), treble (C8), dB
    std::array<juce::Label, 3> voicingLabels;
    juce::Slider voicingFollow, voicingCorner;
    juce::Label voicingFollowLabel, voicingCornerLabel;
    juce::TextButton resetVoicing { "As measured" };
    juce::Rectangle<int> modalArea, voicingArea, voicesArea, tuningArea, referenceArea;
};

} // namespace pf::app
