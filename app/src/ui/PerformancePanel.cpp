// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PerformancePanel.h"

#include "Theme.h"

namespace pf::app {

// ---- LoadTracker ----

void LoadTracker::update(AudioLoad& load, int deviceXRuns)
{
    const auto callbacks = load.callbacks.load(std::memory_order_acquire);
    const double sum = load.loadSum.load(std::memory_order_relaxed);
    const float peak = load.peak.exchange(0.0f);
    if (callbacks > lastCallbacks)
        state.average += 0.15f * ((float) ((sum - lastSum) / (double) (callbacks - lastCallbacks)) - state.average);
    lastCallbacks = callbacks;
    lastSum = sum;
    state.now = load.last.load(std::memory_order_relaxed);
    state.blockMs = 1000.0 * load.blockSeconds.load();
    recent.push_back(peak);
    while (recent.size() > 40) // 2 s at 20 updates per second
        recent.pop_front();
    state.peakRecent = 0.0f;
    for (const float p : recent)
        state.peakRecent = juce::jmax(state.peakRecent, p);

    if (!haveBase) {
        xrunBase = juce::jmax(0, deviceXRuns);
        haveBase = true;
        state.since = juce::Time::getCurrentTime();
    }
    state.lateBlocks = load.lateBlocks.load();
    state.gaps = load.gaps.load();
    state.driverDropouts = deviceXRuns < 0 ? -1 : juce::jmax(0, deviceXRuns - xrunBase);
    state.dropouts = (int) (state.lateBlocks + state.gaps) + juce::jmax(0, state.driverDropouts);

    columnPeak = juce::jmax(columnPeak, peak);
    columnDropout = columnDropout || state.dropouts != lastDropouts;
    lastDropouts = state.dropouts;
    if (++columnTicks >= 2) { // 100 ms
        readyPeak = columnPeak;
        readyDropout = columnDropout;
        columnReady = true;
        minute.push_back(columnPeak);
        while (minute.size() > 600)
            minute.pop_front();
        state.peakMinute = 0.0f;
        for (const float p : minute)
            state.peakMinute = juce::jmax(state.peakMinute, p);
        columnPeak = 0.0f;
        columnDropout = false;
        columnTicks = 0;
    }
}

bool LoadTracker::takeColumn(float& peak, bool& dropout)
{
    if (!columnReady)
        return false;
    peak = readyPeak;
    dropout = readyDropout;
    columnReady = false;
    return true;
}

void LoadTracker::reset(AudioLoad& load, int deviceXRuns)
{
    load.lateBlocks = 0;
    load.gaps = 0;
    xrunBase = juce::jmax(0, deviceXRuns);
    state.since = juce::Time::getCurrentTime();
    state.dropouts = lastDropouts = 0;
    minute.clear();
    state.peakMinute = 0.0f;
}

// ---- PerformancePanel ----

PerformancePanel::PerformancePanel()
{
    addAndMakeVisible(graph);
    addAndMakeVisible(resetButton);
    resetButton.onClick = [this] {
        if (onReset)
            onReset();
    };
    setSize(720, 640);
}

void PerformancePanel::update(const LoadTracker::Snapshot& s, const juce::StringArray& engineLines,
                              const juce::StringArray& deviceLines, const juce::StringArray& advice)
{
    snap = s;
    engine = engineLines;
    device = deviceLines;
    tips = advice;
    repaint();
}

void PerformancePanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);

    // The numbers.
    auto inner = theme::drawCard(g, numbersArea, "Audio thread load");
    const struct {
        const char* label;
        float value;
    } numbers[] = { { "now", snap.now }, { "average", snap.average }, { "peak, 2 s", snap.peakRecent }, { "peak, 1 min", snap.peakMinute } };
    auto row = inner.removeFromTop(44);
    const int width = row.getWidth() / 4;
    for (const auto& n : numbers) {
        auto cell = row.removeFromLeft(width);
        g.setColour(theme::forLoad(n.value));
        g.setFont(theme::font(26.0f, true));
        g.drawText(juce::String(juce::roundToInt(n.value * 100.0f)) + " %", cell.removeFromTop(30), juce::Justification::centredLeft);
        g.setColour(theme::dim);
        g.setFont(theme::font(11.0f));
        g.drawText(n.label, cell, juce::Justification::centredLeft);
    }
    g.setColour(theme::text);
    g.setFont(theme::font(13.0f));
    g.drawText("Each block lasts " + juce::String(snap.blockMs, 1) + " ms: the engine uses " + juce::String(snap.average * snap.blockMs, 2)
                   + " ms of it on average, " + juce::String(snap.peakMinute * snap.blockMs, 2) + " ms at most over the last minute.",
               inner.removeFromTop(20), juce::Justification::centredLeft);
    g.setColour(theme::dim);
    g.setFont(theme::font(12.0f));
    g.drawText("Under 50 %: comfortable.  50 to 80 %: busy, a spike of the system can make a block late.  Over 80 %: dropouts likely.",
               inner.removeFromTop(18), juce::Justification::centredLeft);

    // Dropouts.
    inner = theme::drawCard(g, dropoutArea, "Dropouts");
    g.setColour(snap.dropouts > 0 ? theme::bad : theme::good);
    g.setFont(theme::font(15.0f, true));
    g.drawText(snap.dropouts == 0 ? "None since " + snap.since.formatted("%H:%M:%S")
                                  : juce::String(snap.dropouts) + " since " + snap.since.formatted("%H:%M:%S"),
               inner.removeFromTop(22), juce::Justification::centredLeft);
    g.setColour(theme::text);
    g.setFont(theme::font(12.5f));
    g.drawText("Late blocks (the engine took longer than the block): " + juce::String((int) snap.lateBlocks), inner.removeFromTop(18),
               juce::Justification::centredLeft);
    g.drawText("Gaps between callbacks (the driver or the system held the audio thread back): " + juce::String((int) snap.gaps),
               inner.removeFromTop(18), juce::Justification::centredLeft);
    g.drawText(snap.driverDropouts < 0 ? juce::String("Reported by the driver: this driver type does not report them")
                                       : "Reported by the driver: " + juce::String(snap.driverDropouts),
               inner.removeFromTop(18), juce::Justification::centredLeft);

    auto lines = [&](juce::Rectangle<int> area, const char* title, const juce::StringArray& text, juce::Colour colour) {
        auto box = theme::drawCard(g, area, title);
        g.setFont(theme::font(12.5f));
        g.setColour(colour);
        for (const auto& line : text)
            g.drawFittedText(line, box.removeFromTop(18), juce::Justification::centredLeft, 1, 0.85f);
    };
    lines(engineArea, "Engine", engine, theme::text);
    lines(deviceArea, "Device", device, theme::text);
    lines(adviceArea, "Advice", tips.isEmpty() ? juce::StringArray { "Nothing to change: the settings suit the engine." } : tips,
          tips.isEmpty() ? theme::good : theme::busy);
}

void PerformancePanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    numbersArea = area.removeFromTop(126);
    area.removeFromTop(10);
    graphArea = area.removeFromTop(150);
    graph.setBounds(graphArea);
    area.removeFromTop(10);
    dropoutArea = area.removeFromTop(112);
    resetButton.setBounds(dropoutArea.reduced(12, 8).removeFromTop(22).removeFromRight(210));
    area.removeFromTop(10);
    auto row = area.removeFromTop(78);
    engineArea = row.removeFromLeft(row.getWidth() / 2 - 5);
    row.removeFromLeft(10);
    deviceArea = row;
    area.removeFromTop(10);
    adviceArea = area;
}

} // namespace pf::app
