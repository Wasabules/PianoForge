// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace pf::app {

/// The application's audio devices: JUCE's, and on Linux:
/// - first, PipeWire's own (PipeWireAudio.h), where PipeWire runs: the default for a first start;
/// - JUCE's ALSA and JACK types, each device of theirs that plays through PipeWire (ALSA's "PipeWire" and "Default
///   (currently PipeWire Media Server)" devices; JACK when it is PipeWire's) opened with the block asked of PipeWire's
///   graph (linuxaudio::quantumProps), when pipewire::forceQuantum() says so. JUCE's JACK device has no choice of
///   block (a JACK server has one for all): through PipeWire it gets one, its client made again with it.
/// Their names stay JUCE's ("ALSA", "JACK"): the devices saved keep opening.
class DeviceManager final : public juce::AudioDeviceManager {
protected:
    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override;
};

/// Whether `device` plays through PipeWire (its own type, or ALSA's or JACK's PipeWire devices).
bool throughPipeWire(juce::AudioIODevice* device);

} // namespace pf::app
