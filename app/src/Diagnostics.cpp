#include "Diagnostics.h"

#include "AudioHost.h"
#include "KeyboardInput.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"

#include <juce_audio_devices/juce_audio_devices.h>

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

} // namespace pf::app
