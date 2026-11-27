// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace pf::app {

/// The sound formats a recording can be saved in. The live recording itself is always a 32-bit float WAV (the
/// master: nothing but a copy on the audio thread, nothing encoded there); the others are made from it once the
/// recording stops, in the background.
enum class SoundFormat { wavFloat, wav24, flac, mp3, opus };

struct SoundFormatInfo {
    SoundFormat format;
    const char* key;       // in the settings
    const char* menuName;  // in File > Record formats
    const char* extension; // with its dot
    const char* shortName; // in messages
};

const std::vector<SoundFormatInfo>& soundFormats();
const SoundFormatInfo& infoFor(SoundFormat format);
/// Whether this system can write it (MP3: Media Foundation on Windows, LAME elsewhere when installed).
bool canWrite(SoundFormat format);

/// Converts a WAV file into `target` in `format`. `progress` (0 to 1) and `cancel` may be null. A failure or a
/// cancellation removes the unfinished target. Any thread; blocking.
///   WAV 24-bit, FLAC 24-bit: the samples rounded to 24 bits (FLAC is lossless: the same samples as the WAV 24).
///   MP3: 320 kbit/s, constant; above 48 kHz the sound is first brought down by 2, 4 or 8 to 44.1 or 48 kHz.
///   Opus: 192 kbit/s (Opus works at 48 kHz: it resamples what it is given).
juce::Result convertSound(const juce::File& source, const juce::File& target, SoundFormat format,
                          std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);

/// The conversions of finished recordings, one after the other on a background thread.
class RecordingExporter {
public:
    struct Job {
        juce::File master;                                   // the live recording (32-bit float WAV)
        bool removeMaster = false;                           // not one of the formats asked for
        std::vector<std::pair<SoundFormat, juce::File>> targets;
        juce::String label;                                  // for the messages (the recording's name)
    };
    struct Report {
        juce::String text;
        bool ok = true;
    };

    RecordingExporter() = default;
    /// Cancels the conversion in progress and waits for it: its unfinished file is removed, and its master, like
    /// those of the jobs not started, is kept (under the recording's name when it was to be removed).
    ~RecordingExporter();

    // Message thread.
    void submit(Job job);
    bool busy() const;
    /// What is being converted, with its progress ("FLAC 40 %"), or empty.
    juce::String progressText() const;
    /// The reports of the jobs finished since the last call.
    std::vector<Report> takeReports();

private:
    void run();
    void convert(Job& job);

    mutable std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Report> reports;
    juce::String current;     // guarded by `lock`
    bool working = false;     // guarded by `lock`
    bool stopping = false;    // guarded by `lock`
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancel { false };
    std::thread worker;
};

} // namespace pf::app
