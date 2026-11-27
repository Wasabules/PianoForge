// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Meters.h"

#include <cmath>

namespace pf::app {

namespace {

constexpr float kFloorDb = -60.0f;

float toPosition(float gain)
{
    const float db = juce::Decibels::gainToDecibels(gain, kFloorDb);
    return juce::jlimit(0.0f, 1.0f, (db - kFloorDb) / -kFloorDb);
}

} // namespace

// ---- LevelMeter ----

void LevelMeter::push(float left, float right)
{
    const float in[2] = { left, right };
    for (int c = 0; c < 2; ++c) {
        level[(size_t) c] = juce::jmax(in[c], level[(size_t) c] * 0.80f); // falls about 20 dB per second
        if (in[c] >= hold[(size_t) c]) {
            hold[(size_t) c] = in[c];
            holdAge[(size_t) c] = 0;
        } else if (++holdAge[(size_t) c] > 30) { // held 1.5 s, then falls
            hold[(size_t) c] *= 0.85f;
        }
        clipped = clipped || in[c] >= 1.0f;
    }
    repaint();
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(theme::panel.darker(0.3f));
    g.fillRoundedRectangle(area, 3.0f);
    auto bars = area.reduced(3.0f);
    const float barHeight = (bars.getHeight() - 2.0f) / 2.0f;
    for (int c = 0; c < 2; ++c) {
        auto bar = bars.withHeight(barHeight).translated(0.0f, c * (barHeight + 2.0f));
        const float pos = toPosition(level[(size_t) c]);
        juce::ColourGradient gradient(theme::good, bar.getX(), 0.0f, theme::bad, bar.getRight(), 0.0f, false);
        gradient.addColour(0.75, theme::good);
        gradient.addColour(0.9, theme::busy);
        g.setGradientFill(gradient);
        g.fillRect(bar.withWidth(bar.getWidth() * pos));
        g.setColour(theme::text.withAlpha(0.8f));
        g.fillRect(bar.getX() + bar.getWidth() * toPosition(hold[(size_t) c]) - 1.0f, bar.getY(), 2.0f, bar.getHeight());
    }
    if (clipped) {
        g.setColour(theme::bad);
        g.setFont(theme::font(10.0f, true));
        g.drawText("CLIP", getLocalBounds().reduced(4, 0), juce::Justification::centredRight);
    }
}

// ---- LoadMeter ----

void LoadMeter::set(float newAverage, float newPeak, int newDropouts)
{
    average = newAverage;
    peak = newPeak;
    dropouts = newDropouts;
    repaint();
}

void LoadMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto textArea = area.removeFromTop(16);
    g.setFont(theme::font(11.0f, true));
    g.setColour(theme::dim);
    g.drawText("AUDIO LOAD", textArea, juce::Justification::centredLeft);
    g.setColour(theme::forLoad(peak));
    g.drawText(juce::String(juce::roundToInt(average * 100.0f)) + " %   peak " + juce::String(juce::roundToInt(peak * 100.0f)) + " %",
               textArea, juce::Justification::centredRight);
    area.removeFromTop(3);
    auto bar = area.removeFromTop(10).toFloat();
    // The zones, faint; the average, filled; the peak, a mark.
    g.setColour(theme::good.withAlpha(0.15f));
    g.fillRect(bar.withWidth(bar.getWidth() * theme::kBusyLoad));
    g.setColour(theme::busy.withAlpha(0.15f));
    g.fillRect(bar.withX(bar.getX() + bar.getWidth() * theme::kBusyLoad).withWidth(bar.getWidth() * (theme::kOverloaded - theme::kBusyLoad)));
    g.setColour(theme::bad.withAlpha(0.15f));
    g.fillRect(bar.withX(bar.getX() + bar.getWidth() * theme::kOverloaded).withWidth(bar.getWidth() * (1.0f - theme::kOverloaded)));
    g.setColour(theme::forLoad(average));
    g.fillRect(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, average)));
    g.setColour(theme::text);
    g.fillRect(bar.getX() + bar.getWidth() * juce::jlimit(0.0f, 1.0f, peak) - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f);
    area.removeFromTop(3);
    g.setFont(theme::font(11.0f));
    g.setColour(dropouts > 0 ? theme::bad : theme::dim);
    g.drawText(dropouts == 0 ? juce::String("no dropout") : juce::String(dropouts) + (dropouts == 1 ? " dropout" : " dropouts"),
               area, juce::Justification::centredLeft);
    g.setColour(theme::dim);
    g.drawText("details...", area, juce::Justification::centredRight);
}

// ---- LoadGraph ----

void LoadGraph::push(float peak, bool dropout)
{
    peaks[(size_t) next] = peak;
    dropouts[(size_t) next] = dropout;
    next = (next + 1) % kColumns;
    filled = juce::jmin(filled + 1, kColumns);
    repaint();
}

void LoadGraph::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(theme::panel.darker(0.3f));
    g.fillRoundedRectangle(area, 4.0f);
    auto plot = area.reduced(6.0f);
    auto yFor = [&](float load) { return plot.getBottom() - plot.getHeight() * juce::jlimit(0.0f, 1.0f, load); };
    // Zone lines and labels.
    for (const float level : { theme::kBusyLoad, theme::kOverloaded, 1.0f }) {
        g.setColour(theme::forLoad(level - 0.01f).withAlpha(0.35f));
        g.drawHorizontalLine((int) yFor(level), plot.getX(), plot.getRight());
        g.setFont(theme::font(10.0f));
        const float labelY = level >= 1.0f ? yFor(level) + 1.0f : yFor(level) - 12.0f; // the top one below its line
        g.drawText(juce::String(juce::roundToInt(level * 100.0f)) + " %", plot.withY(labelY).withHeight(12.0f),
                   juce::Justification::topRight);
    }
    const float w = plot.getWidth() / kColumns;
    for (int i = 0; i < filled; ++i) {
        const int index = (next - filled + i + kColumns) % kColumns;
        const float x = plot.getX() + (kColumns - filled + i) * w;
        const float load = peaks[(size_t) index];
        g.setColour(theme::forLoad(load));
        g.fillRect(x, yFor(load), juce::jmax(1.0f, w), plot.getBottom() - yFor(load));
        if (dropouts[(size_t) index]) {
            g.setColour(theme::bad);
            g.fillRect(x, plot.getY(), juce::jmax(2.0f, w), plot.getHeight());
        }
    }
    g.setColour(theme::dim);
    g.setFont(theme::font(10.0f));
    g.drawText("last minute", plot, juce::Justification::topLeft);
}

// ---- InfoCard ----

void InfoCard::setLines(const juce::StringArray& newLines, juce::Colour accentLine)
{
    if (newLines == lines && accentLine == firstColour)
        return;
    lines = newLines;
    firstColour = accentLine;
    repaint();
}

void InfoCard::paint(juce::Graphics& g)
{
    auto inner = theme::drawCard(g, getLocalBounds(), title);
    for (int i = 0; i < lines.size(); ++i) {
        const bool first = i == 0;
        g.setColour(first ? firstColour : theme::dim.brighter(0.25f));
        g.setFont(theme::font(first ? 15.0f : 12.5f, first));
        g.drawFittedText(lines[i], inner.removeFromTop(first ? 22 : 18), juce::Justification::centredLeft, 1, 0.9f);
    }
}

} // namespace pf::app
