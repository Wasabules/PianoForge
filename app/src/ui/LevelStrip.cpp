// SPDX-License-Identifier: AGPL-3.0-or-later

#include "LevelStrip.h"

#include "Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace pf::app {

namespace {

constexpr double kBinSeconds = 0.05; // 50 ms per bin
constexpr int kMaxBins = 20000;      // (16 min at 50 ms; longer pieces: wider bins)
constexpr float kRangeDb = 36.0f;     // from the loudest down to the strip's bottom (a recording)
constexpr float kMidiRangeDb = 24.0f; // (the estimate from the notes: a narrower span)

} // namespace

LevelStrip::~LevelStrip()
{
    stopWorker();
    cancelPendingUpdate();
}

juce::File LevelStrip::soundOf(const juce::File& midi)
{
    const auto folder = midi.getParentDirectory();
    const auto stem = midi.getFileNameWithoutExtension();
    for (const auto* ext : { ".wav", ".flac", ".mp3", ".opus", ".master.wav" }) // the recording's own, lossless first
        if (const auto f = folder.getChildFile(stem + ext); f.existsAsFile())
            return f;
    // A take's render ("take-... (render 20261116-...).wav"): the latest.
    juce::File latest;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, stem + " (render *).wav"))
        if (latest == juce::File() || f.getLastModificationTime() > latest.getLastModificationTime())
            latest = f;
    return latest;
}

void LevelStrip::stopWorker()
{
    cancel = true;
    if (worker.joinable())
        worker.join();
    cancel = false;
}

void LevelStrip::load(const juce::File& midi, std::shared_ptr<const pf::MidiSequence> sequence)
{
    stopWorker();
    {
        std::lock_guard<std::mutex> guard(lock);
        ready.reset();
    }
    length = sequence != nullptr ? sequence->length : 0.0;
    position = 0.0;
    envelope = sequence != nullptr ? fromNotes(*sequence) : nullptr; // at once; the sound's replaces it once read
    const auto sound = sequence != nullptr ? soundOf(midi) : juce::File();
    reading = sound.existsAsFile();
    setTooltip(sound.existsAsFile() ? "The level along the piece, from " + sound.getFileName() + " (click to move there)"
                                    : "The level along the piece, estimated from the notes' velocities: no recording of it next to the "
                                      "MIDI file (click to move there)");
    if (reading)
        worker = std::thread([this, sound] {
            auto e = fromSound(sound, cancel);
            if (cancel.load())
                return;
            {
                std::lock_guard<std::mutex> guard(lock);
                ready = std::move(e);
            }
            triggerAsyncUpdate();
        });
    repaint();
}

void LevelStrip::handleAsyncUpdate()
{
    std::shared_ptr<Envelope> e;
    {
        std::lock_guard<std::mutex> guard(lock);
        e = std::move(ready);
    }
    reading = false;
    if (e != nullptr && !e->rms.empty())
        envelope = std::move(e);
    repaint();
}

std::shared_ptr<LevelStrip::Envelope> LevelStrip::fromSound(const juce::File& sound, const std::atomic<bool>& stop)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sound));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return nullptr;
    const double seconds = (double) reader->lengthInSamples / reader->sampleRate;
    auto e = std::make_shared<Envelope>();
    e->binSeconds = std::max(kBinSeconds, seconds / kMaxBins);
    const auto perBin = std::max<juce::int64>(1, (juce::int64) std::llround(e->binSeconds * reader->sampleRate));
    const int bins = (int) ((reader->lengthInSamples + perBin - 1) / perBin);
    e->rms.assign((std::size_t) bins, 0.0f);
    e->peak.assign((std::size_t) bins, 0.0f);
    e->source = sound.getFileName();
    const int channels = (int) std::min<unsigned int>(2, reader->numChannels);
    juce::AudioBuffer<float> buffer(std::max(1, channels), 65536);
    std::vector<double> sums((std::size_t) bins, 0.0);
    for (juce::int64 start = 0; start < reader->lengthInSamples; start += buffer.getNumSamples()) {
        if (stop.load())
            return nullptr;
        const int n = (int) std::min<juce::int64>(buffer.getNumSamples(), reader->lengthInSamples - start);
        if (!reader->read(&buffer, 0, n, start, true, channels > 1))
            break;
        for (int c = 0; c < std::max(1, channels); ++c) {
            const float* x = buffer.getReadPointer(c);
            for (int k = 0; k < n; ++k) {
                const auto bin = (std::size_t) ((start + k) / perBin);
                const float v = std::abs(x[k]);
                sums[bin] += (double) v * v;
                e->peak[bin] = std::max(e->peak[bin], v);
            }
        }
    }
    for (int b = 0; b < bins; ++b) {
        const double count = (double) perBin * std::max(1, channels);
        e->rms[(std::size_t) b] = (float) std::sqrt(sums[(std::size_t) b] / count);
        e->loudest = std::max(e->loudest, e->peak[(std::size_t) b]);
    }
    return e;
}

std::shared_ptr<LevelStrip::Envelope> LevelStrip::fromNotes(const pf::MidiSequence& s)
{
    // Each note-on a burst of energy (its velocity, squared, as a struck string's), decaying over about a second
    // (a piano's sound, roughly): the strip's shape, not a measure.
    auto e = std::make_shared<Envelope>();
    e->fromMidi = true;
    e->source = "the notes";
    const double seconds = s.length + 1.0;
    e->binSeconds = std::max(kBinSeconds, seconds / kMaxBins);
    const int bins = std::max(1, (int) std::ceil(seconds / e->binSeconds));
    std::vector<double> struck((std::size_t) bins, 0.0);
    for (const auto& ev : s.events)
        if ((ev.status & 0xF0) == 0x90 && ev.data2 > 0) {
            const auto bin = (std::size_t) std::min(bins - 1, (int) (ev.seconds / e->binSeconds));
            const double v = ev.data2 / 127.0;
            struck[bin] += v * v;
        }
    const double keep = std::exp(-e->binSeconds / 1.0);
    double energy = 0.0;
    e->rms.assign((std::size_t) bins, 0.0f);
    e->peak.assign((std::size_t) bins, 0.0f);
    for (int b = 0; b < bins; ++b) {
        energy = energy * keep + struck[(std::size_t) b];
        e->rms[(std::size_t) b] = (float) std::sqrt(energy);
        e->peak[(std::size_t) b] = e->rms[(std::size_t) b];
        e->loudest = std::max(e->loudest, e->rms[(std::size_t) b]);
    }
    return e;
}

void LevelStrip::setPosition(double seconds)
{
    if (xOf == nullptr || std::abs(xOf(seconds) - xOf(position)) >= 1.0f) {
        position = seconds;
        repaint();
    }
}

double LevelStrip::timeAt(float x) const
{
    if (xOf == nullptr || length <= 0.0)
        return 0.0;
    const float x0 = xOf(0.0), x1 = xOf(length);
    return juce::jlimit(0.0, length, length * (double) ((x - x0) / std::max(1.0f, x1 - x0)));
}

void LevelStrip::mouseDown(const juce::MouseEvent& e)
{
    if (onSeek && length > 0.0)
        onSeek(timeAt(e.position.x));
}

void LevelStrip::mouseDrag(const juce::MouseEvent& e)
{
    mouseDown(e);
}

void LevelStrip::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(area, 3.0f);
    if (envelope == nullptr || xOf == nullptr || length <= 0.0 || envelope->loudest <= 0.0f)
        return;
    const float x0 = std::max(0.0f, xOf(0.0)), x1 = std::min((float) getWidth(), xOf(length));
    const float top = area.getY() + 2.0f, bottom = area.getBottom() - 1.0f, height = bottom - top;
    auto level = [&](float v) {
        const float db = 20.0f * std::log10(std::max(v, 1e-9f) / envelope->loudest);
        const float range = envelope->fromMidi ? kMidiRangeDb : kRangeDb;
        return bottom - height * juce::jlimit(0.0f, 1.0f, (db + range) / range);
    };
    // Each pixel column: the loudest bin under it (a short loud passage stays visible however narrow).
    juce::Path loud, peaks;
    loud.startNewSubPath(x0, bottom);
    bool first = true;
    for (float x = x0; x <= x1; x += 1.0f) {
        const double t0 = timeAt(x), t1 = timeAt(x + 1.0f);
        const int b0 = (int) (t0 / envelope->binSeconds), b1 = std::max(b0 + 1, (int) (t1 / envelope->binSeconds));
        float r = 0.0f, p = 0.0f;
        for (int b = b0; b < b1 && b < (int) envelope->rms.size(); ++b) {
            r = std::max(r, envelope->rms[(std::size_t) b]);
            p = std::max(p, envelope->peak[(std::size_t) b]);
        }
        loud.lineTo(x, level(r));
        if (first)
            peaks.startNewSubPath(x, level(p));
        else
            peaks.lineTo(x, level(p));
        first = false;
    }
    loud.lineTo(x1, bottom);
    loud.closeSubPath();
    // Quiet to loud: the accent, then warm, then red near the top.
    juce::ColourGradient fill(theme::bad, 0.0f, top, theme::accent.withAlpha(0.7f), 0.0f, bottom, false);
    fill.addColour(0.35, theme::busy);
    const float played = juce::jlimit(x0, x1, xOf(position));
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(juce::Rectangle<float>(x0, top, played - x0, height).toNearestInt());
        g.setGradientFill(fill);
        g.fillPath(loud);
    }
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(juce::Rectangle<float>(played, top, x1 - played, height).toNearestInt());
        g.setGradientFill(fill);
        g.setOpacity(0.7f); // what is still to come, a little dimmer
        g.fillPath(loud);
    }
    if (!envelope->fromMidi) {
        g.setColour(theme::text.withAlpha(0.35f));
        g.strokePath(peaks, juce::PathStrokeType(1.0f));
    }
    g.setColour(juce::Colours::white);
    g.drawLine(played, top - 1.0f, played, bottom, 1.5f);
    g.setColour(theme::dim);
    g.setFont(theme::font(10.5f));
    g.drawText(reading ? juce::String("reading the recording...") : envelope->fromMidi ? juce::String("from the notes") : envelope->source,
               getLocalBounds().reduced(6, 1).removeFromTop(12), juce::Justification::topRight, true);
}

} // namespace pf::app
