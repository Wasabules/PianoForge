// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DesktopIntegration.h"

namespace pf::app {

DesktopIntegration::DesktopIntegration(juce::PropertiesFile& s, Hooks h) : settings(s), hooks(std::move(h))
{
    apply();
}

DesktopIntegration::~DesktopIntegration()
{
    if (virtualInput != nullptr)
        virtualInput->stop();
    awake.set(false, {});
    performance.set(false, {});
}

bool DesktopIntegration::playing() const
{
    return juce::Time::getMillisecondCounterHiRes() - lastSound < 1000.0 * kPlayingSeconds;
}

void DesktopIntegration::apply()
{
#if JUCE_LINUX
    const bool wantMedia = settings.getBoolValue("mediaControls", true);
    if (wantMedia && media == nullptr && desktop::available())
        media = std::make_unique<desktop::MediaControls>(hooks.commands);
    else if (!wantMedia)
        media.reset();
#endif
#if JUCE_LINUX || JUCE_MAC
    const bool wantMidi = settings.getBoolValue("virtualMidiInput", true);
    if (wantMidi && virtualInput == nullptr && hooks.virtualInput != nullptr) {
        virtualInput = juce::MidiInput::createNewDevice("PianoForge", hooks.virtualInput);
        if (virtualInput != nullptr)
            virtualInput->start();
    } else if (!wantMidi && virtualInput != nullptr) {
        virtualInput->stop();
        virtualInput.reset();
    }
#endif
    tick();
}

void DesktopIntegration::tick()
{
    if (hooks.sounding && hooks.sounding())
        lastSound = juce::Time::getMillisecondCounterHiRes();
    const bool now = playing();
    awake.set(now && settings.getBoolValue("keepAwake", true), "Playing the piano");
#if JUCE_LINUX
    performance.set(now && settings.getBoolValue("performanceProfile", true), "Playing the piano: steady audio");
#endif
    if (media != nullptr && hooks.player)
        media->update(hooks.player());
}

void DesktopIntegration::saved(const juce::String& what)
{
#if JUCE_LINUX
    if (!settings.getBoolValue("notifyRecordings", true) || juce::Process::isForegroundProcess())
        return;
    notificationResult = desktop::notify("PianoForge", what.toStdString()) ? "last one shown" : "no notification service answered";
#else
    (void) what;
#endif
}

DesktopIntegration::Status DesktopIntegration::status() const
{
    Status s;
    const juce::String off = "Off";
    s.keepAwake = !settings.getBoolValue("keepAwake", true) ? off
                  : awake.held()                         ? "Held now (" + juce::String(awake.how()) + ")"
                  : playing()                            ? "Could not be held: " + juce::String(awake.how())
                                                         : juce::String("Held while playing");
#if JUCE_LINUX
    s.performance = !settings.getBoolValue("performanceProfile", true) ? off
                    : performance.held()                               ? juce::String("Performance profile held now")
                    : performance.how().empty()                        ? juce::String("Held while playing")
                                                                       : "Not held: " + juce::String(performance.how());
    s.media = !settings.getBoolValue("mediaControls", true) ? off
              : media == nullptr                             ? juce::String("No session bus: not available")
              : media->active()                              ? juce::String("In the desktop's media controls")
                                                             : juce::String("Registering with the desktop...");
    s.notifications = !settings.getBoolValue("notifyRecordings", true) ? off
                      : notificationResult.isNotEmpty()                ? "On (" + notificationResult + ")"
                                                                       : juce::String("On, while PianoForge is in the background");
    s.instance = !settings.getBoolValue("singleInstance", true) ? "Off (from the next start)"
                 : instanceAnswering                           ? juce::String("This PianoForge takes the files opened")
                                                               : juce::String("From the next start");
#else
    s.performance = s.media = s.notifications = s.instance = "Not on this system";
#endif
#if JUCE_LINUX || JUCE_MAC
    s.midi = !settings.getBoolValue("virtualMidiInput", true) ? off
             : virtualInput != nullptr                       ? juce::String("\"PianoForge\", for other programs to play")
                                                             : juce::String("Not available here");
#else
    s.midi = "Not on this system";
#endif
    return s;
}

} // namespace pf::app
