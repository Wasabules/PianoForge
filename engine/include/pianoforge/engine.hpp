// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

namespace pf {

/// MIDI event timed to the sample, relative to the start of the audio block.
struct Event {
    std::uint32_t offset = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
};

/// Common interface of the sound engines: the reference player today, the modal engine tomorrow.
/// The shell (standalone app, plugin, offline renderer) only knows this interface, which makes it
/// possible to switch between engines for an A/B comparison.
///
/// prepare() and release() are called outside the audio thread and may allocate. process() is called
/// on the audio thread: it must not allocate, lock or perform any I/O.
class Engine {
public:
    virtual ~Engine() = default;

    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void release() noexcept {}

    /// Writes (does not accumulate) numSamples into every output. Events are sorted by offset.
    virtual void process(float* const* outputs, int numChannels, int numSamples,
                         const Event* events, int numEvents) noexcept = 0;

    /// Quickly fades out every voice (without clicks).
    virtual void allSoundOff() noexcept = 0;
};

} // namespace pf
