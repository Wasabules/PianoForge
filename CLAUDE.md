# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

PianoForge is a concert grand piano simulator: a real-time C++20 **modal engine** (banks of complex resonators)
plays a **modal profile** that an offline Python **forge** measures from a rendered reference corpus. One engine,
several shells: the JUCE standalone app, a VST3/AU plugin, an OBS Studio plugin, and a WebAssembly build.
`README.md` is the user manual (features, menus, shortcuts). `docs/building-a-profile.md` explains the
bank → profile method in depth.

## Commands

Windows is the daily platform (`windows-msvc` / `windows-release` presets, `.venv\Scripts\...`); the commands below
use the Linux presets (`linux-gcc` / `linux-release`) and macOS has `macos-clang` / `macos-release`. JUCE 9.0.5 is
fetched at configure time into `build/_deps`.

```
cmake --preset linux-gcc
cmake --build --preset linux-release                       # everything: engine, app, plugin, forge kernels, monitor
cmake --build --preset linux-release --target modal_engine_test   # one target
ctest --preset linux-release                               # all C++ tests
ctest --preset linux-release -R modal_engine               # one test (names: velocity_curve, mode_bank, modal_engine,
                                                           # room, sequence, upsampler, limiter, tone, piano_gestures,
                                                           # audio_feed, stream_look, plugin_host, plugin_host_lv2, obs_overlay, obs_plugin)
xvfb-run -a ctest --preset linux-release                   # headless Linux: plugin_host needs a display
build/linux-gcc/app/PianoForgeApp_artefacts/Release/PianoForge
cmake --install build/linux-gcc --component pianoforge --prefix ~/.local   # Linux: app, menu entry, icons
(cd build/linux-gcc && cpack -G DEB)                     # .deb (or RPM): app + VST3 + LV2, cmake/packaging.cmake
```

Release builds use JUCE's LTO flags: with GCC the app's link alone takes several minutes, and a change to the app's
compile definitions recompiles every JUCE module. To iterate on the app, a second build folder without LTO links in
seconds: `cmake -S . -B build/linux-dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFETCHCONTENT_BASE_DIR=$PWD/build/_deps
-DPIANOFORGE_PLUGIN=OFF`, then `cmake --build build/linux-dev --target PianoForgeApp`. Never start a second build in the same build folder while one runs
(ninja does not lock it). To try the GUI without touching the real settings, run it with `HOME` pointed at a scratch
folder (on Linux the settings go to `$XDG_CONFIG_HOME/PianoForge`, see `app/src/SettingsLocation.h`).

C++ tests are plain executables (no framework): a local `check(cond, "what")` prints `[PASS]`/`[FAIL]` and the exit
code is the failure count. Engine tests use synthetic profiles built in the test; tests that need the corpus skip
when it is absent. Set `PIANOFORGE_NO_TIMING=1` on a loaded/shared machine so timing checks (room, upsampler) only
inform. On Linux and macOS the OBS feed and stream-look channels are POSIX shared memory, which outlives its
processes (`/dev/shm/PianoForgeFeed*`, by design: readers keep it across application restarts); tests that use it
`shm_unlink` their own names first. The plugin's sound tests find their profile through the application's settings
(its `corpus`), and compare the plugin with that profile; the path given to `PianoForgePluginTest` (ctest: Noire's)
is used only when the corpus has none.

App headless modes (see `app/src/Main.cpp`): `--selftest <corpus pass folder> <report>`,
`--render in.mid out.wav <corpus pass folder> [--engine modal]`, `--check-update <address> <report>`, `--list-audio`,
`--midi-monitor`, `--ui-snapshots`, `--mirror-check`.

Python tools (three editable packages, one venv at the repo root, Python ≥ 3.11):

```
python -m venv .venv
.venv/bin/pip install -e tools/bench -e tools/forge -e tools/profile pytest
.venv/bin/python -m pytest -q tools/forge/tests                       # CI runs the forge tests with pytest
.venv/bin/python -m pytest -q tools/forge/tests/test_modes.py -k name  # one test
.venv/bin/python -m unittest discover -s tools/bench/tests
.venv/bin/python -m unittest discover -s tools/profile/tests
```

The forge's native kernels must be built for the native-vs-Python tests to run (otherwise they skip):
`cmake -S tools/forge/native -B build/forge-native -DCMAKE_BUILD_TYPE=Release && cmake --build build/forge-native`
(also built by the main project). The build copies the library into `tools/forge/forge/_native/`.

WebAssembly (see `web/README.md`): `emcmake cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release`, build,
`ctest --test-dir build/wasm` (engine tests under Node), `node web/check.mjs` (wasm vs native, needs a profile).

CI (`.github/workflows/build.yml`) runs on push to main and on pull requests only once the repo is public (its jobs
skip while it is private: Actions minutes are counted), and by hand. `release.yml` turns a `vX.Y.Z` tag (equal to
`project(... VERSION)`) into a draft GitHub release with the Windows installer, .deb, .rpm and macOS zip; `pages.yml`
publishes the WebAssembly page (web/site + the module) to GitHub Pages at pushes touching `web/` or `engine/`. Do not
trigger them by hand (`gh workflow run`, pushing a tag) unless asked.

## Architecture

**Data flow.** sample bank or Kontakt instrument → `tools/bench` renders measurement sessions
(`tools/bench/sessions/*.toml`) into `corpus/reference/<state>/` and `corpus/interactions/<state>/` →
`tools/forge` measures them (`forge pipeline`: analyze, modes, soundboard per velocity layer, release-curves,
release-noise, half-pedal, sympathetic, export) → `corpus/reference/<state>/modal-profile.bin` → the engine loads
it. `tools/profile` (`pianoforge-profile build`) chains bench + forge for an SFZ bank in one resumable command;
`tools/forge/monitor` is a JUCE window that follows/starts those builds.

**`corpus/` is local only and never committed** (commercial library EULA). States: `noire-dry` (Kontakt, default for
forge commands), `salamander-dry`, `in162-close`. A profile made from Noire must never be published or bundled.

**Engine (`engine/`, namespace `pf`, no framework).** `pf::Engine` (`engine.hpp`) is the interface every shell talks
to: `prepare()`/`release()` off the audio thread (may allocate), `process()` on the audio thread — **no allocation,
locks or I/O**. `pf::ModalEngine` holds one voice per MIDI note (no voice stealing), a shared soundboard bank, and
sympathetic free-string banks; its settings are `std::atomic` fields read each block or note-on, and profiles are
swapped with `install()` from the message thread. The long doc comment at the top of `modal_engine.hpp` is the
authoritative description of the model (layer blending, re-strike, dampers, sostenuto, sympathetic, tuning).
Resonator loops in `mode_bank.hpp` are written to auto-vectorise 8 lanes (AVX2 `-mavx2 -mfma` on x86-64, a generic
loop elsewhere, `-msimd128` in wasm). Also here: room/headphone model, upsampler, limiter, compressor, MIDI
sequence player, piano gestures, and the shared-memory channels to OBS (`audio_feed`, `look_channel`).

**Shells.** Each shell assembles its own sound chain around the engine, so a change to the chain or to an engine
setting usually has to be mirrored in several places:
- `app/` — `AudioHost.h` (device callback: MIDI gathering, engine rate division + upsampling above 50 kHz, room,
  reverb, tone, limiter, OBS feed, recording). `EngineSwitch.h` A/B-switches between `ReferencePlayer` (plays the
  corpus takes) and the modal engine. `MainComponent.cpp` is the main window; `ui/` holds the panels.
  Linux audio, beyond JUCE: `PipeWireAudio.cpp` is a JUCE device type of its own ("PipeWire", first in the list):
  a `pw_stream` rendered in PipeWire's RT thread, `node.force-quantum` asking the graph for the chosen block (it goes
  under `clock.min-quantum`, which `node.latency` does not), libpipewire dlopen'd (headers from the system or
  PipeWire 1.0.5's, fetched). `AudioDevices.cpp`'s `DeviceManager` (use it, not `juce::AudioDeviceManager`) puts it
  first and wraps JUCE's ALSA and JACK types so their PipeWire devices ask for the block too (`PIPEWIRE_ALSA`, or
  `PIPEWIRE_PROPS` around the JACK client's creation: PipeWire applies those variables after a stream's own
  properties, to every stream of the process, so they are only ever set around an opening, `linuxaudio::ScopedProps`).
  `LinuxAudio.cpp`: PipeWire's libjack preloaded for JUCE's JACK type, the audio thread made realtime (RLIMIT_RTPRIO
  or rtkit over sd-bus), PipeWire's clock read with `pw-metadata`. `PianoForge --audio-check` measures an output
  headless (see `Diagnostics.h`). `Mp3Encoder.cpp` uses Media Foundation on Windows and `libmp3lame` (dlopen'd)
  elsewhere.
  The desktop (Settings > Desktop, `DesktopIntegration` driving `DesktopBus.cpp`): GIO dlopen'd with its own minimal
  declarations (no glib headers needed): the portal's Inhibit, power-profiles-daemon's HoldProfile (system bus),
  org.freedesktop.Notifications, and an MPRIS server on a GLib thread of its own (commands posted to the message thread).
  `SingleInstance.cpp`: an abstract UNIX socket per user and settings folder (JUCE's single-instance mechanism is a
  TODO on Linux). `ObsFlatpak.cpp`: Flathub's OBS state through the flatpak CLI. `PianoForge --desktop-check` tries
  them all.
- `plugin/` — `EngineRunner.h` is the plugin's copy of that chain (any block size, reports upsampler latency);
  settings are exposed as automatable parameters in `PluginProcessor.cpp`. It reads the application's settings file
  (to find its corpus): both get its location from `app/src/SettingsLocation.h`. VST3 everywhere, AU on macOS, LV2 on
  Linux; the host test runs both formats (JUCE's LV2 host: parameters travel with the audio, so a value set from the
  host is seen after a block, and `prepareToPlay` re-creates the instance).
- `obs/` — OBS sources fed by the app through shared memory; compiles `engine/src/audio_feed.cpp` and
  `look_channel.cpp` itself (without the engine's AVX2 flags). Built only when OBS/libobs is found, or alone
  (`cmake -S obs`): on a machine with Flathub's OBS (no libobs on the host), build and test it inside that OBS's
  SDK, `flatpak run --devel --command=sh com.obsproject.Studio -c "cmake -S obs -B build/obs-flatpak -G Ninja
  -DCMAKE_PREFIX_PATH=/app && cmake --build build/obs-flatpak && ctest --test-dir build/obs-flatpak"` (the sandbox
  sees the repository, not the host's `/tmp`; it needs `--device=shm` to see the host's feed). The keyboard's
  `.effect` must suit OBS's OpenGL translator too: no arrays of uniforms (OBS writes them as one value).
- `web/` — `src/exports.cpp` is a C API (`pf_*`); every new export must also be added to `EXPORTED_FUNCTIONS` in
  `web/CMakeLists.txt`. The page lives in `web/site/`.

**Cross-language contracts** (change both sides together):
- `modal-profile.bin`: written by `tools/forge/forge/export.py` (`MAGIC = b"PFMODAL1"`, `VERSION`, tagged sections
  such as `LEVL`, `SYMP`, `DAMP`, `UNIS`), read by `engine/src/modal_profile.cpp`. Older profiles must keep loading
  and playing.
- Forge native kernels: `tools/forge/native/*.cpp` export `pf_native_version()`, which must equal `VERSION` in
  `tools/forge/forge/native.py`; on a mismatch the forge silently falls back to the (much slower) Python code.
  The Python implementation is the reference; tests compare the two. `PIANOFORGE_FORGE_NATIVE=0` forces Python.

**`studies/`** are standalone scripts behind design decisions (precision, CPU budget, fits), not part of any
package. The design rationale lives in the French documents at the root; `Pré-analyse - Concept, forme et choix
techniques.md` is the "analysis document" that code comments refer to.

## Conventions

- Comments and docs are explanatory prose: they say what the code models and why, often with the measured figure
  that justified a choice. Keep that tone, and keep `README.md` / `docs/` / the analysis document in step with
  user-visible changes.
- Commit subjects are `Area: what changed, in plain words` (areas used so far include Forge, Lab, Room, Sympathetic,
  Web, OBS keyboard, OBS plugin, Profile builder, CI, docs), followed by a prose body explaining why and how it was
  checked.
- LF line endings everywhere (`.gitattributes`); MSVC builds use `/utf-8`.
- The repository is AGPL-3.0-or-later (`LICENSE`; `NOTICE.md` lists the third-party components and the banks'
  licences). Every source file starts with `SPDX-License-Identifier: AGPL-3.0-or-later` in its comment syntax (after a
  shebang or a doctype); a new file gets it too. Commits carry no Claude/AI attribution trailer.
