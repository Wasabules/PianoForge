#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace pf::app {

/// Where the application keeps its settings (PianoForge.settings), for the application, its test aids and the plugin
/// (which plays the application's corpus) alike:
/// - Windows: %APPDATA%\PianoForge;
/// - macOS: ~/Library/Application Support/PianoForge;
/// - Linux: $XDG_CONFIG_HOME/PianoForge, ~/.config/PianoForge by default (JUCE's own choice there is ~/PianoForge, a
///   folder in sight in the home folder; an absolute folder name replaces it).
inline juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "PianoForge";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
#if JUCE_LINUX || JUCE_BSD
    o.folderName = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("PianoForge").getFullPathName();
#else
    o.folderName = "PianoForge";
#endif
    return o;
}

} // namespace pf::app
