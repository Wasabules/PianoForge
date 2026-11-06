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

/// End-to-end check of the stream output through a virtual cable, without the main device: a quiet 1 kHz tone
/// (-40 dBFS) produced at 44.1 kHz in 128-sample blocks goes out through the mirror to <playback device>, and is
/// captured back from <capture device> (the cable's other end, as OBS would take it):
///   PianoForge --mirror-check "<playback device>" "<capture device>" <report file>
int runMirrorCheck(const juce::StringArray& arguments);

/// Pictures of the secondary windows' panels, for checking their layout without opening them (no audio device
/// is opened):   PianoForge --ui-snapshots <folder>
int runUiSnapshots(const juce::StringArray& arguments);

} // namespace pf::app
