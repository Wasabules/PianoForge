#include "UpdateChecker.h"
#include "Diagnostics.h"

#include "AudioDevices.h"
#include "AudioHost.h"
#include "MirrorOutput.h"
#include "DesktopIntegration.h"
#include "ui/AudioPanel.h"
#include "ui/DesktopPanel.h"
#include "ui/GalleryPanel.h"
#include "ui/NamingDialog.h"
#include "ui/RecordingPanel.h"
#include "ui/EnginePanel.h"
#include "ui/PerformancePanel.h"
#include "ui/PlayerBar.h"
#include "ui/RoomPanel.h"
#include "ui/TonePanel.h"
#include "ui/SettingsPanel.h"
#include "Presets.h"
#include "ui/Theme.h"
#include "KeyboardInput.h"
#include "VelocityCurveEditor.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "DesktopBus.h"
#include "ObsFlatpak.h"
#include "PipeWireAudio.h"
#include "SettingsLocation.h"

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
    DeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes()) {
        type->scanForDevices();
        report << type->getTypeName() << ":\n";
        for (const auto& name : type->getDeviceNames(false))
            report << "  - " << name << "\n";
    }
    reportFile.replaceWithText(report, false, false, "\n");
    return 0;
}

int runAudioHold(const juce::StringArray& arguments)
{
    // Test aid: holds the saved audio device for a while (as a closing application or another program would).
    const double seconds = arguments[arguments.indexOf("--audio-hold") + 1].getDoubleValue();
    juce::ApplicationProperties properties;
    properties.setStorageParameters(settingsOptions());
    const auto saved = properties.getUserSettings()->getXmlValue("audio");
    DeviceManager manager;
    manager.initialise(0, 2, saved.get(), false);
    juce::Thread::sleep((int) (std::max(0.0, seconds) * 1000.0));
    manager.closeAudioDevice();
    return 0;
}

int runAudioRestore(const juce::StringArray& arguments)
{
    // What the application does at start: the saved device, the preferred setup otherwise.
    const int i = arguments.indexOf("--audio-restore");
    const juce::File reportFile(arguments[i + 1].isNotEmpty() ? arguments[i + 1] : "audio-restore.txt");
    juce::ApplicationProperties properties;
    properties.setStorageParameters(settingsOptions());
    const auto saved = properties.getUserSettings()->getXmlValue("audio");
    juce::String report;
    report << "saved: " << (saved != nullptr ? saved->toString(juce::XmlElement::TextFormat().singleLine()) : juce::String("none")) << "\n";
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.sampleRate = 44100.0;
    preferred.bufferSize = 128;
    for (const bool fallback : { false, true }) {
        DeviceManager manager;
        const auto error = manager.initialise(0, 2, saved.get(), fallback, {}, &preferred);
        auto* d = manager.getCurrentAudioDevice();
        report << (fallback ? "with" : "without") << " the default on failure: error '" << error << "', opened "
               << (d != nullptr ? manager.getCurrentAudioDeviceType() + " / " + d->getName() + " at " + juce::String(d->getCurrentSampleRate())
                                      + " Hz, " + juce::String(d->getCurrentBufferSizeSamples())
                                : juce::String("nothing"))
               << "\n";
        manager.closeAudioDevice();
    }
    reportFile.replaceWithText(report, false, false, "\n");
    return 0;
}

namespace {

/// Plays silence and measures the callbacks: their blocks, the time between them, the thread they run on.
class AudioProbe final : public juce::AudioIODeviceCallback {
public:
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs, int n,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        const auto now = juce::Time::getHighResolutionTicks();
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                juce::FloatVectorOperations::clear(outputs[c], n);
        thread.store(linuxaudio::threadId(), std::memory_order_relaxed);
        if (resetPending.exchange(false)) {
            callbacks = 0;
            frames = 0;
            minBlock = 1 << 30;
            maxBlock = 0;
            maxGap = 0.0;
            last = 0;
            started = now;
        }
        if (last != 0)
            maxGap = std::max(maxGap.load(), juce::Time::highResolutionTicksToSeconds(now - last));
        last = now;
        callbacks.fetch_add(1);
        frames.fetch_add(n);
        minBlock = std::min(minBlock.load(), n);
        maxBlock = std::max(maxBlock.load(), n);
        ended = now;
    }
    std::atomic<bool> resetPending { true };
    std::atomic<std::int64_t> thread { 0 }, callbacks { 0 }, frames { 0 }, started { 0 }, ended { 0 };
    std::atomic<int> minBlock { 1 << 30 }, maxBlock { 0 };
    std::atomic<double> maxGap { 0.0 };
    std::int64_t last = 0; // audio thread
};

} // namespace

int runDesktopCheck(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--desktop-check");
    const int holdAt = arguments.indexOf("--hold");
    const int hold = holdAt >= 0 ? arguments[holdAt + 1].getIntValue() : 3;
    juce::String reportPath;
    for (int k = i + 1; k < arguments.size(); ++k) {
        if (arguments[k] == "--hold")
            ++k;
        else if (!arguments[k].startsWith("--")) {
            reportPath = arguments[k];
            break;
        }
    }
    const juce::File reportFile(reportPath.isNotEmpty() ? reportPath : "desktop-check.txt");
    juce::String report;
    report << "PianoForge desktop check - " << juce::Time::getCurrentTime().toString(true, true) << "\n"
           << "session bus (GIO): " << (desktop::available() ? "yes" : "no") << "\n";

    desktop::KeepAwake awake;
    desktop::PerformanceProfile performance;
    const bool awakeHeld = awake.set(true, "PianoForge desktop check");
    const bool performanceHeld = performance.set(true, "PianoForge desktop check");
    report << "keep awake: " << (awakeHeld ? "held" : "not held") << " (" << juce::String(awake.how()) << ")\n"
           << "performance profile: " << (performanceHeld ? "held" : "not held") << " (" << juce::String(performance.how()) << ")\n";
    desktop::MediaControls media({});
    for (int k = 0; k < 40 && !media.active(); ++k)
        juce::Thread::sleep(50);
    report << "media controls (MPRIS): " << (media.active() ? "registered" : "not registered") << "\n";
    juce::Thread::sleep(1000 * std::max(0, hold)); // (time to look at them from outside)
    awake.set(false, {});
    performance.set(false, {});
    if (arguments.contains("--notify"))
        report << "notification: " << (desktop::notify("PianoForge", "Desktop check: notifications reach the desktop") ? "shown" : "not shown")
               << "\n";
#if JUCE_LINUX || JUCE_MAC
    struct Ignore final : juce::MidiInputCallback {
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override {}
    } ignore;
    const auto input = juce::MidiInput::createNewDevice("PianoForge check", &ignore);
    report << "virtual MIDI input: " << (input != nullptr ? "made" : "not available") << "\n";
#endif
#if JUCE_LINUX
    const auto obs = obsflatpak::query();
    report << "Flathub's OBS: " << (obs.installed ? "installed" : "not installed");
    if (obs.installed)
        report << ", plugin " << (obs.plugin ? "in it" : "not in it") << ", shared memory " << (obs.sharedMemory ? "granted" : "not granted");
    report << "\n";
    if (const auto type = pipewire::createDeviceType(false)) {
        const auto out = pipewire::output(nullptr);
        report << "PipeWire's default output: " << (out.known ? juce::String::fromUTF8(out.description.c_str()) : juce::String("unknown"))
               << (out.bluetooth ? " (Bluetooth)" : "") << "\n";
    }
#endif
    reportFile.replaceWithText(report, false, false, "\n");
    return 0;
}

int runAudioCheck(const juce::StringArray& arguments)
{
    auto option = [&](const char* name) {
        const int at = arguments.indexOf(name);
        return at >= 0 ? arguments[at + 1] : juce::String();
    };
    const int i = arguments.indexOf("--audio-check");
    juce::String reportPath;
    for (int k = i + 1; k < arguments.size(); ++k) { // the report: the first argument that is neither an option nor a value
        if (juce::StringArray { "--type", "--device", "--rate", "--block", "--seconds" }.contains(arguments[k]))
            ++k;
        else if (!arguments[k].startsWith("--")) {
            reportPath = arguments[k];
            break;
        }
    }
    const juce::File reportFile(reportPath.isNotEmpty() ? reportPath : "audio-check.txt");
    const double seconds = option("--seconds").isNotEmpty() ? option("--seconds").getDoubleValue() : 10.0;

    juce::ApplicationProperties properties;
    properties.setStorageParameters(settingsOptions());
    auto* settings = properties.getUserSettings();
    pipewire::forceQuantum() = arguments.contains("--no-force") ? false
                               : arguments.contains("--force") ? true
                                                               : settings->getBoolValue("pipewireForceQuantum", true);
    DeviceManager manager;
    juce::String report, error;
    const auto saved = settings->getXmlValue("audio");
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.sampleRate = 48000.0;
    preferred.bufferSize = 128;
    if (option("--type").isEmpty() && saved != nullptr) {
        error = manager.initialise(0, 2, saved.get(), false, {}, &preferred);
    } else {
        error = manager.initialise(0, 2, nullptr, false, {}, &preferred);
        if (option("--type").isNotEmpty())
            manager.setCurrentAudioDeviceType(option("--type"), true);
    }
    auto setup = manager.getAudioDeviceSetup();
    if (option("--device").isNotEmpty())
        setup.outputDeviceName = option("--device");
    if (option("--rate").isNotEmpty())
        setup.sampleRate = option("--rate").getDoubleValue();
    if (option("--block").isNotEmpty())
        setup.bufferSize = option("--block").getIntValue();
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (option("--type").isNotEmpty() || option("--device").isNotEmpty() || option("--rate").isNotEmpty() || option("--block").isNotEmpty())
        error = manager.setAudioDeviceSetup(setup, true);

    juce::StringArray types;
    for (auto* t : manager.getAvailableDeviceTypes())
        types.add(t->getTypeName());
    report << "PianoForge audio check - " << juce::Time::getCurrentTime().toString(true, true) << "\n"
           << "device types: " << types.joinIntoString(", ") << "\n"
           << "PipeWire's block asked for (node.force-quantum): " << (pipewire::forceQuantum().load() ? "yes" : "no") << "\n";
#if JUCE_LINUX
    if (const auto clock = linuxaudio::pipewireClock(); clock.running)
        report << "PipeWire's settings: rate " << clock.rate << ", min-quantum " << clock.minQuantum << ", max-quantum "
               << clock.maxQuantum << ", force-quantum " << clock.forceQuantum << "\n";
    if (!linuxaudio::jackSetup().empty())
        report << "JACK: " << juce::String(linuxaudio::jackSetup()) << "\n";
#endif
    auto* d = manager.getCurrentAudioDevice();
    if (d == nullptr || error.isNotEmpty()) {
        const auto type = option("--type").isNotEmpty() ? option("--type") : manager.getCurrentAudioDeviceType();
        juce::String why = error.isNotEmpty() ? error : juce::String("no device opened");
#if JUCE_LINUX
        if (type == "JACK" && error.isEmpty()) // no device at all: no server answered
            why = linuxaudio::pipewireRunning() && !linuxaudio::pipewireJack()
                      ? "no JACK server: with PipeWire, its JACK library makes one (the package pipewire-jack)"
                      : "no JACK server answered";
#endif
        report << "[FAIL]  " << type << " / " << setup.outputDeviceName << ": " << why << "\n";
        reportFile.replaceWithText(report, false, false, "\n");
        return 1;
    }

    AudioProbe probe;
    manager.addAudioCallback(&probe);
    juce::Thread::sleep(600);
    linuxaudio::Scheduling scheduling;
    if (const auto tid = probe.thread.load(); tid != 0) // as the application does
        scheduling = linuxaudio::makeRealtime(tid);
    juce::Thread::sleep(400);
    probe.resetPending = true; // the device started: from now on
    juce::Thread::sleep(juce::roundToInt(1000.0 * seconds));
    const double fs = d->getCurrentSampleRate();
    const int block = d->getCurrentBufferSizeSamples();
    const double elapsed = juce::Time::highResolutionTicksToSeconds(probe.ended.load() - probe.started.load());
    const double latency = 1000.0 * d->getOutputLatencyInSamples() / fs;
    const auto pw = pipewire::status(d);
    manager.removeAudioCallback(&probe);

    report << "device: " << manager.getCurrentAudioDeviceType() << " / " << d->getName() << "\n"
           << "rate " << fs << " Hz, block " << block << " (" << juce::String(1000.0 * block / fs, 2) << " ms)\n"
           << "callbacks: " << probe.callbacks.load() << " over " << juce::String(elapsed, 2) << " s, blocks of "
           << probe.minBlock.load() << " to " << probe.maxBlock.load() << " samples, "
           << juce::String(elapsed > 0.0 ? (double) probe.frames.load() / elapsed : 0.0, 1) << " samples/s\n"
           << "longest wait between callbacks: " << juce::String(1000.0 * probe.maxGap.load(), 2) << " ms\n"
           << "output latency the device reports: " << juce::String(latency, 2) << " ms";
    if (pw.has_value())
        report << " (PipeWire's measure, to the device)\n"
               << "PipeWire's graph: " << pw->graphBlock << " samples at " << pw->graphRate << " Hz ("
               << juce::String(pw->graphRate > 0.0 ? 1000.0 * pw->graphBlock / pw->graphRate : 0.0, 2) << " ms), "
               << (pw->forced ? "asked for" : "its own") << ", " << (int) pw->xruns << " xruns\n"
               << "key to sound, estimated: a block of input timing + the output latency = "
               << juce::String(1000.0 * block / fs + latency, 1) << " ms\n";
    else
        report << " (its own figure: through PipeWire, the graph's own delay comes on top)\n";
    report << "xruns the device counted: " << d->getXRunCount() << "\n"
           << "audio thread: " << juce::String(scheduling.how.empty() ? std::string("unknown") : scheduling.how) << "\n";
    manager.closeAudioDevice();
    reportFile.replaceWithText(report, false, false, "\n");
    return probe.callbacks.load() > 0 ? 0 : 1;
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
        DeviceManager devices; // not initialised: no device opened
        AudioHost host(modal);
        AudioPanel audio(devices, host, settings);
        save(audio, "audio-settings");
        DesktopIntegration integration(settings, {});
        DesktopPanel desktopSettings(settings, integration);
        save(desktopSettings, "desktop-settings");
        TonePanel tone(modal, host, settings);
        save(tone, "tone-and-dynamics");
        {
            SettingsPanel::Presets hooks;
            hooks.factoryNames = [] {
                juce::StringArray names;
                for (const auto& p : factoryPresets())
                    names.add(p.name);
                return names;
            };
            hooks.userNames = [] { return juce::StringArray { "Evening at home", "Stream Sunday" }; };
            hooks.factoryDescription = [](int i) { return juce::String(factoryPresets()[(size_t) i].description); };
            hooks.current = [] { return juce::String("Soft and round"); };
            hooks.currentIsFactory = [] { return true; };
            hooks.modified = [] { return true; };
            hooks.applyFactory = hooks.applyUser = [](int) {};
            hooks.save = hooks.saveAs = hooks.revert = hooks.remove = [] {};
            SettingsPanel all({ &engine, &tone, nullptr, &audio, nullptr, nullptr, nullptr }, { false, false, false, false, false, false, true },
                              std::move(hooks));
            all.setSize(1000, 860);
            all.show(SettingsPanel::Section::tone);
            save(all, "settings");
        }
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
        for (int i = 0; i < 1406; ++i) { // a nocturne's shape: quiet, a climax two thirds in, quiet again
            const double t = 0.19 * i, arc = std::sin(3.14159 * std::pow(t / 271.0, 1.4));
            const auto velocity = (std::uint8_t) juce::jlimit(1, 127, (int) (30 + 90 * arc * arc + 10 * std::sin(t * 1.7)));
            sequence->events.push_back({ t, 0x90, (std::uint8_t) (48 + i % 36), velocity });
        }
        for (const char* hand : { "Right hand", "Left hand" }) {
            pf::MidiSequence::Part part;
            part.track = (int) sequence->parts.size() + 1;
            part.name = hand;
            part.program = 0;
            part.notes = 703;
            sequence->parts.push_back(part);
        }
        host.filePlayer.load(sequence);
        bar.fileChanged();
        bar.setBounds(0, 0, 1416, 108);
        save(bar, "player-bar");
        bar.setSpeed(1.5); // the time at the speed, and the file's
        bar.refresh();
        save(bar, "player-bar-150");
        bar.setSpeed(1.0);
        host.filePlayer.load(nullptr);
        bar.fileChanged();
        save(bar, "player-bar-empty");
        RoomPanel roomPanel(host.room, settings);
        save(roomPanel, "room");
        {   // A preset of the user's, saved, read back and selected at start.
            juce::PropertiesFile own(folder.getChildFile("snapshot-presets.settings"), options);
            own.setValue("roomModel", 2);
            own.setValue("roomUserPresets", "[{\"name\":\"Living room, 3 m\",\"roomLevel\":-3.0,\"roomSize\":0.3,\"roomDecay\":0.7,"
                                            "\"roomBrightness\":0.6,\"roomWidth\":0.9,\"roomDistance\":3.0,\"keepLoudness\":true}]");
            own.setValue("roomPreset", 200);
            own.setValue("roomPresetName", "Living room, 3 m");
            own.setValue("roomDistance", 3.0);
            own.setValue("roomPosition", 2);
            own.setValue("roomHeadphones", true); // and heard on headphones
            pf::Room room;
            RoomPanel withPreset(room, own);
            save(withPreset, "room-user-preset");
            own.getFile().deleteFile();
        }
        // The pedal curve: a pedal that never sends more than 120, the half pedal of the reference's measured curve.
        juce::MidiMessageCollector collector;
        pf::VelocityCurve velocities;
        pf::PedalCurve pedal;
        auto pedalSettings = pf::PedalCurve::defaults();
        pedalSettings.inputMax = 120;
        pedal.set(pedalSettings);
        KeyboardInput keys(collector, velocities, &pedal);
        collector.reset(44100.0);
        keys.handleIncomingMidiMessage(nullptr, juce::MidiMessage::controllerEvent(1, 64, 40));
        VelocityCurvePanel pedalPanel(pedalSettings, keys, {}, CurveKind::pedal);
        std::array<float, 128> lift {};
        for (int v = 0; v < 128; ++v) // as measured (forge half-pedal): resting at 12 and under, lifted from 20
            lift[(std::size_t) v] = juce::jlimit(0.0f, 1.0f, (float) (v - 12) / 8.0f);
        pedalPanel.setDamperLift(lift);
        save(pedalPanel, "pedal-curve");
        {   // The gallery of this computer's takes and recordings, the naming question, the recording settings.
            TakePlayback playback;
            const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("PianoForge");
            const std::vector<std::pair<juce::File, bool>> roots { { docs.getChildFile("Recordings"), true }, { docs.getChildFile("Takes"), false } };
            GalleryPanel gallery(playback, roots, {});
            save(gallery, "gallery");
            NamingDialog::Info info;
            info.time = juce::Time::getCurrentTime();
            info.seconds = 192.0;
            info.notes = 1024;
            info.formats = "wav mp3 mid";
            info.root = docs.getChildFile("Recordings");
            NamingDialog naming(info, {}, scanGallery(roots), {});
            save(naming, "naming");
            RecordingPanel recording(settings);
            save(recording, "recording-settings");
        }
        settings.getFile().deleteFile();
    }
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    return 0;
}

int runCheckUpdate(const juce::StringArray& arguments)
{
    const int at = arguments.indexOf("--check-update");
    const auto address = arguments[at + 1].unquoted();
    const juce::File reportFile(arguments[at + 2].isNotEmpty() ? arguments[at + 2] : "update-check.txt");
    juce::String report;
    report << "PianoForge " << PIANOFORGE_VERSION << ", update address: " << address << "\n";
    const auto result = UpdateChecker::fetch(address, PIANOFORGE_VERSION);
    int code = 1;
    if (!result.ok) {
        report << "not read: " << result.error << "\n";
    } else {
        report << "offers " << result.release.version << " (" << (result.newer ? "newer" : "not newer") << " than this one), installer "
               << result.release.installer << ", " << juce::String(result.release.size) << " bytes, SHA-256 " << result.release.sha256 << "\n";
        const auto destination = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("PianoForge-update-check")
                                     .getChildFile(result.release.fileName());
        if (const auto error = UpdateChecker::download(result.release, destination); error.isNotEmpty()) {
            report << "download: " << error << "\n";
        } else {
            report << "downloaded and checked: " << destination.getFullPathName() << " (not run)\n";
            destination.deleteFile();
            code = 0;
        }
    }
    reportFile.replaceWithText(report, false, false, "\n");
    return code;
}

} // namespace pf::app
