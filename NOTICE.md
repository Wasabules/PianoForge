# Licence and third-party notices

## PianoForge

Copyright © 2026 the PianoForge authors.

PianoForge is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General
Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option)
any later version (SPDX: `AGPL-3.0-or-later`). It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Affero General Public License for more details: [LICENSE](LICENSE).

This covers the whole repository (the engine, the application, the plugins, the OBS plugin, the web build, the Python
tools, the studies, the build files and the documentation) and its whole history: every version is under the same
licence, and the LICENSE file is in every commit from the first one (it was put into each of them on 2026-11-27, when
the licence was chosen; nothing else in them changed).

Each source file names it on its first lines (`SPDX-License-Identifier: AGPL-3.0-or-later`).

## Built into PianoForge

| Component | Where | Licence |
|---|---|---|
| [JUCE](https://juce.com) 9 | the application, the plugins, the forge monitor (fetched at configure time) | AGPLv3, the option used here (or the commercial JUCE licence); the libraries JUCE vendors are listed in its `JUCE.spdx.json` |
| Steinberg VST3 SDK | the VST3 plugin, through JUCE | MIT |
| Apple Audio Unit SDK | the AU plugin on macOS, through JUCE | Apache-2.0 |
| LV2 (and lilv, serd, sord, sratom for the test host) | the LV2 plugin on Linux, through JUCE | ISC |
| Steinberg ASIO SDK 2.3.4 | the Windows application (fetched at configure time) | dual proprietary / GPL-3.0: the GPL-3.0 option, compatible with the AGPL |
| PipeWire 1.0.5 headers | the Linux application (fetched when the system has none) | MIT |
| JACK 1.9.22 headers | the Linux application (fetched when the system has none) | LGPL-2.1-or-later |
| libobs (OBS Studio) | the OBS plugin links against it | GPL-2.0-or-later, used under version 3 |

## Used from the system, not distributed

Loaded at run time when present, never shipped with PianoForge: libpipewire (MIT), PipeWire's or JACK's libjack
(LGPL), ALSA (LGPL), GIO/GLib (LGPL-2.1-or-later, the desktop integration), libsystemd's sd-bus (LGPL-2.1-or-later,
rtkit), LAME's libmp3lame (LGPL, MP3 export), Windows Media Foundation (MP3 export on Windows).

The Python tools install their dependencies from PyPI, each under its own licence; the bench's
[pedalboard](https://github.com/spotify/pedalboard) (GPL-3.0) hosts a VST3 instrument while rendering and is never
part of the engine.

## Sample banks and modal profiles

A modal profile is measured from a sample bank and keeps short excerpts of it (attack and release noises): it is an
adaptation of that bank and follows the bank's licence, not this one. The repository holds no bank and no profile.

- **Salamander Grand Piano V3** by Alexander Holm (SFZ by SFZ Instruments), licensed under
  [CC-BY 3.0](https://creativecommons.org/licenses/by/3.0/). A profile made from it may be published with this credit
  and the mention that it is a set of measures derived from the bank by PianoForge's forge.
- **Piano in 162** by Ivy Audio: no licence text comes with it; a profile made from it stays local unless Ivy Audio
  agrees otherwise.
- **Noire** (Native Instruments): commercial; its renders and any profile made from them stay local, never published.
