// SPDX-License-Identifier: AGPL-3.0-or-later

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
/// --check-update <address> [report]: what the address offers (a manifest or a GitHub release) against this version, then the
/// installer downloaded to the temporary folder and checked (size, SHA-256), never run. 0 if all of it worked.
int runCheckUpdate(const juce::StringArray& arguments);
int runListAudio(const juce::StringArray& arguments);
/// --audio-restore <report>: the saved audio device reopened as at start, and what JUCE says.
int runAudioRestore(const juce::StringArray& arguments);
/// --audio-hold <seconds>: holds the saved audio device that long (to test a busy driver at start).
int runAudioHold(const juce::StringArray& arguments);
/// An output opened as the application opens it, measured while it plays silence:
///   PianoForge --audio-check [--type <PipeWire|ALSA|JACK...>] [--device <name>] [--rate <Hz>] [--block <samples>]
///                            [--seconds <s>] [--force | --no-force] [<report file>]
/// (without --type: the saved device). Reports the device, its rate and block, the callbacks' blocks and the longest
/// wait between them, the latency it reports, its xruns, its thread's scheduling (made realtime as the application
/// does), and on PipeWire its graph's block, rate and xruns. 0 if it opened and played.
int runAudioCheck(const juce::StringArray& arguments);
/// What the desktop offers PianoForge (Settings > Desktop), each tried:
///   PianoForge --desktop-check [--hold <seconds>] [--notify] [<report file>]
/// the screen kept on and the performance profile held for a few seconds (to be seen from outside), the media
/// controls registered, a notification (--notify), a virtual MIDI input, Flathub's OBS as the plugin needs it, and the
/// output PipeWire plays on (Bluetooth or not).
int runDesktopCheck(const juce::StringArray& arguments);

/// End-to-end check of the stream output through a virtual cable, without the main device: a quiet 1 kHz tone
/// (-40 dBFS) produced at 44.1 kHz in 128-sample blocks goes out through the mirror to <playback device>, and is
/// captured back from <capture device> (the cable's other end, as OBS would take it):
///   PianoForge --mirror-check "<playback device>" "<capture device>" <report file>
int runMirrorCheck(const juce::StringArray& arguments);

/// Pictures of the secondary windows' panels, for checking their layout without opening them (no audio device
/// is opened):   PianoForge --ui-snapshots <folder>
int runUiSnapshots(const juce::StringArray& arguments);

} // namespace pf::app
