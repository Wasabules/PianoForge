// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ObsFlatpak.h"

namespace pf::app::obsflatpak {

#if JUCE_LINUX

namespace {

constexpr const char* kApp = "com.obsproject.Studio";

/// A command's output, or an empty string when it failed or took more than 5 s.
juce::String run(const juce::StringArray& command, bool& ok)
{
    juce::ChildProcess p;
    ok = false;
    if (!p.start(command, juce::ChildProcess::wantStdOut))
        return {};
    if (!p.waitForProcessToFinish(5000)) {
        p.kill();
        return {};
    }
    const auto text = p.readAllProcessOutput();
    ok = p.getExitCode() == 0;
    return text;
}

} // namespace

State query()
{
    State s;
    bool ok = false;
    const auto permissions = run({ "flatpak", "info", "--show-permissions", kApp }, ok);
    if (!ok)
        return s; // no flatpak, or no Flatpak OBS
    s.installed = true;
    // [Context] devices=all;shm; (the app's own permissions and the overrides together)
    for (const auto& line : juce::StringArray::fromLines(permissions))
        if (line.startsWith("devices="))
            s.sharedMemory = juce::StringArray::fromTokens(line.fromFirstOccurrenceOf("=", false, false), ";", "").contains("shm");
    const auto config = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".var/app/com.obsproject.Studio/config");
    s.plugin = config.getChildFile("obs-studio/plugins/pianoforge-obs/bin/64bit/pianoforge-obs.so").existsAsFile();
    if (!s.plugin) {
        run({ "flatpak", "info", "com.obsproject.Studio.Plugin.PianoForge" }, ok);
        s.plugin = ok;
    }
    return s;
}

juce::String allowSharedMemory()
{
    bool ok = false;
    run({ "flatpak", "override", "--user", "--device=shm", kApp }, ok);
    return ok ? juce::String() : juce::String("flatpak override did not answer");
}

#else

State query()
{
    return {};
}

juce::String allowSharedMemory()
{
    return "not on this system";
}

#endif

} // namespace pf::app::obsflatpak
