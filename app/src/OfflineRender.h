#pragma once

#include <juce_core/juce_core.h>

namespace pf::app {

/// Renders a standard MIDI file through the reference player, exactly as the listening lab plays it:
///   PianoForge --render <file.mid> <out.wav> <folder of one corpus pass> [restrike fade in s]
/// Output: 32-bit float WAV at the corpus sample rate, with a 4 s tail after the last event.
int runOfflineRender(const juce::StringArray& arguments);

} // namespace pf::app
