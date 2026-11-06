#pragma once

#include <pianoforge/engine.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pf {

/// A MIDI file's performance, as the player plays it: notes and controllers (the pedals) in seconds.
struct MidiSequence {
    struct Event {
        double seconds = 0.0;
        std::uint8_t status = 0, data1 = 0, data2 = 0;
    };
    std::string name, path;    // set by the caller (a file name, a path)
    std::vector<Event> events; // ascending time; equal times keep the file's order (track, then position)
    double length = 0.0;       // seconds, up to the last event
    int notes = 0, tracks = 0;
    bool drumsLeftOut = false; // General MIDI percussion (channel 10), left out because other channels have notes
};

/// Reads a Standard MIDI File (formats 0, 1 and 2, any number of tracks, tempo changes, running status) from its
/// bytes. Keeps the notes and the controllers of every channel, but the General MIDI percussion (channel 10) when
/// other channels have notes: it would sound as piano notes. SMPTE time is refused. Returns nullptr and fills
/// `error` when the bytes are not a MIDI file, or hold no note.
std::shared_ptr<MidiSequence> readMidiFile(const std::uint8_t* data, std::size_t size, std::string& error);

/// Plays a MidiSequence into an engine, to the sample: on every block, the events that fall in it come out with
/// their offsets. Transport (play, pause, stop, seek, speed, loop) from any thread through atomics; the sequence is
/// handed over through a published pointer, the previous one freed once the audio thread is out of render().
///
/// A seek, a pause or a stop releases the notes the sequence has sounding, and its sustain and sostenuto pedals;
/// playing on (or resuming) puts the sustain pedal back as the sequence has it there (the sostenuto would catch
/// nothing: the notes it held are not struck again). Shared by the lab (app/) and the browser module (web/).
class SequencePlayer {
public:
    /// Not the audio thread: makes `sequence` the one played (stopped, at its start; nullptr: none).
    void load(std::shared_ptr<const MidiSequence> sequence);
    std::shared_ptr<const MidiSequence> sequence() const { return owned; }

    void play();
    void pause() { wantPlaying = false; }
    void stop()
    {
        wantPlaying = false;
        seek(0.0);
    }
    void seek(double seconds)
    {
        seekTarget = seconds;
        seekSerial.fetch_add(1);
    }
    void setSpeed(double factor);
    void setLoop(bool on) { loop = on; }
    bool isLooping() const noexcept { return loop.load(); }
    double getSpeed() const noexcept { return speed.load(); }
    bool isPlaying() const noexcept { return playing.load(); }
    bool wantsToPlay() const noexcept { return wantPlaying.load(); }
    /// The position in the sequence's time (seconds of the file, whatever the speed).
    double position() const noexcept { return positionSeconds.load(); }

    /// Audio thread: writes the block's events (offsets 0 to numSamples - 1, ascending) into `out`, at most
    /// `capacity`; returns how many. No allocation, no lock.
    int render(Event* out, int capacity, int numSamples, double sampleRate) noexcept;
    /// Audio thread (for a display): how many of the sequence's notes sound on this key, every channel together.
    int sounding(int key) const noexcept;

private:
    void renderBlock(int numSamples, double sampleRate) noexcept;
    void emit(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset) noexcept;
    void releaseAll(int at) noexcept;
    void restorePedals(int at) noexcept;
    void note(const MidiSequence::Event& e) noexcept;

    std::shared_ptr<const MidiSequence> owned;
    std::atomic<const MidiSequence*> published { nullptr };
    std::atomic<bool> rendering { false };
    std::atomic<bool> wantPlaying { false }, playing { false }, loop { false };
    std::atomic<double> speed { 1.0 }, seekTarget { 0.0 }, positionSeconds { 0.0 };
    std::atomic<std::uint32_t> seekSerial { 0 };

    // Audio thread.
    const MidiSequence* current = nullptr;
    std::size_t next = 0;
    double pos = 0.0;
    std::uint32_t seekSeen = 0;
    std::array<std::uint8_t, 16 * 128> notesOn {}; // per channel and key
    std::array<std::uint8_t, 16> pedal {}, sostenuto {}; // CC64, CC66 per channel
    Event* out = nullptr;
    int outCapacity = 0, outCount = 0;
};

} // namespace pf
