#include "Diagnostics.h"

#include "AudioHost.h"
#include "MirrorOutput.h"
#include "ui/AudioPanel.h"
#include "ui/EnginePanel.h"
#include "ui/PerformancePanel.h"
#include "ui/PlayerBar.h"
#include "ui/Theme.h"
#include "KeyboardInput.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace pf::app {

namespace {

/// Records raw messages, then forwards them to the application's input path.
class Recorder final : public juce::MidiInputCallback {
public:
    Recorder(KeyboardInput& k, double t0) : next(k), start(t0) {}

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        {
            const juce::ScopedLock sl(lock);
            log << juce::String(now - start, 4) << " s | " << (source != nullptr ? source->getName() : juce::String("?"))
                << " | timestamp - now = " << juce::String((m.getTimeStamp() - now) * 1000.0, 2) << " ms | "
                << juce::String::toHexString(m.getRawData(), m.getRawDataSize()) << " | " << m.getDescription() << "\n";
            ++count;
        }
        next.handleIncomingMidiMessage(source, m);
    }

    juce::String take()
    {
        const juce::ScopedLock sl(lock);
        auto t = log;
        log.clear();
        return t;
    }

    int count = 0;

private:
    KeyboardInput& next;
    double start;
    juce::CriticalSection lock;
    juce::String log;
};

} // namespace

int runMidiMonitor(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--midi-monitor");
    const double seconds = arguments[i + 1].isNotEmpty() ? arguments[i + 1].getDoubleValue() : 20.0;
    const juce::File reportFile(arguments[i + 2].isNotEmpty() ? arguments[i + 2] : "midi-monitor.txt");
    const juce::File corpus(arguments[i + 3]);
    juce::String report;
    report << "PianoForge MIDI monitor - " << juce::Time::getCurrentTime().toString(true, true) << "\n\nMIDI inputs seen by JUCE:\n";

    // The same engine path as the application, fed by simulated 128-sample blocks at 48 kHz.
    const double fs = 48000.0;
    constexpr int block = 128;
    ReferencePlayer player;
    AudioHost host { player };
    pf::VelocityCurve curve;
    KeyboardInput keyboard { host.collector, curve };
    if (corpus.isDirectory()) {
        BankLoader loader;
        loader.load(corpus, [](int, int vel) { return vel == 64; }); // one take per note is enough here
        while (loader.isLoading())
            juce::Thread::sleep(20);
        player.installBank(loader.takeResult());
        report << "(engine loaded with " << loader.status() << ")\n";
    }
    host.collector.reset(fs);
    player.prepare(fs, block);

    const double t0 = juce::Time::getMillisecondCounterHiRes() * 0.001;
    Recorder recorder { keyboard, t0 };
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    for (const auto& d : juce::MidiInput::getAvailableDevices()) {
        report << "  - " << d.name << "   [" << d.identifier << "]\n";
        if (auto in = juce::MidiInput::openDevice(d.identifier, &recorder)) {
            in->start();
            inputs.push_back(std::move(in));
        } else {
            report << "    (could not be opened: another application may hold it)\n";
        }
    }
    report << "\nRaw messages, then engine state after each simulated block that received events:\n";

    std::vector<float> l(block), r(block);
    float* outputs[2] = { l.data(), r.data() };
    juce::String engineLog;
    std::uint32_t seenNoteOns = 0;
    int maxVoices = 0;
    const double blockSeconds = block / fs;
    double nextBlock = t0;
    while (juce::Time::getMillisecondCounterHiRes() * 0.001 - t0 < seconds) {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (now < nextBlock) {
            juce::Thread::sleep(1);
            continue;
        }
        nextBlock += blockSeconds;
        host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, block, {});
        const auto noteOns = player.noteOnsReceived.load();
        maxVoices = juce::jmax(maxVoices, player.activeVoices.load());
        if (noteOns != seenNoteOns) {
            engineLog << juce::String(now - t0, 4) << " s | engine: " << (int) (noteOns - seenNoteOns)
                      << " note-on(s) in this block, total " << (int) noteOns << ", active voices "
                      << player.activeVoices.load() << "\n";
            seenNoteOns = noteOns;
        }
    }
    for (auto& in : inputs)
        in->stop();

    report << recorder.take() << "\n" << engineLog << "\nSummary: " << recorder.count << " raw messages, "
           << (int) player.noteOnsReceived.load() << " note-ons reached the engine, at most " << maxVoices
           << " voices sounding at once.\n";
    reportFile.replaceWithText(report, false, false, "\n");
    return 0;
}

int runListAudio(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--list-audio");
    const juce::File reportFile(arguments[i + 1].isNotEmpty() ? arguments[i + 1] : "audio-devices.txt");
    juce::String report;
    juce::AudioDeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes()) {
        type->scanForDevices();
        report << type->getTypeName() << ":\n";
        for (const auto& name : type->getDeviceNames(false))
            report << "  - " << name << "\n";
    }
    reportFile.replaceWithText(report, false, false, "\n");
    return 0;
}


namespace {

/// Captures an input device into a buffer allocated beforehand, noting the wall time of the first block.
class Capture final : public juce::AudioIODeviceCallback {
public:
    explicit Capture(int frames) : data((std::size_t) frames, 0.0f) {}
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const*, int, int n,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        if (count == 0)
            firstBlock = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (int k = 0; k < n && count < (int) data.size(); ++k)
            data[(std::size_t) count++] = numInputs > 0 && inputs[0] != nullptr ? inputs[0][k] : 0.0f;
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    std::vector<float> data;
    std::atomic<int> count { 0 };
    std::atomic<double> firstBlock { 0.0 };
};

} // namespace

int runMirrorCheck(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--mirror-check");
    const juce::String playback = arguments[i + 1], capture = arguments[i + 2];
    const juce::File reportFile(arguments[i + 3].isNotEmpty() ? arguments[i + 3] : "mirror-check.txt");
    juce::String report;
    constexpr double inRate = 44100.0, toneStart = 1.0, seconds = 10.0;
    constexpr int block = 128;

    // The capture side: the cable's output, through the shared mixer, as OBS takes it.
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared));
    if (type == nullptr) {
        reportFile.replaceWithText("no WASAPI on this system");
        return 1;
    }
    type->scanForDevices();
    std::unique_ptr<juce::AudioIODevice> in(type->createDevice({}, capture));
    if (in == nullptr) {
        reportFile.replaceWithText("cannot find the capture device " + capture + "; inputs: " + type->getDeviceNames(true).joinIntoString(", "));
        return 1;
    }
    juce::BigInteger channels;
    channels.setRange(0, 2, true);
    if (const auto e = in->open(channels, {}, 48000.0, in->getDefaultBufferSize()); e.isNotEmpty()) {
        reportFile.replaceWithText("cannot open " + capture + ": " + e);
        return 1;
    }
    const double captureRate = in->getCurrentSampleRate();
    Capture recorder((int) (captureRate * (seconds + 1.0)));

    MirrorOutput mirror;
    if (const auto opened = mirror.open(playback, inRate, block); opened.failed()) {
        reportFile.replaceWithText("cannot open the mirror on " + playback + ": " + opened.getErrorMessage());
        return 1;
    }
    in->start(&recorder);
    // The producer: blocks at the pace of a 44.1 kHz device, silence then the tone from toneStart.
    std::vector<float> l(block), r(block);
    const float* outputs[2] = { l.data(), r.data() };
    long long produced = 0;
    const auto start = std::chrono::steady_clock::now();
    while (produced < (long long) (seconds * inRate)) {
        for (int k = 0; k < block; ++k) {
            const double t = (double) (produced + k) / inRate;
            l[(std::size_t) k] = r[(std::size_t) k] = t < toneStart ? 0.0f : 0.01f * (float) std::sin(6.283185307179586 * 1000.0 * t);
        }
        mirror.push(outputs, 2, block);
        produced += block;
        std::this_thread::sleep_until(start + std::chrono::microseconds((long long) (1e6 * produced / inRate)));
    }
    const auto status = mirror.status();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    in->stop();
    mirror.close();
    in->close();

    // The tone in the capture: onset, level and frequency.
    const int n = recorder.count.load();
    int onset = -1;
    for (int k = 0; k < n && onset < 0; ++k)
        if (std::abs(recorder.data[(std::size_t) k]) > 0.003f)
            onset = k;
    double level = -300.0, frequency = 0.0;
    if (onset >= 0 && onset + (int) captureRate < n) {
        const int a = onset + (int) (0.2 * captureRate), b = onset + (int) captureRate;
        double e = 0.0;
        int crossings = 0;
        for (int k = a; k < b; ++k) {
            e += (double) recorder.data[(std::size_t) k] * recorder.data[(std::size_t) k];
            crossings += (recorder.data[(std::size_t) k - 1] < 0.0f) != (recorder.data[(std::size_t) k] < 0.0f) ? 1 : 0;
        }
        level = 10.0 * std::log10(e / (b - a) + 1e-30);
        frequency = 0.5 * crossings * captureRate / (b - a);
    }
    report << "Mirror on " << playback << ": " << status << "\n"
           << "Captured from " << capture << " at " << juce::String(captureRate, 0) << " Hz: " << n << " samples\n"
           << "Tone: " << (onset >= 0 ? "received" : "NOT received") << ", level " << juce::String(level, 1)
           << " dBFS (sent -43.0 dBFS RMS), frequency " << juce::String(frequency, 1)
           << " Hz (sent 1000 Hz: another value means the two ends of the cable run at different rates)\n";
    { // the capture, for a closer look
        juce::AudioBuffer<float> audio(1, n);
        audio.copyFrom(0, 0, recorder.data.data(), n);
        const auto wav = reportFile.withFileExtension("wav");
        wav.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(wav);
        juce::WavAudioFormat format;
        if (auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions {}.withSampleRate(captureRate).withNumChannels(1).withBitsPerSample(32)
                                                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
            writer->writeFromAudioSampleBuffer(audio, 0, n);
    }
    reportFile.replaceWithText(report);
    const bool ok = onset >= 0 && std::abs(frequency - 1000.0) < 5.0 && std::abs(level + 43.0) < 3.0;
    return ok ? 0 : 1;
}


int runUiSnapshots(const juce::StringArray& arguments)
{
    const juce::File folder(arguments[arguments.indexOf("--ui-snapshots") + 1]);
    folder.createDirectory();
    theme::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
    auto save = [&](juce::Component& c, const juce::String& name) {
        const auto image = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
        const auto file = folder.getChildFile(name + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    };
    {
        pf::ModalEngine modal;
        ReferencePlayer player;
        juce::PropertiesFile::Options options;
        juce::PropertiesFile settings(folder.getChildFile("snapshot.settings"), options);
        EnginePanel engine(modal, player, settings);
        save(engine, "engine-settings");
        juce::AudioDeviceManager devices; // not initialised: no device opened
        AudioHost host(modal);
        AudioPanel audio(devices, host, settings);
        save(audio, "audio-settings");
        PerformancePanel performance;
        LoadTracker::Snapshot snap;
        snap.now = 0.18f;
        snap.average = 0.14f;
        snap.peakRecent = 0.31f;
        snap.peakMinute = 0.57f;
        snap.blockMs = 10.0;
        snap.lateBlocks = 0;
        snap.gaps = 2;
        snap.driverDropouts = -1;
        snap.dropouts = 2;
        snap.since = juce::Time::getCurrentTime();
        for (int i = 0; i < LoadGraph::kColumns; ++i)
            performance.pushColumn(0.12f + 0.08f * (float) std::sin(i * 0.05) + (i % 97 == 0 ? 0.45f : 0.0f), i == 300);
        performance.update(snap, { "Modal: PianoForge engine", "12 voices of 64, 3 240 resonators", "30 free strings ringing, pedal 100 %, 0 voices stolen" },
                           { "Haut-parleurs (FiiO K series) (Windows Audio)", "192.0 kHz, blocks of 1920 (10.0 ms), output latency ~20.0 ms" },
                           { "At 192 kHz the engine does 4.0 times the work of 48 kHz for nothing audible: the corpus is at 44.1 kHz. Use 44.1 or 48 kHz.",
                             "Blocks of 10.0 ms add as much latency between key and sound: 128 samples (2.9 ms at 44.1 kHz) is comfortable for this engine." });
        save(performance, "performance");
        PlayerBar bar(host.filePlayer, settings);
        auto sequence = std::make_shared<MidiFilePlayer::Sequence>();
        sequence->name = "nocturne-op9-2";
        sequence->notes = 1406;
        sequence->tracks = 2;
        sequence->length = 271.0;
        sequence->events.push_back({ 0.0, 0x90, 60, 64 });
        host.filePlayer.load(sequence);
        bar.fileChanged();
        bar.setBounds(0, 0, 1416, 40);
        save(bar, "player-bar");
        host.filePlayer.load(nullptr);
        bar.fileChanged();
        save(bar, "player-bar-empty");
        settings.getFile().deleteFile();
    }
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    return 0;
}

} // namespace pf::app
