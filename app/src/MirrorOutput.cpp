#include "MirrorOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace pf::app {

// ---- DriftResampler ----

void DriftResampler::prepare(double inputRate, double outputRate, int maxInputBlock, int maxOutputBlock)
{
    inRate = inputRate;
    outRate = outputRate;
    baseRatio = inputRate / outputRate;
    // The fill level aimed at: one block of each side, half again for the jitter of their timing, plus 2 ms.
    target = 1.5 * ((double) maxInputBlock + (double) maxOutputBlock * baseRatio) + 0.002 * inputRate;
    const int capacity = (int) std::ceil(std::max(1.0, 1.0 * inputRate) + 8.0 * target); // 1 s and more
    fifo.setTotalSize(capacity);
    fifo.reset();
    fifoData.assign((std::size_t) capacity * 2, 0.0f);
    pending.assign((std::size_t) (capacity + 4 * kHalfTaps) * 2, 0.0f);
    pendingFrames = kHalfTaps; // history (silence) before the first frame read
    readPos = kHalfTaps;
    integral = 0.0;
    // Windowed sinc, cut at the lower of the two Nyquist frequencies (with 5 % of margin), Kaiser beta 8.
    const double cutoff = 0.95 * std::min(1.0, 1.0 / baseRatio);
    const double beta = 8.0;
    auto bessel0 = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    kernel.assign((std::size_t) (kPhases + 1) * 2 * kHalfTaps, 0.0f);
    for (int ph = 0; ph <= kPhases; ++ph) {
        const double frac = (double) ph / kPhases;
        double sum = 0.0;
        for (int j = 0; j < 2 * kHalfTaps; ++j) {
            const double x = (double) (j - kHalfTaps + 1) - frac; // tap j weighs frame i - kHalfTaps + 1 + j
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(3.141592653589793 * cutoff * x) / (3.141592653589793 * cutoff * x);
            const double u = x / kHalfTaps;
            const double window = std::abs(u) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - u * u)) / bessel0(beta);
            const double h = cutoff * sinc * window;
            kernel[(std::size_t) ph * 2 * kHalfTaps + (std::size_t) j] = (float) h;
            sum += h;
        }
        for (int j = 0; j < 2 * kHalfTaps; ++j) // unit gain at DC for every phase
            kernel[(std::size_t) ph * 2 * kHalfTaps + (std::size_t) j] = (float) (kernel[(std::size_t) ph * 2 * kHalfTaps + (std::size_t) j] / sum);
    }
    // A second of callbacks for the fill average.
    fills.assign((std::size_t) std::max(8, (int) std::ceil(outputRate / std::max(1, maxOutputBlock))), 0.0);
    fillAt = fillCount = 0;
    fillSum = 0.0;
    fillSmoothed = target;
    fillAverage = target;
    correctionValue = 0.0;
    playing = false;
    underrunCount = 0;
    overrunCount = 0;
}

void DriftResampler::push(const float* const* input, int numChannels, int numFrames) noexcept
{
    int start1, size1, start2, size2;
    fifo.prepareToWrite(numFrames, start1, size1, start2, size2);
    if (size1 + size2 < numFrames) {
        overrunCount.fetch_add(1, std::memory_order_relaxed); // the consumer stopped: the block is dropped
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

void DriftResampler::pull(float* const* output, int numChannels, int numFrames) noexcept
{
    // 1. Everything the producer has written, appended to the frames pending.
    {
        const int room = (int) (pending.size() / 2) - pendingFrames;
        const auto scope = fifo.read(std::min(fifo.getNumReady(), std::max(0, room)));
        auto append = [&](int start, int size) {
            if (size > 0)
                std::memcpy(pending.data() + (std::size_t) pendingFrames * 2, fifoData.data() + (std::size_t) start * 2,
                            (std::size_t) size * 2 * sizeof(float));
            pendingFrames += size;
        };
        append(scope.startIndex1, scope.blockSize1);
        append(scope.startIndex2, scope.blockSize2);
    }
    auto silence = [&](int from) {
        for (int c = 0; c < numChannels; ++c)
            if (output[c] != nullptr)
                std::fill(output[c] + from, output[c] + numFrames, 0.0f);
    };
    const double available = pendingFrames - readPos; // frames ahead of the read position
    // 2. Start once the target is reached; far above it (the consumer stalled), drop back to it.
    if (!playing.load(std::memory_order_relaxed)) {
        if (available < target + kHalfTaps) {
            silence(0);
            return;
        }
        playing = true;
        fillAt = fillCount = 0;
        fillSum = 0.0;
    }
    if (available > 6.0 * target + numFrames * baseRatio) {
        readPos += std::floor(available - target);
        overrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    // 3. The ratio: nominal, corrected by a PI loop on the fill level averaged over the last second (settles in
    //    about 20 s, damping 0.7).
    const double dt = numFrames / outRate;
    const double fill = pendingFrames - readPos;
    if (fillCount == fills.size())
        fillSum -= fills[fillAt];
    else
        ++fillCount;
    fills[fillAt] = fill;
    fillSum += fill;
    fillAt = (fillAt + 1) % fills.size();
    fillSmoothed = fillSum / (double) fillCount;
    const double error = (fillSmoothed - target) / target;
    integral = std::clamp(integral + error * dt, -30.0, 30.0);
    const double correction = std::clamp(0.002 * error + 0.0001 * integral, -kMaxCorrection, kMaxCorrection);
    const double ratio = baseRatio * (1.0 + correction);
    fillAverage.store(fillSmoothed, std::memory_order_relaxed);
    correctionValue.store(correction, std::memory_order_relaxed);
    // 4. Windowed-sinc interpolation from the frames pending.
    float* left = numChannels > 0 ? output[0] : nullptr;
    float* right = numChannels > 1 ? output[1] : nullptr;
    const float* p = pending.data();
    constexpr int taps = 2 * kHalfTaps;
    int k = 0;
    for (; k < numFrames; ++k) {
        const int i = (int) readPos;
        if (i + kHalfTaps >= pendingFrames) { // the producer is late: silence, and wait for the target again
            underrunCount.fetch_add(1, std::memory_order_relaxed);
            playing = false;
            break;
        }
        const double phase = (readPos - i) * kPhases;
        const int ph = std::min((int) phase, kPhases - 1);
        const float w = (float) (phase - ph);
        const float* h0 = kernel.data() + (std::size_t) ph * taps;
        const float* h1 = h0 + taps;
        const float* x = p + 2 * (i - kHalfTaps + 1);
        float l = 0.0f, r = 0.0f;
        for (int j = 0; j < taps; ++j) {
            const float h = h0[j] + w * (h1[j] - h0[j]);
            l += h * x[2 * j];
            r += h * x[2 * j + 1];
        }
        if (left != nullptr)
            left[k] = l;
        if (right != nullptr)
            right[k] = r;
        readPos += ratio;
    }
    silence(k);
    for (int c = 2; c < numChannels; ++c)
        if (output[c] != nullptr)
            std::fill(output[c], output[c] + numFrames, 0.0f);
    // 5. Drop what has been read, keeping the history the interpolator needs before the read position.
    const int consumed = std::clamp((int) readPos - kHalfTaps, 0, pendingFrames - kHalfTaps);
    if (consumed > 0) {
        std::memmove(pending.data(), pending.data() + (std::size_t) consumed * 2,
                     (std::size_t) (pendingFrames - consumed) * 2 * sizeof(float));
        pendingFrames -= consumed;
        readPos -= consumed;
    }
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
    resampler.prepare(inputRate, device->getCurrentSampleRate(), inputBlock, device->getCurrentBufferSizeSamples());
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
        resampler.push(outputs, numChannels, numSamples);
    inPush.fetch_sub(1);
}

void MirrorOutput::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    resampler.pull(outputs, numOutputs, numSamples);
}

juce::String MirrorOutput::status() const
{
    if (device == nullptr)
        return "Off";
    const double out = device->getCurrentSampleRate();
    const double latencyMs = 1000.0 * (resampler.targetSeconds()
                                       + (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples()) / std::max(out, 1.0));
    return juce::String(out / 1000.0, 1) + " kHz, blocks of " + juce::String(device->getCurrentBufferSizeSamples()) + ", ~"
         + juce::String(juce::roundToInt(latencyMs)) + " ms behind, drift " + juce::String(juce::roundToInt(resampler.correctionPpm())) + " ppm" + (resampler.running() ? juce::String() : juce::String(", waiting"))
         + (resampler.underruns() + resampler.overruns() > 0
                ? ", " + juce::String((int) (resampler.underruns() + resampler.overruns())) + " resyncs"
                : juce::String());
}

} // namespace pf::app
