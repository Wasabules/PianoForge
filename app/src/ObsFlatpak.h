// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

namespace pf::app::obsflatpak {

/// Flathub's OBS Studio (com.obsproject.Studio), the usual one on Ubuntu, as PianoForge's OBS plugin needs it (Linux;
/// elsewhere: not installed): the plugin in its folder (obs/install-flatpak.sh), or as its Flatpak extension; and the
/// system's shared memory, where PianoForge's feed is: a Flatpak keeps a /dev/shm of its own until it is granted
/// --device=shm.
struct State {
    bool installed = false;    // the Flatpak OBS
    bool plugin = false;       // PianoForge's plugin, in its folder or as an extension
    bool sharedMemory = false; // it sees the system's /dev/shm (an override, or its own permissions)
};

/// Runs flatpak (a few hundred milliseconds): from a background thread.
State query();

/// flatpak override --user --device=shm com.obsproject.Studio: OBS sees PianoForge from its next start. Returns an
/// empty string, or why it could not.
juce::String allowSharedMemory();

} // namespace pf::app::obsflatpak
