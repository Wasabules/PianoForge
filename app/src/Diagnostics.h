#pragma once

#include <juce_core/juce_core.h>

namespace pf::app {

/// Headless MIDI diagnostic:
///   PianoForge --midi-monitor <seconds> <report file> [<folder of one corpus pass>]
/// Lists the MIDI inputs as JUCE sees them, records every raw message, and replays the messages through the
/// application's own path (velocity curve, collector, audio host, reference player) with simulated audio
/// blocks, logging how many voices actually sound.
int runMidiMonitor(const juce::StringArray& arguments);

/// Lists the audio driver types (WASAPI, ASIO, DirectSound...) and their output devices:
///   PianoForge --list-audio <report file>
int runListAudio(const juce::StringArray& arguments);

} // namespace pf::app
