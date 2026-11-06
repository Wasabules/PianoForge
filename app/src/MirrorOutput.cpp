#include "MirrorOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace pf::app {

// ---- DriftFifo ----

void DriftFifo::prepare(double inputRate, double outputRate, int maxInputBlock, int maxOutputBlock)
{
    resampler.prepare(inputRate, outputRate, maxInputBlock, maxOutputBlock);
    const int capacity = (int) std::ceil(inputRate) + 8 * (maxInputBlock + maxOutputBlock); // a second and more
    fifo.setTotalSize(capacity);
    fifo.reset();
    fifoData.assign((std::size_t) capacity * 2, 0.0f);
    dropped = 0;
}

void DriftFifo::push(const float* const* input, int numChannels, int numFrames) noexcept
{
    int start1, size1, start2, size2;
    fifo.prepareToWrite(numFrames, start1, size1, start2, size2);
    if (size1 + size2 < numFrames) {
        dropped.fetch_add(1, std::memory_order_relaxed); // the consumer stopped: the block is dropped
        return;
    }
    auto copy = [&](int start, int size, int offset) {
        float* d = fifoData.data() + (std::size_t) start * 2;
        const float* l = numChannels > 0 && input[0] != nullptr ? input[0] + offset : nullptr;
        const float* r = numChannels > 1 && input[1] != nullptr ? input[1] + offset : l;
        for (int k = 0; k < size; ++k) {
            d[2 * k] = l != nullptr ? l[k] : 0.0f;
            d[2 * k + 1] = r != nullptr ? r[k] : 0.0f;
        }
    };
    copy(start1, size1, 0);
    if (size2 > 0)
        copy(start2, size2, size1);
    fifo.finishedWrite(size1 + size2);
}

void DriftFifo::pull(float* const* output, int numChannels, int numFrames) noexcept
{
    const auto scope = fifo.read(std::min(fifo.getNumReady(), std::max(0, resampler.room())));
    if (scope.blockSize1 > 0)
        resampler.append(fifoData.data() + (std::size_t) scope.startIndex1 * 2, scope.blockSize1);
    if (scope.blockSize2 > 0)
        resampler.append(fifoData.data() + (std::size_t) scope.startIndex2 * 2, scope.blockSize2);
    resampler.render(output, numChannels, numFrames);
}

// ---- MirrorOutput ----

std::unique_ptr<juce::AudioIODeviceType> MirrorOutput::createType(bool lowLatency)
{
#if JUCE_WINDOWS
    return std::unique_ptr<juce::AudioIODeviceType>(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
        lowLatency ? juce::WASAPIDeviceMode::sharedLowLatency : juce::WASAPIDeviceMode::shared));
#elif JUCE_LINUX
    return std::unique_ptr<juce::AudioIODeviceType>(juce::AudioIODeviceType::createAudioIODeviceType_ALSA());
#else
    (void) lowLatency;
    return nullptr;
#endif
}

juce::StringArray MirrorOutput::deviceNames()
{
    auto t = createType(false);
    if (t == nullptr)
        return {};
    t->scanForDevices();
    return t->getDeviceNames(false);
}

juce::Result MirrorOutput::open(const juce::String& deviceName, double inputRate, int inputBlock)
{
    close();
    if (inputRate <= 0.0 || inputBlock <= 0)
        return juce::Result::fail("the main audio device is not running");
    // The shared mixer's low-latency mode (short periods) when the device has it, else the plain shared mode.
    juce::String error = "cannot find " + deviceName;
    for (const bool lowLatency : { true, false }) {
        type = createType(lowLatency);
        if (type == nullptr)
            return juce::Result::fail("no shared audio system on this platform");
        type->scanForDevices();
        device.reset(type->createDevice(deviceName, {}));
        if (device == nullptr)
            continue;
        // Same rate as the main device when the mirror offers it (only the clocks' drift to follow), else its own.
        const auto rates = device->getAvailableSampleRates();
        const double rate = rates.contains(inputRate) ? inputRate : (rates.contains(48000.0) ? 48000.0 : (rates.isEmpty() ? 48000.0 : rates[0]));
        const auto sizes = device->getAvailableBufferSizes();
        const int size = lowLatency && !sizes.isEmpty() ? sizes[0] : device->getDefaultBufferSize();
        juce::BigInteger outputs;
        outputs.setRange(0, 2, true);
        error = device->open({}, outputs, rate, size);
        if (error.isEmpty())
            break;
        device.reset();
    }
    if (device == nullptr)
        return juce::Result::fail(error);
    stream.prepare(inputRate, device->getCurrentSampleRate(), inputBlock, device->getCurrentBufferSizeSamples());
    openedRate = inputRate;
    openedBlock = inputBlock;
    device->start(this);
    accepting = true; // from here the main callback copies its blocks
    return juce::Result::ok();
}

void MirrorOutput::close()
{
    accepting = false;
    while (inPush.load() != 0) // a copy in progress on the main audio thread finishes first
        std::this_thread::yield();
    if (device != nullptr) {
        device->stop();
        device->close();
        device.reset();
    }
    type.reset();
}

void MirrorOutput::push(const float* const* outputs, int numChannels, int numSamples) noexcept
{
    if (!accepting.load())
        return;
    inPush.fetch_add(1);
    if (accepting.load() && numSamples <= openedBlock)
        stream.push(outputs, numChannels, numSamples);
    inPush.fetch_sub(1);
}

void MirrorOutput::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    stream.pull(outputs, numOutputs, numSamples);
}

juce::String MirrorOutput::status() const
{
    if (device == nullptr)
        return "Off";
    const double out = device->getCurrentSampleRate();
    const double latencyMs = 1000.0 * (stream.targetSeconds()
                                       + (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples()) / std::max(out, 1.0));
    return juce::String(out / 1000.0, 1) + " kHz, blocks of " + juce::String(device->getCurrentBufferSizeSamples()) + ", ~"
         + juce::String(juce::roundToInt(latencyMs)) + " ms behind, drift " + juce::String(juce::roundToInt(stream.correctionPpm())) + " ppm" + (stream.running() ? juce::String() : juce::String(", waiting"))
         + (stream.underruns() + stream.overruns() > 0
                ? ", " + juce::String((int) (stream.underruns() + stream.overruns())) + " resyncs"
                : juce::String());
}

} // namespace pf::app
