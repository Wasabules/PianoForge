#include "StreamLook.h"

namespace pf::app {

StreamLook::StreamLook(juce::PropertiesFile& s) : settings(s)
{
    if (settings.containsKey("streamLook"))
        current.read(settings.getValue("streamLook").toStdString());
    changed = (std::uint64_t) settings.getValue("streamLookChanged", "0").getLargeIntValue();
    if (link.open(pf::LookChannel::Side::app))
        link.publish(current.text(), changed); // OBS takes it if it is the newest
}

void StreamLook::poll()
{
    if (dirty) { // (the settings written at this pace, not at every move of a slider)
        settings.saveIfNeeded();
        dirty = false;
    }
    std::string text;
    std::uint64_t obsChanged = 0;
    bool fromObs = false;
    while (link.other(obsSerial, text, obsChanged)) // the newest of what OBS sent
        fromObs = true;
    if (!fromObs)
        return;
    seen = true;
    pf::look::Values theirs;
    theirs.read(text);
    if (theirs == current)
        return;
    if (obsChanged >= changed) { // OBS's is the newest (or as new): it is the look
        current = theirs;
        changed = obsChanged;
        save();
        if (onChanged)
            onChanged();
    } else { // ours is: OBS is told again
        link.publish(current.text(), changed);
    }
}

void StreamLook::set(std::size_t index, double value)
{
    pf::look::Values next = current;
    next.setAt(index, value);
    if (next == current)
        return;
    current = next;
    edited();
}

void StreamLook::setAll(const pf::look::Values& values)
{
    if (values == current)
        return;
    current = values;
    edited();
}

void StreamLook::edited()
{
    changed = std::max(pf::LookChannel::nowMs(), changed + 1);
    link.publish(current.text(), changed);
    save();
}

void StreamLook::save()
{
    settings.setValue("streamLook", juce::String(current.text()));
    settings.setValue("streamLookChanged", juce::String((juce::int64) changed));
    dirty = true;
}

} // namespace pf::app
