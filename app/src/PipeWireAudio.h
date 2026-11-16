#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace pf::app::pipewire {

/// PipeWire's own audio interface, as a JUCE device type, "PipeWire": the sound goes straight into PipeWire's graph,
/// rendered in PipeWire's realtime thread at the graph's pace (as JACK would), with neither the ALSA plug-in's ring of
/// four blocks in between nor a JACK library to install. The block chosen is asked of the graph (node.force-quantum,
/// while PianoForge plays: PipeWire's clock.min-quantum does not hold it back), the rate too (node.rate: the graph
/// switches to it when nothing else plays, else PipeWire converts), and the latency reported is PipeWire's own measure
/// of the way to the device.
///
/// libpipewire is loaded at run time (nothing linked): where it is missing, older than 0.3.64 (2023: Debian 12, Ubuntu
/// 23.04) or its daemon is not running, there is no such type, and ALSA and JACK remain.
///
/// The outputs are the system's default (following it when it changes) and each sink by name; a sink that goes away
/// hands the sound to the default one, as PipeWire does for every stream.

/// Whether a device asks PipeWire's graph to run at its block (on by default). Read when a device opens.
std::atomic<bool>& forceQuantum();

/// The type, or nullptr where PipeWire cannot be used. `mainOutput`: PianoForge's main output (it forces the block);
/// otherwise a secondary one (the mirror), which follows the graph.
std::unique_ptr<juce::AudioIODeviceType> createDeviceType(bool mainOutput = true);

/// The name of the type ("PipeWire"), and of its device that follows the system's default output.
inline constexpr const char* kTypeName = "PipeWire";
inline constexpr const char* kDefaultOutput = "Default output";

/// What a PipeWire device is doing, as measured on its last cycles.
struct Status {
    int block = 0;           // the block asked for (the callbacks' size)
    int graphBlock = 0;      // the graph's quantum, at the graph's rate
    double graphRate = 0.0;  // the graph's rate (it converts when the device's differs)
    double latencyMs = 0.0;  // from the sample written to the device's output, PipeWire's measure
    bool forced = false;     // the block was asked with node.force-quantum
    std::uint32_t xruns = 0; // the graph's xruns while this device played
};
/// The status of `device` when it is a PipeWire device.
std::optional<Status> status(juce::AudioIODevice* device);

/// The output PipeWire plays `device` on: a PipeWire device's own sink, else the system's default one (where ALSA's
/// and JACK's PipeWire devices go). Not known where PipeWire does not run.
struct Output {
    bool known = false;
    std::string name, description;
    bool bluetooth = false; // a Bluetooth headset or speaker: 100 to 250 ms of its own, whatever the block
};
Output output(juce::AudioIODevice* device);

} // namespace pf::app::pipewire
