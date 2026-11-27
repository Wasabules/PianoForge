// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pf::app::linuxaudio {

/// What PianoForge does on Linux for its sound, beyond JUCE: PipeWire's JACK, PipeWire's block asked for by the ALSA
/// and JACK devices that play through it, a realtime audio thread, and PipeWire's clock read for advice. (PipeWire's
/// own device type is in PipeWireAudio.h.) Every function here is a no-op, or answers "none", on other systems.

/// Once, before the first audio device type is made (JUCE loads libjack then, once):
/// - where PipeWire runs and its JACK library is installed (Debian and Ubuntu: the package pipewire-jack; Fedora and
///   Arch install it as the system's libjack), that library is loaded first, so that JUCE's JACK device type plays
///   through PipeWire as pw-jack would make it; PIANOFORGE_JACK_LIBRARY names another one (a JACK server's);
/// - PIPEWIRE_ALSA and PIPEWIRE_PROPS, unless the user set them, are made empty: ScopedProps fills them around a
///   device's opening only (changing a variable that exists is safe while other threads read the environment, adding
///   one is not).
/// Returns what was done for JACK, in a few words ("PipeWire's JACK"), or an empty string.
std::string prepare();

/// What prepare() did for JACK; whether JACK is PipeWire's.
const std::string& jackSetup();
bool pipewireJack();

/// Whether PipeWire's daemon runs (its socket is there).
bool pipewireRunning();

/// PipeWire's properties asking its graph for `block` samples while the node lives: node.force-quantum (which, unlike
/// node.latency, goes under the graph's clock.min-quantum: measured with PipeWire 1.6, 1024 -> 64), and node.latency.
std::string quantumProps(int block, double rate);

/// `variable` (PIPEWIRE_ALSA: read by PipeWire's ALSA plug-in when a device opens; PIPEWIRE_PROPS: by every PipeWire
/// client, JACK's when its client opens) set to `value` for the object's lifetime, then empty again. Nothing when the
/// user set the variable: theirs prevails.
class ScopedProps {
public:
    ScopedProps(const char* variable, const std::string& value);
    ~ScopedProps();
    ScopedProps(const ScopedProps&) = delete;
    ScopedProps& operator=(const ScopedProps&) = delete;

private:
    const char* variable;
};

/// PipeWire's clock, as its settings say (pw-metadata -n settings): the graph never runs at blocks shorter than
/// minQuantum unless a node forces one (node.force-quantum), or a tool the whole graph (clock.force-quantum).
/// Through ALSA, JUCE's buffer holds four of its blocks, and must hold two of the graph's: a block under half the
/// graph's quantum underruns.
struct PipeWireClock {
    bool running = false; // PipeWire runs (its socket is there)
    int rate = 0, minQuantum = 0, maxQuantum = 0, forceQuantum = 0, forceRate = 0;
    std::vector<int> allowedRates; // clock.allowed-rates: the rates the graph may switch to
    /// The shortest block the graph runs at for a client that does not force one, 0 if unknown.
    int floorQuantum() const noexcept { return forceQuantum > 0 ? forceQuantum : minQuantum; }
};
/// Message thread (it runs pw-metadata: a few milliseconds, at most one second).
PipeWireClock pipewireClock();

/// The calling thread's kernel id: the audio thread notes its own on every callback (a thread-local read).
std::int64_t threadId() noexcept;

struct Scheduling {
    bool realtime = false;
    int priority = 0;
    std::string how; // "realtime, priority 20 (rtkit)"; or why it is not
};

/// The scheduling of thread `tid` of this process.
Scheduling scheduling(std::int64_t tid);

/// Message thread: a realtime priority for thread `tid` (the audio thread, which JUCE's ALSA device runs at a normal
/// priority): directly when the user's limits allow it (RLIMIT_RTPRIO, the audio group's), else through rtkit (the
/// D-Bus service desktops run for PipeWire and PulseAudio), which then requires RLIMIT_RTTIME: it is set to rtkit's
/// maximum (200 ms of CPU without blocking, as PipeWire's clients do; an audio thread blocks at every block). A
/// thread already realtime (JACK's, PipeWire's) is left as it is.
Scheduling makeRealtime(std::int64_t tid);

} // namespace pf::app::linuxaudio
