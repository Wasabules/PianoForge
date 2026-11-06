#include "VelocityCurveEditor.h"

namespace pf::app {

namespace {
constexpr float kPointRadius = 7.0f;
const auto kCurveColour = juce::Colour(0xff4fc3f7);
const auto kGridColour = juce::Colours::white.withAlpha(0.12f);
} // namespace

// --- graph --------------------------------------------------------------------------------------------------

int VelocityCurveGraph::output(int input) const
{
    return kind == CurveKind::pedal ? pf::PedalCurve::output(settings, input) : pf::VelocityCurve::output(settings, input);
}

juce::Rectangle<float> VelocityCurveGraph::plotArea() const
{
    return getLocalBounds().toFloat().reduced(40.0f, 16.0f).withTrimmedBottom(14.0f);
}

juce::Point<float> VelocityCurveGraph::toScreen(double input, double out) const
{
    const auto a = plotArea();
    const double lo = lowest(), span = 127.0 - lo;
    return { a.getX() + (float) ((input - lo) / span) * a.getWidth(), a.getBottom() - (float) ((out - lo) / span) * a.getHeight() };
}

juce::Point<float> VelocityCurveGraph::pointPosition(int i) const
{
    const double lo = settings.inputMin, hi = juce::jmax(settings.inputMin + 1, settings.inputMax);
    const double input = lo + (hi - lo) * i / (pf::VelocityCurve::kPoints - 1);
    const double base = lowest();
    return toScreen(input, base + (127.0 - base) * settings.points[(size_t) i]);
}

void VelocityCurveGraph::paint(juce::Graphics& g)
{
    const auto a = plotArea();
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRoundedRectangle(a, 4.0f);

    // Pedal: the band where the dampers only graze the strings (the half pedal), and where they rest.
    if (kind == CurveKind::pedal && hasLift) {
        int first = -1, last = -1;
        for (int v = 0; v < 128; ++v)
            if (damperLift[(size_t) v] > 0.02f && damperLift[(size_t) v] < 0.98f) {
                first = first < 0 ? v : first;
                last = v;
            }
        if (first >= 0) {
            const auto top = toScreen(0, last + 0.5).y, bottom = toScreen(0, first - 0.5).y;
            g.setColour(juce::Colours::orange.withAlpha(0.12f));
            g.fillRect(juce::Rectangle<float>::leftTopRightBottom(a.getX(), top, a.getRight(), bottom));
            g.setColour(juce::Colours::orange.withAlpha(0.8f));
            g.setFont(juce::FontOptions(11.0f));
            g.drawText("half pedal (the dampers graze the strings)", juce::Rectangle<float>(a.getX() + 6.0f, top, a.getWidth() - 12.0f, bottom - top),
                       juce::Justification::centredLeft);
        }
    }

    g.setFont(juce::FontOptions(11.0f));
    const std::initializer_list<int> ticks = kind == CurveKind::pedal ? std::initializer_list<int> { 0, 32, 64, 96, 127 }
                                                                      : std::initializer_list<int> { 1, 32, 64, 96, 127 };
    for (int v : ticks) { // grid and axis labels
        g.setColour(kGridColour);
        const auto x = toScreen(v, lowest()).x, y = toScreen(lowest(), v).y;
        g.drawVerticalLine((int) x, a.getY(), a.getBottom());
        g.drawHorizontalLine((int) y, a.getX(), a.getRight());
        g.setColour(juce::Colours::grey);
        g.drawText(juce::String(v), juce::Rectangle<float>(x - 15.0f, a.getBottom() + 2.0f, 30.0f, 12.0f), juce::Justification::centred);
        g.drawText(juce::String(v), juce::Rectangle<float>(a.getX() - 34.0f, y - 6.0f, 30.0f, 12.0f), juce::Justification::centredRight);
    }
    g.drawText(kind == CurveKind::pedal ? "what the pedal sends (CC64)" : "keyboard velocity (input)",
               a.reduced(8.0f, 4.0f).removeFromBottom(12.0f), juce::Justification::centredRight); // inside: the ticks stay legible

    // Velocities rendered in the corpus: the take actually played is the closest one.
    g.setColour(juce::Colours::orange.withAlpha(0.7f));
    for (int v : renderedLayers) {
        const auto y = toScreen(lowest(), v).y;
        g.drawHorizontalLine((int) y, a.getRight() - 8.0f, a.getRight());
    }

    // Input range actually used by the keyboard.
    g.setColour(kCurveColour.withAlpha(0.08f));
    g.fillRect(juce::Rectangle<float>::leftTopRightBottom(toScreen(settings.inputMin, lowest()).x, a.getY(),
                                                          toScreen(settings.inputMax, lowest()).x, a.getBottom()));

    // The curve itself, through the exact mapping used.
    juce::Path curve;
    for (int v = lowest(); v <= 127; ++v) {
        const auto p = toScreen(v, output(v));
        if (v == lowest()) curve.startNewSubPath(p); else curve.lineTo(p);
    }
    g.setColour(kCurveColour);
    g.strokePath(curve, juce::PathStrokeType(2.0f));

    for (int i = 0; i < pf::VelocityCurve::kPoints; ++i) {
        const auto p = pointPosition(i);
        g.setColour(i == dragged ? juce::Colours::white : kCurveColour);
        g.fillEllipse(juce::Rectangle<float>(kPointRadius * 2, kPointRadius * 2).withCentre(p));
    }

    const int in = kind == CurveKind::pedal ? keyboard.lastPedalInput.load() : keyboard.lastInput.load();
    if (in >= lowest() && (kind == CurveKind::pedal || in > 0)) { // last key played, or the pedal's last value
        const auto p = toScreen(in, kind == CurveKind::pedal ? keyboard.lastPedalOutput.load() : keyboard.lastOutput.load());
        g.setColour(juce::Colours::limegreen.withAlpha(0.5f));
        g.drawVerticalLine((int) p.x, p.y, a.getBottom());
        g.drawHorizontalLine((int) p.y, a.getX(), p.x);
        g.setColour(juce::Colours::limegreen);
        g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre(p));
    }
}

void VelocityCurveGraph::mouseDown(const juce::MouseEvent& e)
{
    dragged = -1;
    float best = 3.0f * kPointRadius;
    for (int i = 0; i < pf::VelocityCurve::kPoints; ++i)
        if (const float d = pointPosition(i).getDistanceFrom(e.position); d < best) {
            best = d;
            dragged = i;
        }
    repaint();
}

void VelocityCurveGraph::mouseDrag(const juce::MouseEvent& e)
{
    if (dragged < 0)
        return;
    const auto a = plotArea();
    auto y = (float) juce::jlimit(0.0, 1.0, (double) ((a.getBottom() - e.position.y) / a.getHeight()));
    // Keep the points in order: a point cannot go below the previous one nor above the next one.
    if (dragged > 0) y = juce::jmax(y, settings.points[(size_t) dragged - 1]);
    if (dragged < pf::VelocityCurve::kPoints - 1) y = juce::jmin(y, settings.points[(size_t) dragged + 1]);
    settings.points[(size_t) dragged] = y;
    if (onEdit) onEdit();
    repaint();
}

// --- panel --------------------------------------------------------------------------------------------------

VelocityCurvePanel::VelocityCurvePanel(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> c, CurveKind kd)
    : settings(s), keyboard(k), changed(std::move(c)), kind(kd), graph(s, k, kd)
{
    const bool pedal = kind == CurveKind::pedal;
    graph.onEdit = [this] { notify(); };
    addAndMakeVisible(graph);

    for (auto* slider : { &inputMin, &inputMax }) {
        slider->setRange(pedal ? 0.0 : 1.0, 127.0, 1.0);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
        addAndMakeVisible(*slider);
    }
    inputMinLabel.setText(pedal ? "Pedal up sends at most" : "Softest keyboard velocity", juce::dontSendNotification);
    inputMaxLabel.setText(pedal ? "Pedal down sends at least" : "Loudest keyboard velocity", juce::dontSendNotification);
    addAndMakeVisible(inputMinLabel);
    addAndMakeVisible(inputMaxLabel);
    inputMin.onValueChange = [this] {
        settings.inputMin = juce::jmin((int) inputMin.getValue(), settings.inputMax - 1);
        notify();
    };
    inputMax.onValueChange = [this] {
        settings.inputMax = juce::jmax((int) inputMax.getValue(), settings.inputMin + 1);
        notify();
    };

    soft.setButtonText(pedal ? "Half pedal sooner" : "Soft touch");
    hard.setButtonText(pedal ? "Half pedal later" : "Hard touch");
    auto shape = [this](double exponent) {
        settings = pf::VelocityCurve::shaped(settings, exponent);
        notify();
    };
    linear.onClick = [shape] { shape(1.0); };
    soft.onClick = [shape] { shape(0.6); };
    hard.onClick = [shape] { shape(1.6); };
    learn.setClickingTogglesState(true);
    learn.onClick = [this, pedal] {
        if (learn.getToggleState()) {
            if (pedal)
                keyboard.startPedalLearning();
            else
                keyboard.startLearning();
            learn.setButtonText("Stop learning");
            return;
        }
        learn.setButtonText("Learn range");
        const int lo = pedal ? keyboard.pedalMin.load() : keyboard.learnedMin.load();
        const int hi = pedal ? keyboard.pedalMax.load() : keyboard.learnedMax.load();
        (pedal ? keyboard.pedalLearning : keyboard.learning) = false;
        if (hi > lo) {
            settings.inputMin = lo;
            settings.inputMax = hi;
            notify();
        }
    };
    for (auto* b : { &linear, &soft, &hard, &learn })
        addAndMakeVisible(*b);

    explanation.setText(pedal ? "Applies to the MIDI keyboard's sustain pedal only (not to Space nor to MIDI files). Learn "
                                "range: press the pedal slowly all the way down, let it up, then stop. Drag the points to "
                                "choose where the half pedal falls in the pedal's travel."
                              : "Applies to MIDI keyboards only (not to the mouse or the computer keyboard). Drag the points "
                                "to reshape the curve. Learn range: play your softest and loudest notes, then stop.",
                        juce::dontSendNotification);
    explanation.setColour(juce::Label::textColourId, juce::Colours::grey);
    explanation.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(explanation);
    addAndMakeVisible(readout);

    syncControls();
    setSize(620, 560);
    startTimerHz(20);
}

void VelocityCurvePanel::notify()
{
    syncControls();
    graph.repaint();
    if (changed)
        changed();
}

void VelocityCurvePanel::syncControls()
{
    inputMin.setValue(settings.inputMin, juce::dontSendNotification);
    inputMax.setValue(settings.inputMax, juce::dontSendNotification);
}

void VelocityCurvePanel::timerCallback()
{
    juce::String text;
    if (kind == CurveKind::pedal) {
        if (const int in = keyboard.lastPedalInput.load(); in >= 0)
            text << "Pedal: sends " << in << " -> the engine receives " << keyboard.lastPedalOutput.load();
        else
            text << "Press the MIDI keyboard's sustain pedal to see where it lands on the curve.";
        if (keyboard.pedalLearning.load())
            text << "   |   learning: lowest " << keyboard.pedalMin.load() << ", highest " << keyboard.pedalMax.load();
    } else {
        if (const int in = keyboard.lastInput.load(); in > 0)
            text << "Last key: keyboard velocity " << in << " -> " << keyboard.lastOutput.load();
        else
            text << "Play a key on the MIDI keyboard to see where it lands on the curve.";
        if (keyboard.learning.load())
            text << "   |   learning: softest " << keyboard.learnedMin.load() << ", loudest " << keyboard.learnedMax.load();
    }
    readout.setText(text, juce::dontSendNotification);
    const auto count = kind == CurveKind::pedal ? keyboard.pedalCount.load() : keyboard.noteCount.load();
    if (count != lastCount) {
        lastCount = count;
        graph.repaint();
    }
}

void VelocityCurvePanel::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void VelocityCurvePanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    explanation.setBounds(area.removeFromBottom(36));
    readout.setBounds(area.removeFromBottom(22));
    auto buttons = area.removeFromBottom(30);
    for (auto* b : { &linear, &soft, &hard })
        b->setBounds(buttons.removeFromLeft(kind == CurveKind::pedal ? 130 : 100).reduced(2));
    learn.setBounds(buttons.removeFromRight(130).reduced(2));
    auto row = area.removeFromBottom(26);
    inputMaxLabel.setBounds(row.removeFromLeft(170));
    inputMax.setBounds(row);
    row = area.removeFromBottom(26);
    inputMinLabel.setBounds(row.removeFromLeft(170));
    inputMin.setBounds(row);
    area.removeFromBottom(6);
    graph.setBounds(area);
}

// --- window -------------------------------------------------------------------------------------------------

VelocityCurveWindow::VelocityCurveWindow(pf::VelocityCurve::Settings& s, KeyboardInput& k, std::function<void()> changed, CurveKind kind)
    : DocumentWindow(kind == CurveKind::pedal ? "Sustain pedal curve" : "Keyboard velocity curve",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
{
    content = new VelocityCurvePanel(s, k, std::move(changed), kind);
    setUsingNativeTitleBar(true);
    setContentOwned(content, true);
    setResizable(true, false);
    setResizeLimits(480, 440, 1600, 1200);
}

} // namespace pf::app
