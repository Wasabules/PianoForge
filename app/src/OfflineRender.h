#pragma once

#include "SampleBank.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <memory>

namespace pf::app {

/// Renders a MIDI sequence (timestamps in seconds) through a reference player on `bank`, at `sampleRate`, exactly
/// as the listening lab plays it (same engine, same settings), followed by a tail.
juce::AudioBuffer<float> renderSequence(const juce::MidiMessageSequence& sequence, std::shared_ptr<const SampleBank> bank,
                                        double sampleRate, float restrikeFadeSeconds, bool playWholeTake,
                                        double tailSeconds = 4.0);

/// Writes a 32-bit float WAV file (creating the folder when needed).
juce::Result writeWav(const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file);

/// Renders a standard MIDI file through the reference player:
///   PianoForge --render <file.mid> <out.wav> <folder of one corpus pass> [restrike fade in s]
/// Output at the corpus sample rate, with a 4 s tail after the last event.
int runOfflineRender(const juce::StringArray& arguments);

} // namespace pf::app
