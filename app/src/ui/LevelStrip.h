#pragma once

#include <pianoforge/midi_sequence.hpp>

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pf::app {

/// Under the MIDI player's position: the level along the piece, to see at a glance where it is loud. From the sound
/// of the MIDI file when one sits next to it (the same name: WAV, FLAC, MP3, Opus; a recording's, a take's render),
/// read in the background; else estimated from the notes' velocities. The loudness (RMS over 50 ms) as a filled
/// curve coloured from quiet to loud, the peaks as a line, what has been played brighter; a click moves there.
class LevelStrip final : public juce::Component, public juce::SettableTooltipClient, private juce::AsyncUpdater {
public:
    /// Where a time (s) falls, in this component's x (the player's position slider: the same mapping).
    std::function<float(double)> xOf;
    std::function<void(double)> onSeek;

    LevelStrip() = default;
    ~LevelStrip() override;

    /// The file played (its sound found and read in the background); nullptr: none.
    void load(const juce::File& midi, std::shared_ptr<const pf::MidiSequence> sequence);
    void setPosition(double seconds);
    /// The sound file found next to a MIDI file (the best one), or none.
    static juce::File soundOf(const juce::File& midi);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

    struct Envelope {
        std::vector<float> rms, peak; // per bin, linear
        double binSeconds = 0.05;
        float loudest = 0.0f;         // the largest peak (the scale's top)
        juce::String source;          // what it was made from
        bool fromMidi = false;
    };
    /// Built from a sound file (cancellable: empty when cancelled or unreadable), or from the notes.
    static std::shared_ptr<Envelope> fromSound(const juce::File& sound, const std::atomic<bool>& cancel);
    static std::shared_ptr<Envelope> fromNotes(const pf::MidiSequence& sequence);

private:
    void handleAsyncUpdate() override;
    void stopWorker();
    double timeAt(float x) const;

    std::shared_ptr<const Envelope> envelope; // message thread
    double length = 0.0, position = 0.0;
    bool reading = false;
    std::thread worker;
    std::atomic<bool> cancel { false };
    std::mutex lock;
    std::shared_ptr<Envelope> ready; // from the worker, guarded by `lock`
};

} // namespace pf::app
