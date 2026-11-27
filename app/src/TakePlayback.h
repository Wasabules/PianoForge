// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace pf::app {

/// Plays recorded takes back through the audio host: one file, or two in A/B (both from the same instant, the one
/// heard switched with a 10 ms crossfade). Mixed into what the device plays after the recording and the streaming
/// taps: heard, not recorded again nor streamed. A file at another rate than the engine's is read at its own speed
/// (linear interpolation).
///
/// load() is for the message thread: it stops the playback, waits for the audio thread to be out of render()
/// (Dekker: both flags sequentially consistent), then replaces the audio; render() allocates nothing and takes no
/// lock.
class TakePlayback {
public:
    /// Message thread: reads `a` (and `b` for an A/B), in memory. An empty string when it worked, the error otherwise.
    juce::String load(const juce::File& a, const juce::File& b = {})
    {
        release();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        for (int which = 0; which < 2; ++which) {
            const auto& file = which == 0 ? a : b;
            auto& track = tracks[which];
            track = {};
            if (file == juce::File())
                continue;
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (reader == nullptr)
                return "cannot read " + file.getFileName();
            const auto frames = (int) std::min<juce::int64>(reader->lengthInSamples, (juce::int64) std::numeric_limits<int>::max() / 2);
            track.audio.setSize(2, frames);
            reader->read(&track.audio, 0, frames, 0, true, reader->numChannels > 1);
            if (reader->numChannels == 1)
                track.audio.copyFrom(1, 0, track.audio, 0, 0, frames);
            track.rate = reader->sampleRate;
            track.name = file.getFileNameWithoutExtension();
        }
        position = 0.0;
        side.store(0);
        mix = 0.0f; // (the audio thread is out of render(): ready is false)
        ready.store(true);
        return {};
    }

    /// Message thread: stops and frees the audio.
    void release()
    {
        playing.store(false);
        ready.store(false);
        while (rendering.load())
            std::this_thread::yield();
        for (auto& t : tracks)
            t = {};
    }

    void play() noexcept
    {
        if (ready.load() && lengthSeconds() > 0.0)
            playing.store(true);
    }
    void stop() noexcept { playing.store(false); }
    void seek(double seconds) noexcept { seekTo.store(std::max(0.0, seconds)); }
    void setSide(int s) noexcept { side.store(s != 0 && hasB() ? 1 : 0); }

    bool isPlaying() const noexcept { return playing.load(); }
    bool isLoaded() const noexcept { return ready.load(); }
    bool hasB() const noexcept { return ready.load() && tracks[1].rate > 0.0; }
    int currentSide() const noexcept { return side.load(); }
    double positionSeconds() const noexcept { return position.load(); }
    double lengthSeconds() const noexcept
    {
        double l = 0.0;
        for (const auto& t : tracks)
            if (t.rate > 0.0)
                l = std::max(l, t.audio.getNumSamples() / t.rate);
        return l;
    }
    juce::String name(int s) const { return tracks[s != 0 ? 1 : 0].name; }

    /// Audio thread: adds the playback to (left, right), n frames at `rate`.
    void render(float* left, float* right, int n, double rate) noexcept
    {
        rendering.store(true);
        if (!ready.load() || !playing.load() || rate <= 0.0 || left == nullptr || right == nullptr) {
            rendering.store(false);
            return;
        }
        if (const double s = seekTo.exchange(-1.0); s >= 0.0)
            position.store(s);
        double t = position.load();
        const double length = lengthSeconds();
        const float target = (float) side.load(), glide = (float) (1.0 - std::exp(-1.0 / (0.01 * rate)));
        for (int k = 0; k < n; ++k) {
            mix += (target - mix) * glide;
            float l = 0.0f, r = 0.0f;
            for (int s = 0; s < 2; ++s) {
                const float w = s == 0 ? 1.0f - mix : mix;
                if (w < 1e-6f || tracks[s].rate <= 0.0)
                    continue;
                const auto& audio = tracks[s].audio;
                const double x = t * tracks[s].rate;
                const int i = (int) x;
                if (i + 1 >= audio.getNumSamples())
                    continue;
                const float f = (float) (x - i);
                l += w * (audio.getSample(0, i) + f * (audio.getSample(0, i + 1) - audio.getSample(0, i)));
                r += w * (audio.getSample(1, i) + f * (audio.getSample(1, i + 1) - audio.getSample(1, i)));
            }
            left[k] += l;
            right[k] += r;
            t += 1.0 / rate;
        }
        if (t >= length) { // at the end: stopped, back to the start
            playing.store(false);
            t = 0.0;
        }
        position.store(t);
        rendering.store(false);
    }

private:
    struct Track {
        juce::AudioBuffer<float> audio;
        double rate = 0.0;
        juce::String name;
    };
    Track tracks[2];
    std::atomic<bool> ready { false }, rendering { false }, playing { false };
    std::atomic<int> side { 0 };
    std::atomic<double> position { 0.0 }, seekTo { -1.0 };
    float mix = 0.0f; // audio thread: 0 hears A, 1 hears B
};

} // namespace pf::app
