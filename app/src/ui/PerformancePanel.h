// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../AudioHost.h"
#include "Meters.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>
#include <functional>

namespace pf::app {

/// Turns the audio host's raw measures into what the interface shows: the load now, its average and its peaks,
/// one column of history every 100 ms, and the dropouts (late blocks, gaps between callbacks, and those the driver
/// reports when it does: ASIO does, the Windows mixer does not). Message thread, about 20 times a second.
class LoadTracker {
public:
    struct Snapshot {
        float now = 0.0f, average = 0.0f, peakRecent = 0.0f, peakMinute = 0.0f; // fractions of the block's time
        double blockMs = 0.0;
        std::uint32_t lateBlocks = 0, gaps = 0;
        int driverDropouts = -1; // -1: the driver does not report them
        int dropouts = 0;        // all of them, since the last reset
        juce::Time since;
    };

    /// `deviceXRuns`: the device's own count (-1 when it has none).
    void update(AudioLoad& load, int deviceXRuns);
    /// A column of history every 100 ms (its peak, and whether a dropout happened in it): true when one is ready.
    bool takeColumn(float& peak, bool& dropout);
    void reset(AudioLoad& load, int deviceXRuns);
    const Snapshot& snapshot() const noexcept { return state; }

private:
    Snapshot state;
    double lastSum = 0.0;
    std::uint64_t lastCallbacks = 0;
    int xrunBase = 0;
    bool haveBase = false;
    std::deque<float> recent;  // the last 2 s of peaks (per update)
    std::deque<float> minute;  // the last minute of column peaks
    float columnPeak = 0.0f;
    int columnTicks = 0, lastDropouts = 0;
    bool columnDropout = false, columnReady = false;
    float readyPeak = 0.0f;
    bool readyDropout = false;
};

/// The audio load in detail, in its own window: the numbers, a minute of history, the dropouts and where they
/// come from, the engine's work, the device, and advice when the settings cost more than they bring.
class PerformancePanel final : public juce::Component {
public:
    PerformancePanel();
    void update(const LoadTracker::Snapshot& s, const juce::StringArray& engineLines, const juce::StringArray& deviceLines,
                const juce::StringArray& advice);
    void pushColumn(float peak, bool dropout) { graph.push(peak, dropout); }
    std::function<void()> onReset;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    LoadTracker::Snapshot snap;
    juce::StringArray engine, device, tips;
    LoadGraph graph;
    juce::TextButton resetButton { "Reset the dropout counters" };
    juce::Rectangle<int> numbersArea, graphArea, dropoutArea, engineArea, deviceArea, adviceArea;
};

} // namespace pf::app
