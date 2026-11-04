#pragma once

#include "KeyboardInput.h"

#include <pianoforge/velocity_curve.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pf::app {

/// Graph of the velocity curve: input velocity (what the keyboard sends) horizontally, output velocity
/// (what the engine receives) vertically. The control points are dragged vertically; the last key played
/// shows as a dot; horizontal ticks mark the velocities rendered in the reference corpus.
class VelocityCurveGraph final : public juce::Component {
public:
    std::function<void()> onEdit;

    VelocityCurveGraph(pf::VelocityCurve::Settings& s, const KeyboardInput& k) : settings(s), keyboard(k) {}

    void setRenderedLayers(std::vector<int> layers) { renderedLayers = std::move(layers); repaint(); }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override { dragged = -1; }

private:
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toScreen(double input, double output) const;
    juce::Point<float> pointPosition(int i) const;

    pf::VelocityCurve::Settings& settings;
    const KeyboardInput& keyboard;
    std::vector<int> renderedLayers;
    int dragged = -1;
};

/// Velocity curve window content: graph, input range, shapes, range learning, live readout.
class VelocityCurvePanel final : public juce::Component, private juce::Timer {
public:
    /// `changed` is called on the message thread each time the settings change.
    VelocityCurvePanel(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> changed);

    void setRenderedLayers(std::vector<int> layers) { graph.setRenderedLayers(std::move(layers)); }
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    void notify();
    void syncControls();

    pf::VelocityCurve::Settings& settings;
    KeyboardInput& keyboard;
    std::function<void()> changed;
    VelocityCurveGraph graph;
    juce::Slider inputMin, inputMax;
    juce::Label inputMinLabel, inputMaxLabel, readout, explanation;
    juce::TextButton linear { "Linear" }, soft { "Soft touch" }, hard { "Hard touch" }, learn { "Learn range" };
    std::uint32_t lastCount = 0;
};

/// Free-standing window that keeps its content when closed (it is only hidden).
class VelocityCurveWindow final : public juce::DocumentWindow {
public:
    VelocityCurveWindow(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> changed);
    VelocityCurvePanel& panel() { return *content; }
    void closeButtonPressed() override { setVisible(false); }

private:
    VelocityCurvePanel* content;
};

} // namespace pf::app
