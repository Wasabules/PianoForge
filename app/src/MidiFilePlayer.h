#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace pf::app {

/// Plays a standard MIDI file into the engine, from the audio thread, to the sample: on every block, the events
/// that fall in it are added to the block's MIDI with their exact positions. Transport from the interface (play,
/// pause, stop, seek, speed, loop) through atomics; the file is parsed on the message thread and handed over
/// through a published pointer (the message thread frees the previous file once the audio thread is outside
/// render(), and render() never touches a file it has not been handed).
///
/// A seek, a pause or a stop releases the notes the file has sounding (and its sustain and sostenuto pedals); a
/// seek then puts the sustain pedal back as the file has it at the new position (the sostenuto would catch
/// nothing: the notes it held are not struck again). Notes on, controllers (the pedals) and notes off are
/// played, on any channel but the General MIDI percussion one (10) when others have notes; the rest is left out.
class MidiFilePlayer {
public:
    struct Event {
        double seconds = 0.0;
        std::uint8_t status = 0, data1 = 0, data2 = 0;
    };
    struct Sequence {
        juce::String name, path;
        std::vector<Event> events; // ascending time
        double length = 0.0;       // seconds, up to the last event
        int notes = 0, tracks = 0;
        bool drumsLeftOut = false; // General MIDI percussion (channel 10), when other channels have notes
    };

    /// Message thread: reads a MIDI file (nullptr and `error` filled when it cannot).
    static std::shared_ptr<const Sequence> read(const juce::File& file, juce::String& error);
    /// Message thread: makes `sequence` the one played (stopped, at its start; nullptr: none). The notes of the
    /// previous file are released on the next block.
    void load(std::shared_ptr<const Sequence> sequence);
    std::shared_ptr<const Sequence> sequence() const { return owned; }

    // Message thread, any time.
    void play()
    {
        if (owned == nullptr)
            return;
        if (positionSeconds.load() >= owned->length) // at the end: from the start again
            seek(0.0);
        wantPlaying = true;
    }
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
    void setSpeed(double factor) { speed = juce::jlimit(0.25, 4.0, factor); }
    void setLoop(bool on) { loop = on; }
    bool isLooping() const noexcept { return loop.load(); }
    double getSpeed() const noexcept { return speed.load(); }
    bool isPlaying() const noexcept { return playing.load(); }
    bool wantsToPlay() const noexcept { return wantPlaying.load(); }
    double position() const noexcept { return positionSeconds.load(); }

    /// Audio thread: adds the block's events to `midi` (positions 0 to numSamples - 1). No allocation (the buffer
    /// has room), no lock.
    void render(juce::MidiBuffer& midi, int numSamples, double sampleRate) noexcept;

private:
    void renderBlock(juce::MidiBuffer& midi, int numSamples, double sampleRate) noexcept;
    void releaseAll(juce::MidiBuffer& midi, int at) noexcept;
    void restorePedals(juce::MidiBuffer& midi, int at) noexcept;
    void note(const Event& e) noexcept;

    std::shared_ptr<const Sequence> owned;                  // message thread
    std::atomic<const Sequence*> published { nullptr };
    std::atomic<bool> rendering { false };                  // the audio thread is inside render()
    std::atomic<bool> wantPlaying { false }, playing { false }, loop { false };
    std::atomic<double> speed { 1.0 }, seekTarget { 0.0 }, positionSeconds { 0.0 };
    std::atomic<std::uint32_t> seekSerial { 0 };

    // Audio thread.
    const Sequence* current = nullptr;
    std::size_t next = 0;
    double pos = 0.0;
    std::uint32_t seekSeen = 0;
    std::array<std::uint8_t, 16 * 128> sounding {}; // notes on, per channel and key
    std::array<std::uint8_t, 16> pedal {};          // CC64 per channel
    std::array<std::uint8_t, 16> sostenuto {};      // CC66 per channel
};

} // namespace pf::app
