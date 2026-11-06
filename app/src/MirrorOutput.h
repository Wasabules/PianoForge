#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <vector>

namespace pf::app {

/// Carries a stereo stream from one audio clock to another: a lock-free FIFO filled by the producer (the main
/// device's callback), drained by the consumer (another device's callback) through a windowed-sinc interpolator
/// (16 taps, Kaiser window) whose ratio follows the two clocks. Two devices never run at exactly the same rate
/// (tens of ppm apart): a fixed ratio would empty or overflow the FIFO within minutes. The ratio is the nominal
/// one, corrected by a slow PI loop on the FIFO's fill level averaged over a second (the blocks of both sides make
/// it a sawtooth), at most +-0.3 %, a few ppm once settled: inaudible.
///
/// push() is lock-free and allocation-free (producer thread); pull() too (consumer thread). prepare() allocates
/// and must not run while either side is in use.
class DriftResampler {
public:
    void prepare(double inputRate, double outputRate, int maxInputBlock, int maxOutputBlock);
    void push(const float* const* input, int numChannels, int numFrames) noexcept;
    void pull(float* const* output, int numChannels, int numFrames) noexcept;

    double targetSeconds() const noexcept { return inRate > 0.0 ? target / inRate : 0.0; }
    double fillSeconds() const noexcept { return inRate > 0.0 ? fillAverage.load() / inRate : 0.0; }
    double correctionPpm() const noexcept { return 1e6 * correctionValue.load(); }
    std::uint32_t underruns() const noexcept { return underrunCount.load(); }
    std::uint32_t overruns() const noexcept { return overrunCount.load(); }
    bool running() const noexcept { return playing.load(); }

private:
    static constexpr double kMaxCorrection = 0.003;
    static constexpr int kHalfTaps = 8, kPhases = 512; // the interpolator: 16 taps, 512 phases (linear between)
    double inRate = 0.0, outRate = 0.0, baseRatio = 1.0, target = 0.0;
    std::vector<float> kernel;               // (kPhases + 1) x 2 kHalfTaps
    std::vector<double> fills;               // the fill level at the last callbacks (a second of them)
    std::size_t fillAt = 0, fillCount = 0;
    double fillSum = 0.0;
    juce::AbstractFifo fifo { 1 };
    std::vector<float> fifoData;             // interleaved stereo, the FIFO's storage
    std::vector<float> pending;              // consumer side: interleaved stereo, frames not yet consumed
    int pendingFrames = 0;
    double readPos = 0.0;                    // in pending frames (kHalfTaps frames of history before it)
    double integral = 0.0, fillSmoothed = 0.0;
    std::atomic<double> fillAverage { 0.0 }, correctionValue { 0.0 };
    std::atomic<bool> playing { false };
    std::atomic<std::uint32_t> underrunCount { 0 }, overrunCount { 0 };
};

/// A second output, for streaming software (OBS): a copy of what the main device plays, sent to another device
/// through the system's shared mixer (WASAPI shared on Windows, in its low-latency mode when the device has it;
/// ALSA on Linux), typically a virtual cable (VB-Audio Hi-Fi Cable: OBS takes its output as an audio input; both
/// ends of the cable must be set to the same sample rate in Windows). The main device (ASIO) is not touched: its
/// callback only copies its block into a lock-free FIFO; the mirror device runs on its own thread, and its
/// latency (a few tens of ms) is its own.
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
    DriftResampler resampler;
    std::atomic<bool> accepting { false };
    std::atomic<int> inPush { 0 };
    std::atomic<double> mainRate { 0.0 };
    std::atomic<int> mainBlock { 0 };
    double openedRate = 0.0;
    int openedBlock = 0;
};

} // namespace pf::app
