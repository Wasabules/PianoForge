#include "AudioDevices.h"

#include "LinuxAudio.h"
#include "PipeWireAudio.h"

namespace pf::app {

#if JUCE_LINUX

namespace {

enum class Kind { alsa, jack };

/// JUCE's ALSA devices that are PipeWire's plug-in: "PipeWire Sound Server", "Default ALSA Output (currently PipeWire
/// Media Server)".
bool alsaThroughPipeWire(const juce::String& name)
{
    return name.containsIgnoreCase("PipeWire");
}

/// A device of JUCE's ALSA or JACK type, everything forwarded, but its opening: through PipeWire, the graph is asked
/// for the device's block first. Its callback's device is this one (the application never sees the inner one).
class QuantumDevice final : public juce::AudioIODevice, private juce::AudioIODeviceCallback {
public:
    QuantumDevice(juce::AudioIODeviceType& t, std::unique_ptr<juce::AudioIODevice> d, const juce::String& out,
                  const juce::String& in, Kind k)
        : juce::AudioIODevice(d->getName(), d->getTypeName()), innerType(t), inner(std::move(d)), outputName(out),
          inputName(in), kind(k)
    {
    }

    ~QuantumDevice() override { close(); }

    juce::AudioIODevice* innerDevice() const noexcept { return inner.get(); }

    bool viaPipeWire() const
    {
        return kind == Kind::alsa ? alsaThroughPipeWire(getName()) : linuxaudio::pipewireJack();
    }

    juce::StringArray getOutputChannelNames() override { return inner->getOutputChannelNames(); }
    juce::StringArray getInputChannelNames() override { return inner->getInputChannelNames(); }
    std::optional<juce::BigInteger> getDefaultOutputChannels() const override { return inner->getDefaultOutputChannels(); }
    std::optional<juce::BigInteger> getDefaultInputChannels() const override { return inner->getDefaultInputChannels(); }
    juce::Array<double> getAvailableSampleRates() override { return inner->getAvailableSampleRates(); }

    juce::Array<int> getAvailableBufferSizes() override
    {
        if (kind == Kind::jack && forcing()) // PipeWire's JACK runs the graph at the block chosen
            return { 32, 64, 128, 256, 512, 1024, 2048 };
        return inner->getAvailableBufferSizes();
    }

    int getDefaultBufferSize() override { return kind == Kind::jack && forcing() ? 128 : inner->getDefaultBufferSize(); }

    juce::String open(const juce::BigInteger& inputs, const juce::BigInteger& outputs, double rate, int block) override
    {
        if (!forcing())
            return explained(inner->open(inputs, outputs, rate, block));
        if (kind == Kind::alsa) { // PipeWire's ALSA plug-in reads PIPEWIRE_ALSA when the device opens
            linuxaudio::ScopedProps props("PIPEWIRE_ALSA", linuxaudio::quantumProps(block, rate));
            return explained(inner->open(inputs, outputs, rate, block));
        }
        // JACK: the client is made with JUCE's device, and PipeWire's JACK reads its properties then: made again.
        std::unique_ptr<juce::AudioIODevice> fresh;
        {
            linuxaudio::ScopedProps props("PIPEWIRE_PROPS", linuxaudio::quantumProps(block, rate));
            inner->close();
            fresh.reset(innerType.createDevice(outputName, inputName));
        }
        if (fresh == nullptr)
            return "JACK did not answer";
        inner = std::move(fresh);
        return inner->open(inputs, outputs, rate, block);
    }

    void close() override { inner->close(); }
    bool isOpen() override { return inner->isOpen(); }

    void start(juce::AudioIODeviceCallback* callback) override
    {
        user = callback;
        inner->start(this);
    }

    void stop() override
    {
        inner->stop();
        user = nullptr;
    }

    bool isPlaying() override { return inner->isPlaying(); }
    juce::String getLastError() override { return inner->getLastError(); }
    int getCurrentBufferSizeSamples() override { return inner->getCurrentBufferSizeSamples(); }
    double getCurrentSampleRate() override { return inner->getCurrentSampleRate(); }
    int getCurrentBitDepth() override { return inner->getCurrentBitDepth(); }
    juce::BigInteger getActiveOutputChannels() const override { return inner->getActiveOutputChannels(); }
    juce::BigInteger getActiveInputChannels() const override { return inner->getActiveInputChannels(); }
    int getOutputLatencyInSamples() override { return inner->getOutputLatencyInSamples(); }
    int getInputLatencyInSamples() override { return inner->getInputLatencyInSamples(); }
    juce::AudioWorkgroup getWorkgroup() const override { return inner->getWorkgroup(); }
    bool hasControlPanel() const override { return inner->hasControlPanel(); }
    bool showControlPanel() override { return inner->showControlPanel(); }
    bool setAudioPreprocessingEnabled(bool enabled) override { return inner->setAudioPreprocessingEnabled(enabled); }
    int getXRunCount() const noexcept override { return inner->getXRunCount(); }

private:
    juce::AudioIODeviceType& innerType;
    std::unique_ptr<juce::AudioIODevice> inner;
    juce::String outputName, inputName;
    Kind kind;
    juce::AudioIODeviceCallback* user = nullptr; // set before the inner device starts, cleared once it stopped

    bool forcing() const { return viaPipeWire() && pipewire::forceQuantum().load(); }

    /// JUCE's ALSA says "no channels" of a card another program holds: where PipeWire runs, it holds them all.
    juce::String explained(const juce::String& error) const
    {
        if (error.isEmpty() || kind != Kind::alsa || viaPipeWire() || !linuxaudio::pipewireRunning())
            return error;
        return error + " (PipeWire holds the sound cards: play through it, with the PipeWire device type, or ALSA's "
                       "PipeWire device)";
    }

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int n,
                                          const juce::AudioIODeviceCallbackContext& context) override
    {
        if (user != nullptr)
            user->audioDeviceIOCallbackWithContext(in, numIn, out, numOut, n, context);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override
    {
        if (user != nullptr)
            user->audioDeviceAboutToStart(this);
    }
    void audioDeviceStopped() override
    {
        if (user != nullptr)
            user->audioDeviceStopped();
    }
    void audioDeviceError(const juce::String& message) override
    {
        if (user != nullptr)
            user->audioDeviceError(message);
    }
};

/// JUCE's type around its devices; its list, its listeners and its name its own.
class QuantumType final : public juce::AudioIODeviceType, private juce::AudioIODeviceType::Listener {
public:
    QuantumType(std::unique_ptr<juce::AudioIODeviceType> t, Kind k) : juce::AudioIODeviceType(t->getTypeName()), inner(std::move(t)), kind(k)
    {
        inner->addListener(this);
    }
    ~QuantumType() override { inner->removeListener(this); }

    void scanForDevices() override { inner->scanForDevices(); }
    juce::StringArray getDeviceNames(bool wantInputNames) const override { return inner->getDeviceNames(wantInputNames); }
    int getDefaultDeviceIndex(bool forInput) const override { return inner->getDefaultDeviceIndex(forInput); }
    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        auto* own = dynamic_cast<QuantumDevice*>(device);
        return inner->getIndexOfDevice(own != nullptr ? own->innerDevice() : device, asInput);
    }
    bool hasSeparateInputsAndOutputs() const override { return inner->hasSeparateInputsAndOutputs(); }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName, const juce::String& inputDeviceName) override
    {
        std::unique_ptr<juce::AudioIODevice> device(inner->createDevice(outputDeviceName, inputDeviceName));
        return device != nullptr ? new QuantumDevice(*inner, std::move(device), outputDeviceName, inputDeviceName, kind) : nullptr;
    }

private:
    std::unique_ptr<juce::AudioIODeviceType> inner;
    Kind kind;

    void audioDeviceListChanged() override { callDeviceChangeListeners(); }
};

} // namespace

void DeviceManager::createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    if (auto pipewire = pipewire::createDeviceType(true))
        types.add(pipewire.release());
    juce::OwnedArray<juce::AudioIODeviceType> stock;
    juce::AudioDeviceManager::createAudioDeviceTypes(stock);
    while (!stock.isEmpty()) {
        std::unique_ptr<juce::AudioIODeviceType> type(stock.removeAndReturn(0));
        const auto name = type->getTypeName();
        if (name == "ALSA")
            types.add(new QuantumType(std::move(type), Kind::alsa));
        else if (name == "JACK")
            types.add(new QuantumType(std::move(type), Kind::jack));
        else
            types.add(type.release());
    }
}

bool throughPipeWire(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return false;
    const auto type = device->getTypeName();
    if (type == pipewire::kTypeName)
        return true;
    if (type == "ALSA")
        return alsaThroughPipeWire(device->getName());
    if (type == "JACK")
        return linuxaudio::pipewireJack();
    return false;
}

#else

void DeviceManager::createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    juce::AudioDeviceManager::createAudioDeviceTypes(types);
}

bool throughPipeWire(juce::AudioIODevice*)
{
    return false;
}

#endif

} // namespace pf::app
