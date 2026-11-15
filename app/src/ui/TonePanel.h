#pragma once

#include "../AudioHost.h"

#include <pianoforge/modal_engine.hpp>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace pf::app {

/// Tone and dynamics, in their own window: the touch and resonance of the modal engine (dynamic range, sustain,
/// dampers, attack, lid), the output's tone (an EQ), a convolution reverb in parallel with the room, and the
/// stream's compressor (what leaves for OBS and the mirror only). Created with the application (the settings apply
/// from the start), each setting saved as it changes.
class TonePanel final : public juce::Component, private juce::Timer {
public:
    TonePanel(pf::ModalEngine& modal, AudioHost& host, juce::PropertiesFile& settings);
    ~TonePanel() override;

    /// Reads every setting back from the settings file and applies it (a preset, an import).
    void reload();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Row {
        juce::Label label;
        juce::Slider slider;
    };
    void setup(Row& row, const juce::String& name, double min, double max, double step, const juce::String& suffix,
               double reset, const juce::String& tooltip);
    void applyEngine();
    void applyTone();
    void applyCompressor();
    void save();
    void chooseImpulse();
    void loadImpulse(const juce::File& file, bool report);
    void timerCallback() override;

    pf::ModalEngine& modal;
    AudioHost& host;
    juce::PropertiesFile& settings;

    // Touch and resonance (the modal engine).
    Row dynamicRange, sustain, dampers, attackSoft, attackLength, lid;
    juce::TextButton resetEngine { "As measured" };
    // The output's tone.
    Row bass, presence, air;
    juce::TextButton resetTone { "Flat" };
    // The convolution reverb.
    juce::ToggleButton reverbOn { "Convolution reverb" };
    juce::TextButton chooseFile { "Impulse response..." }, clearFile { "None" };
    juce::Label fileLabel;
    Row wet, dry;
    juce::File impulseFile;
    std::unique_ptr<juce::FileChooser> chooser;
    // The stream's compressor.
    juce::ToggleButton compressorOn { "Compress what leaves for a stream" };
    juce::Label reduction;
    Row threshold, ratio, attack, release, makeup;
    juce::TextButton resetCompressor { "Defaults" };

    juce::Rectangle<int> engineArea, toneArea, reverbArea, compressorArea;
};

} // namespace pf::app
