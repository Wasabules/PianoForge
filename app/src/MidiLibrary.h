// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace pf::app {

/// One piece of the MIDI library: its file, who wrote it, its title, its length, and the credit of the collection it
/// comes from (its licence asks for it wherever it plays).
struct LibraryPiece {
    juce::File file;
    juce::String composer, title, credit, license;
    int seconds = 0;
};

/// The MIDI library that comes with PianoForge (media/midi in the repository: library.json and the files it lists;
/// today 22 pieces from piano-midi.de, CC BY-SA 3.0 DE). Found where each package puts it: beside the executable
/// (Windows: <app>/midi), in the package's data (Linux: <prefix>/share/pianoforge/midi), in the bundle (macOS:
/// Contents/Resources/midi), or in the repository when PianoForge runs from a build folder.
class MidiLibrary {
public:
    static juce::File folder();
    /// The pieces in the index's order (by composer, then as listed), read once; empty when there is no library.
    static const std::vector<LibraryPiece>& pieces();
};

} // namespace pf::app
