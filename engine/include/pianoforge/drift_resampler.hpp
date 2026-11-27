// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pf {

/// The consumer side of a stereo stream between two audio clocks. Frames are appended as they arrive from the
/// producer's clock and rendered on the consumer's, through a windowed-sinc interpolator (16 taps, Kaiser window,
/// cut at the lower Nyquist frequency) whose ratio follows the two clocks. Two devices never run at exactly the same
/// rate (tens to hundreds of ppm apart): a fixed ratio would starve or overflow within minutes. The ratio is the
/// nominal one, corrected by a PI loop on the frames waiting, averaged over about a second (the blocks of both
/// sides make the fill level a sawtooth): at most +-0.3 %, a few ppm once settled, inaudible. The loop locks fast
/// (settled within seconds, the correction a little nervous), then slows down over kLockSeconds to its steady gains
/// (calm, slow to move); its integral is kept as a correction, so that the change of gains is seamless.
///
/// The fill level is a sawtooth: each of the producer's blocks raises it at once, the consumer lowers it frame by
/// frame. A producer whose blocks arrive in bursts (a timer of 15.6 ms, a busy machine) makes it a ragged one, which
/// a second of averaging does not smooth enough: the loop wanders (by a hundred ppm, a wobble of the pitch). A
/// consumer that knows when the producer wrote its last block (a shared clock) gives render() the frames produced
/// since then at the nominal rate, less their average (fillOffset): the sawtooth taken out, the fill's average kept.
///
/// append() and render() run on the consumer's thread, without allocation or lock; prepare() allocates. The
/// statistics may be read from any thread.
class DriftResampler {
public:
    static constexpr int kHalfTaps = 8, kPhases = 512;

    /// `extraSeconds`: more frames kept waiting than the two block sizes call for (a consumer woken by a timer).
    void prepare(double inputRate, double outputRate, int maxInputBlock, int maxOutputBlock, double extraSeconds = 0.0)
    {
        inRate = inputRate;
        outRate = outputRate;
        baseRatio = inputRate / outputRate;
        rateMeasured = false;
        target = 1.5 * ((double) maxInputBlock + (double) maxOutputBlock * baseRatio) + (0.002 + extraSeconds) * inputRate;
        capacity = (int) std::ceil(std::max(inputRate, 8.0 * target)) + 4 * kHalfTaps;
        pending.assign((std::size_t) capacity * 2, 0.0f);
        // Windowed sinc cut at the lower of the two Nyquist frequencies (5 % of margin), Kaiser beta 8.
        const double cutoff = 0.95 * std::min(1.0, 1.0 / baseRatio), beta = 8.0;
        auto bessel0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 30; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        constexpr int taps = 2 * kHalfTaps;
        kernel.assign((std::size_t) (kPhases + 1) * taps, 0.0f);
        for (int ph = 0; ph <= kPhases; ++ph) {
            const double frac = (double) ph / kPhases;
            double row[taps], sum = 0.0;
            for (int j = 0; j < taps; ++j) {
                const double x = (double) (j - kHalfTaps + 1) - frac; // tap j weighs frame i - kHalfTaps + 1 + j
                const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
                const double u = x / kHalfTaps;
                const double window = std::abs(u) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - u * u)) / bessel0(beta);
                row[j] = cutoff * sinc * window;
                sum += row[j];
            }
            for (int j = 0; j < taps; ++j) // unit gain at DC for every phase
                kernel[(std::size_t) ph * taps + (std::size_t) j] = (float) (row[j] / sum);
        }
        fills.assign((std::size_t) std::max(8, (int) std::ceil(outputRate / std::max(1, maxOutputBlock))), 0.0);
        reset();
        underrunCount = 0;
        overrunCount = 0;
    }

    /// Back to silence, waiting for the target again (the producer stopped).
    void reset() noexcept
    {
        std::fill(pending.begin(), pending.end(), 0.0f);
        pendingFrames = kHalfTaps; // history (silence) before the first frame read
        readPos = kHalfTaps;
        integral = 0.0;
        playedSeconds = 0.0;
        fillAt = fillCount = 0;
        fillSum = 0.0;
        fillAverage = target;
        correctionValue = 0.0;
        lastRatio = baseRatio;
        playing = false;
    }

    /// The producer's rate as measured on the consumer's clock (from a shared clock's timestamps of its blocks, a
    /// line through them over seconds): the nominal ratio follows it (within 0.5 % of the nominal one), and the loop,
    /// left with the fill level alone, keeps only its proportional term, a calm one (the drift is the ratio's: an
    /// integral would only wind up the start's transients). The first time, the ratio moves by the drift found, a few
    /// hundred ppm at most: inaudible.
    void setProducerRate(double measured) noexcept
    {
        if (!(measured > 0.0) || outRate <= 0.0)
            return;
        const double nominal = inRate / outRate;
        baseRatio = std::clamp(measured / outRate, nominal * 0.995, nominal * 1.005);
        if (!rateMeasured)
            integral = 0.0;
        rateMeasured = true;
    }

    /// Frames append() can take now.
    int room() const noexcept { return capacity - pendingFrames; }
    /// Frames waiting, ahead of the read position.
    double waiting() const noexcept { return pendingFrames - readPos; }
    /// Frames render() can still produce before it runs dry (at the last ratio).
    int renderable() const noexcept
    {
        return std::max(0, (int) std::floor((pendingFrames - kHalfTaps - 1 - readPos) / std::max(lastRatio, 1e-9)));
    }
    /// Enough frames waiting to start (render() starts by itself; a consumer keeping its own timeline asks first).
    bool readyToStart() const noexcept { return waiting() >= target + kHalfTaps; }

    /// Interleaved stereo frames from the producer (at most room()).
    void append(const float* interleaved, int frames) noexcept
    {
        frames = std::clamp(frames, 0, room());
        std::memcpy(pending.data() + (std::size_t) pendingFrames * 2, interleaved, (std::size_t) frames * 2 * sizeof(float));
        pendingFrames += frames;
    }

    /// numFrames at the consumer's rate (silence until the target is reached, and after an underrun). fillOffset:
    /// the frames the producer has made since its last block less their average (see above), added to the fill level.
    void render(float* const* output, int numChannels, int numFrames, double fillOffset = 0.0) noexcept
    {
        auto silence = [&](int from) {
            for (int c = 0; c < numChannels; ++c)
                if (output[c] != nullptr)
                    std::fill(output[c] + from, output[c] + numFrames, 0.0f);
        };
        if (!playing.load(std::memory_order_relaxed)) {
            if (!readyToStart()) {
                silence(0);
                return;
            }
            playing = true;
            fillAt = fillCount = 0;
            fillSum = 0.0;
        }
        if (waiting() > 6.0 * target + numFrames * baseRatio) { // the consumer stalled: back to the target
            readPos += std::floor(waiting() - target);
            overrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        // The ratio: nominal, corrected by a PI loop on the fill level averaged over the last second. Fast gains
        // to lock (natural frequency about 1 rad/s, damping 0.75: settled in 5 s), blended over kLockSeconds into
        // the steady ones (0.07 rad/s, damping 0.75), the integral term carried over as it is.
        const double dt = numFrames / outRate;
        playedSeconds += dt;
        const double lock = rateMeasured ? 1.0 : std::clamp((playedSeconds - kLockSeconds) / kLockSeconds, 0.0, 1.0); // 0 fast, 1 steady
        // (The producer's rate measured, the loop only holds the fill level: proportional, calm.)
        const double kp = rateMeasured ? 0.00012 : 0.03 + lock * (0.002 - 0.03);
        const double ki = rateMeasured ? 0.0 : 0.02 + lock * (0.0001 - 0.02);
        if (fillCount == fills.size())
            fillSum -= fills[fillAt];
        else
            ++fillCount;
        fills[fillAt] = waiting() + fillOffset;
        fillSum += fills[fillAt];
        fillAt = (fillAt + 1) % fills.size();
        const double smoothed = fillSum / (double) fillCount;
        const double error = (smoothed - target) / target;
        integral = std::clamp(integral + ki * error * dt, -kMaxCorrection, kMaxCorrection); // as a correction
        const double correction = std::clamp(kp * error + integral, -kMaxCorrection, kMaxCorrection);
        const double ratio = baseRatio * (1.0 + correction);
        lastRatio = ratio;
        ratioValue.store(ratio, std::memory_order_relaxed);
        fillAverage.store(smoothed, std::memory_order_relaxed);
        correctionValue.store(correction, std::memory_order_relaxed);
        // Windowed-sinc interpolation, phases interpolated linearly.
        float* left = numChannels > 0 ? output[0] : nullptr;
        float* right = numChannels > 1 ? output[1] : nullptr;
        constexpr int taps = 2 * kHalfTaps;
        const float* p = pending.data();
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
        // Drop what has been read, keeping the history the interpolator needs.
        const int consumed = std::clamp((int) readPos - kHalfTaps, 0, pendingFrames - kHalfTaps);
        if (consumed > 0) {
            std::memmove(pending.data(), pending.data() + (std::size_t) consumed * 2,
                         (std::size_t) (pendingFrames - consumed) * 2 * sizeof(float));
            pendingFrames -= consumed;
            readPos -= consumed;
        }
    }

    double targetFrames() const noexcept { return target; }
    double targetSeconds() const noexcept { return inRate > 0.0 ? target / inRate : 0.0; }
    double fillSeconds() const noexcept { return inRate > 0.0 ? fillAverage.load() / inRate : 0.0; }
    double correctionPpm() const noexcept { return 1e6 * correctionValue.load(); }
    /// The ratio against the nominal one (the drift compensated, measured rate and loop together), at the last render.
    double ratioPpm() const noexcept { return inRate > 0.0 ? 1e6 * (ratioValue.load() * outRate / inRate - 1.0) : 0.0; }
    std::uint32_t underruns() const noexcept { return underrunCount.load(); }
    std::uint32_t overruns() const noexcept { return overrunCount.load(); }
    bool running() const noexcept { return playing.load(); }

private:
    static constexpr double kMaxCorrection = 0.003, kPi = 3.141592653589793;
    static constexpr double kLockSeconds = 6.0; // fast gains this long, then as long again to blend into the steady ones
    double inRate = 0.0, outRate = 0.0, baseRatio = 1.0, target = 0.0, lastRatio = 1.0;
    bool rateMeasured = false; // setProducerRate() gives the nominal ratio
    int capacity = 0;
    std::vector<float> kernel;   // (kPhases + 1) x 2 kHalfTaps
    std::vector<float> pending;  // interleaved stereo, frames not yet consumed
    int pendingFrames = 0;
    double readPos = 0.0;        // in pending frames (kHalfTaps frames of history before it)
    std::vector<double> fills;   // the fill level at the last renders (about a second of them)
    std::size_t fillAt = 0, fillCount = 0;
    double fillSum = 0.0, integral = 0.0, playedSeconds = 0.0;
    std::atomic<double> fillAverage { 0.0 }, correctionValue { 0.0 }, ratioValue { 0.0 };
    std::atomic<bool> playing { false };
    std::atomic<std::uint32_t> underrunCount { 0 }, overrunCount { 0 };
};

} // namespace pf
