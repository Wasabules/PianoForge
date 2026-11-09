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
/// a background thread writes the FIFO to a 32-bit float WAV file. The recording's first and last sample positions
/// in the device session are kept: the MIDI captured by the audio host over the same span lines up with the WAV to
/// the sample (MIDI tick 0 = WAV sample 0).
///
/// - Armed at the first note (start(..., atFirstNote)): the recording waits for the first note that reaches the
///   engine (the audio host tells it, noteOn()), and starts kPrerollSeconds before it: no silence recorded before
///   the music, its first attack whole.
/// - The writer keeps the last kHoldSeconds before writing them: a recording can end before where it was stopped
///   (stop(at)), the notes of a gesture that stopped it left out.
class LiveRecorder {
public:
    static constexpr double kPrerollSeconds = 0.25, kHoldSeconds = 0.5;

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
    /// A note reached the engine at `position` (device session): an armed recording starts with it.
    void noteOn(std::int64_t position) noexcept;
    /// The device stops: a recording in progress ends with the last block pushed.
    void deviceStopped() noexcept;

    // Message thread.
    /// Starts a recording with the next audio block, or at the first note (atFirstNote) from `notesFrom` on (a device
    /// session position: the notes of a gesture that armed it, still on their way to the engine, do not start it).
    /// `wav` empty: positions only (MIDI recording).
    juce::Result start(const juce::File& wav, double sampleRate, std::uint32_t segment, const juce::String& description,
                       bool atFirstNote = false, std::int64_t notesFrom = -1);
    /// Ends the recording (at `at`, a device session position, if it is earlier than where it is now: kHoldSeconds
    /// back at most), finishes the WAV file and returns the span recorded. Armed and no note yet: nothing recorded.
    Span stop(std::int64_t at = -1);
    bool recording() const noexcept { return armed; }
    /// Armed, waiting for the first note.
    bool waiting() const noexcept { return armed && trigger.load(std::memory_order_acquire) < 0; }
    /// The device stopped while recording: stop() should be called.
    bool endedByDevice() const noexcept { return state.load() == kStopped && armed; }
    /// stop() is waiting for the audio thread (tests).
    bool stopPending() const noexcept { return state.load() == kStopRequested; }
    /// Seconds recorded so far (0 while waiting for the first note).
    double seconds() const noexcept;
    std::int64_t droppedSamples() const noexcept { return dropped.load(); }

private:
    static constexpr int kIdle = 0, kRequested = 1, kRecording = 2, kStopRequested = 3, kStopped = 4;
    static constexpr double kFifoSeconds = 10.0;

    void writerLoop();
    /// The writer: discard or write what is ready, up to what may be written now (`final`: everything up to the end).
    void drain(bool final);
    /// Where the recording starts: the first note less the pre-roll, not before the first block pushed.
    std::int64_t beginning() const noexcept;

    std::atomic<int> state { kIdle };
    std::atomic<std::int64_t> firstSample { 0 }, endSample { 0 }, pushed { 0 }, dropped { 0 };
    std::atomic<std::int64_t> trigger { -1 }; // the first note's position (-1: none yet), or the first block's
    std::atomic<std::int64_t> endAt { -1 };   // an end asked for before the last block (-1: none)
    std::atomic<std::int64_t> notesFrom { -1 }; // armed: the notes before this position do not start it
    bool withAudio = false;                   // set before arming, read by the audio thread after it
    bool atFirst = false;
    std::int64_t preroll = 0, hold = 0;
    int channels = 2;
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> buffer;          // the FIFO's storage
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::thread writerThread;
    std::atomic<bool> writing { false };
    std::atomic<bool> writeFailed { false };
    std::int64_t front = 0;                   // writer thread: the position of the FIFO's next frame
    std::int64_t written = 0;                 // writer thread: frames written to the file

    // Message thread.
    bool armed = false;
    double rate = 0.0;
    std::uint32_t segment = 0;
    juce::File wavFile;
};

} // namespace pf::app
