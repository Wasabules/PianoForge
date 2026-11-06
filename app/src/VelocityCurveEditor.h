#pragma once

#include "KeyboardInput.h"

#include <pianoforge/velocity_curve.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace pf::app {

/// Which curve an editor shows: the keyboard's velocities (1..127, note-ons) or its sustain pedal (0..127, CC64).
enum class CurveKind { velocity, pedal };

/// Graph of a curve: what the keyboard sends horizontally, what the engine receives vertically. The control
/// points are dragged vertically; the last key played (or the pedal's last value) shows as a dot. Velocities:
/// ticks mark the velocities rendered in the reference corpus. Pedal: the band where the dampers only graze the
/// strings (the half pedal, from the modal profile).
class VelocityCurveGraph final : public juce::Component {
public:
    std::function<void()> onEdit;

    VelocityCurveGraph(pf::VelocityCurve::Settings& s, const KeyboardInput& k, CurveKind c) : settings(s), keyboard(k), kind(c) {}

    void setRenderedLayers(std::vector<int> layers) { renderedLayers = std::move(layers); repaint(); }
    /// The damper lift (0 resting, 1 lifted) per pedal value the engine receives: the half-pedal band.
    void setDamperLift(const std::array<float, 128>& lift) { damperLift = lift; hasLift = true; repaint(); }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override { dragged = -1; }

private:
    int lowest() const { return kind == CurveKind::pedal ? 0 : 1; }
    int output(int input) const;
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toScreen(double input, double output) const;
    juce::Point<float> pointPosition(int i) const;

    pf::VelocityCurve::Settings& settings;
    const KeyboardInput& keyboard;
    CurveKind kind;
    std::vector<int> renderedLayers;
    std::array<float, 128> damperLift {};
    bool hasLift = false;
    int dragged = -1;
};

/// Curve window content: graph, input range, shapes, range learning, live readout.
class VelocityCurvePanel final : public juce::Component, private juce::Timer {
public:
    /// `changed` is called on the message thread each time the settings change.
    VelocityCurvePanel(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> changed, CurveKind kind = CurveKind::velocity);

    void setRenderedLayers(std::vector<int> layers) { graph.setRenderedLayers(std::move(layers)); }
    void setDamperLift(const std::array<float, 128>& lift) { graph.setDamperLift(lift); }
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    void notify();
    void syncControls();

    pf::VelocityCurve::Settings& settings;
    KeyboardInput& keyboard;
    std::function<void()> changed;
    CurveKind kind;
    VelocityCurveGraph graph;
    juce::Slider inputMin, inputMax;
    juce::Label inputMinLabel, inputMaxLabel, readout, explanation;
    juce::TextButton linear { "Linear" }, soft, hard, learn { "Learn range" };
    std::uint32_t lastCount = 0;
};

/// Free-standing window that keeps its content when closed (it is only hidden).
class VelocityCurveWindow final : public juce::DocumentWindow {
public:
    VelocityCurveWindow(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> changed, CurveKind kind = CurveKind::velocity);
    VelocityCurvePanel& panel() { return *content; }
    void closeButtonPressed() override { setVisible(false); }

private:
    VelocityCurvePanel* content;
};

} // namespace pf::app
