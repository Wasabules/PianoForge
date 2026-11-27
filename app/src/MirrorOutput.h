// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/drift_resampler.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <vector>

namespace pf::app {

/// A stereo stream from one audio clock to another within the application: a lock-free FIFO filled by the
/// producer's callback, drained by the consumer's through pf::DriftResampler (which follows the two clocks).
/// push() and pull() are lock-free and allocation-free; prepare() allocates and must not run while they may.
class DriftFifo {
public:
    void prepare(double inputRate, double outputRate, int maxInputBlock, int maxOutputBlock);
    void push(const float* const* input, int numChannels, int numFrames) noexcept;
    void pull(float* const* output, int numChannels, int numFrames) noexcept;

    double targetSeconds() const noexcept { return resampler.targetSeconds(); }
    double fillSeconds() const noexcept { return resampler.fillSeconds(); }
    double correctionPpm() const noexcept { return resampler.correctionPpm(); }
    std::uint32_t underruns() const noexcept { return resampler.underruns(); }
    std::uint32_t overruns() const noexcept { return resampler.overruns() + dropped.load(); }
    bool running() const noexcept { return resampler.running(); }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> fifoData; // interleaved stereo, the FIFO's storage
    pf::DriftResampler resampler;
    std::atomic<std::uint32_t> dropped { 0 };
};

/// A second output, for streaming software: a copy of what the main device plays, sent to another device through
/// the system's shared mixer (WASAPI shared on Windows, in its low-latency mode when the device has it; ALSA on
/// Linux), typically a virtual cable (both ends of the cable set to the same sample rate in Windows). The OBS
/// plugin (obs/) needs none of this: it reads the application's audio feed directly. The main device (ASIO) is not
/// touched: its callback only copies its block into a lock-free FIFO; the mirror device runs on its own thread,
/// and its latency is its own.
class MirrorOutput final : private juce::AudioIODeviceCallback {
public:
    ~MirrorOutput() override { close(); }

    /// Output devices the mirror can open (message thread; scans the system).
    static juce::StringArray deviceNames();

    /// Message thread: opens `deviceName` for a stream at the main device's rate and block size.
    juce::Result open(const juce::String& deviceName, double inputRate, int inputBlock);
    void close();
    bool isOpen() const noexcept { return device != nullptr; }
    juce::String currentDevice() const { return device != nullptr ? device->getName() : juce::String(); }
    juce::String status() const;

    /// Main audio thread, after the engine filled the block. Lock-free.
    void push(const float* const* outputs, int numChannels, int numSamples) noexcept;
    /// The main device (re)started: its rate and block size (the mirror must be opened again when they change).
    void mainFormat(double rate, int block) noexcept
    {
        mainRate = rate;
        mainBlock = block;
    }
    bool needsReopen() const noexcept { return device != nullptr && (mainRate.load() != openedRate || mainBlock.load() > openedBlock); }
    double mainSampleRate() const noexcept { return mainRate.load(); }
    int mainBlockSize() const noexcept { return mainBlock.load(); }

private:
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    static std::unique_ptr<juce::AudioIODeviceType> createType(bool lowLatency);

    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    DriftFifo stream;
    std::atomic<bool> accepting { false };
    std::atomic<int> inPush { 0 };
    std::atomic<double> mainRate { 0.0 };
    std::atomic<int> mainBlock { 0 };
    double openedRate = 0.0;
    int openedBlock = 0;
};

} // namespace pf::app
