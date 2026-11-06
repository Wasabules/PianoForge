#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace pf::app {

/// Live recording of what the audio device plays, started and stopped from the user interface.
///
/// The audio path is left as it is: no buffering, no added latency. On the audio thread, push() only copies the
/// block that was just sent to the device into a pre-allocated lock-free FIFO (no lock, no allocation, no I/O);
/// a background thread writes the FIFO to a 32-bit float WAV file. The recording starts and ends on block
/// boundaries, and its first and last sample positions in the device session are kept: the MIDI captured by the
/// audio host over the same span lines up with the WAV to the sample (MIDI tick 0 = WAV sample 0).
class LiveRecorder {
public:
    struct Span {
        bool valid = false;
        double sampleRate = 0.0;
        std::uint32_t segment = 0;         // device session (see AudioHost)
        std::int64_t start = 0, end = 0;   // positions in the device session, end excluded
        std::int64_t dropped = 0;          // samples the writer could not keep up with (0 in practice)
        juce::File wav;                    // empty when only the MIDI is recorded
        juce::String error;
    };

    LiveRecorder() = default;
    ~LiveRecorder();

    // Audio thread.
    /// After the engine filled `outputs` for the block that starts at `blockStart` (device session position).
    void push(const float* const* outputs, int numChannels, int numSamples, std::int64_t blockStart) noexcept;
    /// The device stops: a recording in progress ends with the last block pushed.
    void deviceStopped() noexcept;

    // Message thread.
    /// Arms a recording; it starts with the next audio block. `wav` empty: positions only (MIDI recording).
    juce::Result start(const juce::File& wav, double sampleRate, std::uint32_t segment, const juce::String& description);
    /// Ends the recording at the next block boundary, finishes the WAV file and returns the span recorded.
    Span stop();
    bool recording() const noexcept { return armed; }
    /// The device stopped while recording: stop() should be called.
    bool endedByDevice() const noexcept { return state.load() == kStopped && armed; }
    /// stop() is waiting for the audio thread (tests).
    bool stopPending() const noexcept { return state.load() == kStopRequested; }
    double seconds() const noexcept { return rate > 0.0 ? (double) pushed.load() / rate : 0.0; }
    std::int64_t droppedSamples() const noexcept { return dropped.load(); }

private:
    static constexpr int kIdle = 0, kRequested = 1, kRecording = 2, kStopRequested = 3, kStopped = 4;
    static constexpr double kFifoSeconds = 10.0;

    void writerLoop();
    void drain();

    std::atomic<int> state { kIdle };
    std::atomic<std::int64_t> startSample { 0 }, endSample { 0 }, pushed { 0 }, dropped { 0 };
    bool withAudio = false;                   // set before arming, read by the audio thread after it
    int channels = 2;
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> buffer;          // the FIFO's storage
    juce::AudioBuffer<float> chunk;           // writer thread
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::thread writerThread;
    std::atomic<bool> writing { false };
    std::atomic<bool> writeFailed { false };

    // Message thread.
    bool armed = false;
    double rate = 0.0;
    std::uint32_t segment = 0;
    juce::File wavFile;
};

} // namespace pf::app
