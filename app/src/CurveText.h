#pragma once

#include <pianoforge/velocity_curve.hpp>

#include <juce_core/juce_core.h>

namespace pf::app {

/// A velocity or pedal curve as saved in the settings (the application's, the plugin's state):
/// "inputMin;inputMax;p0,p1,p2,p3,p4". `lowest` is the smallest input allowed (1 for velocities, 0 for the pedal);
/// anything malformed gives `fallback`.
inline pf::VelocityCurve::Settings parseCurveText(const juce::String& text, int lowest = 1,
                                                  pf::VelocityCurve::Settings fallback = {})
{
    auto s = fallback;
    const auto parts = juce::StringArray::fromTokens(text, ";", "");
    if (parts.size() != 3)
        return s;
    const auto points = juce::StringArray::fromTokens(parts[2], ",", "");
    if (points.size() != pf::VelocityCurve::kPoints)
        return s;
    s.inputMin = juce::jlimit(lowest, 126, parts[0].getIntValue());
    s.inputMax = juce::jlimit(s.inputMin + 1, 127, parts[1].getIntValue());
    for (int i = 0; i < pf::VelocityCurve::kPoints; ++i)
        s.points[(size_t) i] = juce::jlimit(0.0f, 1.0f, points[i].getFloatValue());
    return s;
}

inline juce::String formatCurveText(const pf::VelocityCurve::Settings& s)
{
    juce::StringArray points;
    for (const float p : s.points)
        points.add(juce::String(p, 4));
    return juce::String(s.inputMin) + ";" + juce::String(s.inputMax) + ";" + points.joinIntoString(",");
}

} // namespace pf::app
