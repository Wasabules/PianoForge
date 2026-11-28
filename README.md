<img src="app/resources/logo.svg" alt="PianoForge" width="96" align="right">

# PianoForge

A concert grand piano simulator played from a MIDI keyboard: a calibrated hybrid modal engine, with
recording of every take and technician-level settings. Cross-platform (Windows, Linux, macOS: see Platforms),
low latency (key to sound <= 10 ms, jitter <= 1 ms).

Status: proof of concept. The modal engine plays modes measured on a reference corpus rendered from a sampled
grand piano (unison strings and their beating, the soundboard body, a short attack residual); the listening lab
switches between that reference and the modal engine while playing.

## In pictures

**The application** (Windows, Linux, macOS), playing a piece of its MIDI library, Debussy's *Clair de lune*: the
keys it plays, the sustain pedal down and 86 free strings ringing with it, the audio load, the piece's level along
its length.

![The application playing Clair de lune](docs/images/app.png)

<table>
  <tr>
    <td width="56%"><img src="docs/images/app-tone.png" alt="Settings: tone and dynamics"><br>
      <sub>Settings: the touch, the sustain and the dampers, the lid, the output tone, a convolution reverb, and a
      compressor for what goes to a stream.</sub></td>
    <td width="44%"><img src="docs/images/app-room.png" alt="The room and the listening position"><br>
      <sub>The room around the piano (its volume, decay, brightness, width) and where it is heard from, on speakers
      or headphones.</sub></td>
  </tr>
</table>

**The plugin** (VST3; AU on macOS, LV2 on Linux): the same engine, room and settings in a host.

![The plugin's editor](docs/images/plugin.png)

**In a browser**, at <https://geoffrey-lecoq.fr/PianoForge/>: the engine compiled to WebAssembly. Three steps lead to
the first note (a piano, the engine, the keys); the MIDI gallery gives each piece's facts and its credit.

![The web page, before the first note](docs/images/web-start.png)

![The web page's MIDI gallery](docs/images/web-gallery.png)

The pictures come from the programs themselves: `PianoForge <piece.mid> --window-snapshot <file.png> [seconds]`
(the window, the piece playing silently), `PianoForge --ui-snapshots <folder>` (the panels), the web page driven in
a headless browser.

## Layout

| Folder | Content |
|---|---|
| `engine/` | Core, pure C++20 without framework: the `pf::Engine` interface, the modal engine (complex resonator banks, AVX2), the profile loader; `modal_render` (offline renders, CPU measurements). |
| `app/` | Standalone application (JUCE 9): listening lab with an 88-key keyboard, MIDI input, audio device selection. |
| `tools/bench/` | Reference bench (Python): renders a VST3 instrument through measurement sessions and checks the takes. |
| `tools/forge/` | Forge (Python): analysis and calibration, from the reference corpus to engine data. |
| `tools/profile/` | Profile builder (Python): a sample bank to a modal profile in one resumable command (`pianoforge-profile`). |
| `docs/` | Building a profile from a sample bank: the tool, the method, the checks. |
| `studies/` | Standalone computations and spikes behind design decisions (resonator precision, CPU budget, resynthesis). |
| `plugin/` | The VST3 instrument (JUCE): the same engine, room, headphones, output tone and limiter in a host. |
| `obs/` | OBS Studio plugin: the PianoForge sound (on speakers, as heard or dry, its own level), keyboard (lit keys and pedals, rounded notes, lights and sparkles, with the sound) and text sources, and chapter markers in OBS's recordings; fed by the application through shared memory. |
| `media/midi/` | The MIDI library: 22 piano pieces by Bernd Krueger (piano-midi.de, CC BY-SA 3.0 DE), from Bach to Debussy, in Player > Library and on the web page. |
| `web/` | The modal engine and the room in WebAssembly, played from a browser page: <https://wasabules.github.io/PianoForge/> (it asks for a modal profile, kept in the browser). |
| `installer/` | The Windows installer (Inno Setup 6) and the update manifest it comes with. |
| `corpus/` | Rendered takes and plugin states. Local only, never committed (commercial sample library licences). |

## Build the application

Requirements: CMake >= 3.25 and Ninja, with Visual Studio 2022 (Windows), GCC 13 or later and JUCE's Linux
dependencies (Linux), or Xcode's command-line tools (macOS). JUCE is fetched automatically at configure time.

```
cmake --preset windows-msvc          # linux-gcc, macos-clang
cmake --build --preset windows-release   # linux-release, macos-release
build/windows-msvc/app/PianoForgeApp_artefacts/Release/PianoForge.exe
```

JUCE's Linux dependencies (Ubuntu 24.04):

```
sudo apt install ninja-build libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev libxrender-dev libgl1-mesa-dev
```

On Linux, installed into a prefix, PianoForge gets its menu entry (which opens MIDI files too) and its icons:

```
cmake --install build/linux-gcc --component pianoforge --prefix ~/.local     # or /usr/local, as root
```

- Audio, from the lowest latency (Audio > Audio device type):
  - **PipeWire** (Ubuntu, Fedora and most desktops today; the default where it runs): PianoForge's own PipeWire
    device, Linux's counterpart of ASIO. The sound goes straight into PipeWire's graph, rendered in PipeWire's
    realtime thread at the block chosen, which the graph is asked to run at while PianoForge plays
    (`node.force-quantum`: under PipeWire's own minimum, `clock.min-quantum`, as well). Measured on a laptop's built-in
    output: 128 samples, about 5 ms from key to sound; 64, about 3 ms; 32 underruns there. Any output: the system's
    default (followed when it changes) or a sink by name. Nothing to install: libpipewire (0.3.64 or later) is loaded
    at run time, its headers taken from the system (libpipewire-0.3-dev) or fetched at configure time.
  - **JACK**: PipeWire's once its JACK library is installed (Debian and Ubuntu: `sudo apt install pipewire-jack`;
    Fedora and Arch install it as the system's libjack), loaded by PianoForge itself (no `pw-jack`), its block chosen
    the same way; or a JACK server's (`PIANOFORGE_JACK_LIBRARY` names its libjack). JACK's headers come from the
    system (libjack-jackd2-dev) or are fetched; `-DPIANOFORGE_JACK=OFF` leaves JACK out.
  - **ALSA**: its PipeWire device keeps four blocks of its own before PipeWire (about 8 ms more at 128), its block
    asked of the graph too (`PIPEWIRE_ALSA`); its other devices are the sound cards, which PipeWire holds while it
    runs. JUCE runs its ALSA thread at a normal priority: PianoForge makes it realtime, by the user's own limit (the
    audio group's `rtprio`) or else through rtkit, as PipeWire does.

  Audio > PipeWire turns the asking off (the graph then keeps its own block, never under its `clock.min-quantum`)
  and shows what the graph does (its block, its rate, the latency to the device, its xruns); Audio > Performance
  advises. Settings saved with ALSA's PipeWire device move to the PipeWire device once, the block brought to 128.
  `PianoForge --audio-check [--type PipeWire|ALSA|JACK] [--device ...] [--rate ...] [--block ...] [report]` opens an
  output as the application does and measures it (blocks, waits, latency, xruns, the audio thread).
- MP3: LAME, loaded when it is installed (Debian and Ubuntu `libmp3lame0`, Fedora `lame-libs`, Arch `lame`).
- Settings in `~/.config/PianoForge` (`$XDG_CONFIG_HOME`), recordings and takes in the XDG documents folder
  (`~/Documents/PianoForge`), what is deleted to the desktop's trash.
- Packages: `cpack -G DEB` (or `RPM`) in the build folder makes `pianoforge_<version>_amd64.deb` (or the .rpm): the
  application, its menu entry (with "Start a recording" and "Recordings and takes"), icons and AppStream metadata
  (`org.pianoforge.PianoForge`), and the VST3 and LV2 plugins in `/usr/lib/vst3` and `/usr/lib/lv2`
  (`cmake/packaging.cmake`). A package asks for the C library it was built against: build it on the oldest
  distribution aimed at (the CI makes the .deb on Ubuntu 24.04).
- Settings > Desktop, each part a setting, on by default:
  - the interface's size (automatic, or 100 to 200 %: XWayland may blur or shrink it under a 125 or 150 % desktop);
  - while playing (a note in the last ten minutes, a MIDI file, a recording): the screen kept on and no suspend (the
    desktop portal; a MIDI keyboard is no activity to the desktop), and the "performance" power profile
    (power-profiles-daemon, given back after ten minutes without a note or when PianoForge closes);
  - the MIDI player in the desktop's media controls (MPRIS: media keys, the shell's widget, a headset's buttons);
  - a notification when a recording is saved while PianoForge is in the background;
  - one PianoForge: a MIDI file opened from the file manager, or the menu's actions, go to the one running (a socket
    of its own, per user and settings folder);
  - a MIDI input named "PianoForge" for other programs (a DAW, a score editor, a sequencer): their notes as sent.
  `PianoForge --desktop-check [--hold <s>] [--notify]` tries each of them.
- A Bluetooth output (headphones, a speaker) adds 100 to 250 ms whatever the block: PianoForge says so (the output's
  name in Audio and in Audio > Performance). Only one PianoForge feeds OBS at a time (a lock on the feed, let go even
  by a crash).

Engine tests (no corpus needed) and headless self-test of the application (reference player, and the modal
engine when the corpus state has a profile):

```
ctest --preset windows-release       # linux-release, macos-release
PianoForge.exe --selftest corpus/reference/noire-dry/single-notes selftest.txt
```

## Platforms

Windows is where PianoForge is used and tested every day. The code is written for Linux and macOS too, and the
continuous integration (`.github/workflows/build.yml`: Windows, Ubuntu, macOS on Apple silicon, the forge's tests,
the WebAssembly build) builds and tests it on all three at every push to main and every pull request once the
repository is public, and by hand at any time (Actions > build > Run workflow, or `gh workflow run build.yml`); while
the repository is private, its jobs skip (minutes counted). On Linux it has run too (Ubuntu
26.04, GCC 15, PipeWire): every test, the application's self-test, the plugin in a host, and a free bank's profile
built from scratch and played (Salamander: the download, the renders and the forge in about 35 minutes). macOS has
not run it yet.

| | Windows | Linux | macOS |
|---|---|---|---|
| Engine | AVX2 | AVX2 (x86-64); the generic loop elsewhere | the generic loop on Apple silicon (not measured yet) |
| Audio | ASIO, WASAPI, DirectSound | PipeWire (its own device: ~5 ms at 128, ~3 ms at 64), JACK, ALSA | Core Audio |
| Audio thread | the driver's | PipeWire's or JACK's (realtime); ALSA's made realtime (the user's limit, or rtkit) | Core Audio's |
| Stream output (a second device) | WASAPI | ALSA | Core Audio |
| MP3 export | Media Foundation | LAME, when installed (libmp3lame) | LAME, when installed (Homebrew's lame) |
| Settings | %APPDATA%\PianoForge | ~/.config/PianoForge | ~/Library/Application Support/PianoForge |
| Updates | the installer, from the app | as it was installed | as it was installed |
| Installer | Inno Setup | `cmake --install` (menu entry, icons); .deb and .rpm by `cpack` | none yet (nor signing) |
| VST3 plugin | ~/AppData/Local/Programs/Common/VST3 | ~/.vst3 | ~/Library/Audio/Plug-Ins/VST3, and an Audio Unit |
| OBS plugin | built against the installed OBS, tested in libobs | a Flatpak OBS: built in its SDK, tested in its libobs (OpenGL); a system OBS: against libobs | a .plugin bundle (untried) |
| OBS feed, stream look channel | shared memory (Local\\) | POSIX shared memory (a Flatpak OBS: `--device=shm`, see `obs/README.md`) | POSIX shared memory |

The timing checks of the tests (the room, the upsampler) are informative where `PIANOFORGE_NO_TIMING` is set (a
shared machine: the CI).

Play a MIDI file live: File > Open MIDI file (Ctrl+O), drop it on the window, or pass it on the command line
(`PianoForge.exe song.mid`); or one of the library's 22 pieces, Player > Library (Bach, Mozart, Beethoven, Schubert,
Chopin, Schumann, Liszt, Tchaikovsky, Grieg, Debussy: Bernd Krueger's performances from piano-midi.de, CC BY-SA 3.0
DE, each with the sustain pedal as played; previous and next go through the library). It plays to the sample with what the keyboards play, and is recorded with it.
Tracks... picks the parts to play (a part is a track's channel; drums off by default); -oct, -, +, +oct
transpose the file by semitones or octaves.

The player's buttons:

- previous and next file of the list. The list is the MIDI files of the folder opened from, or what the gallery
  shows when "Play with the engine" starts there. Previous, after the first 3 s of a file, goes back to its start;
- 5 s back and on;
- the speed, 25 to 200 %, with - and + by 5 %;
- auto-play: a file starts as soon as it is opened, and the next one starts at its end;
- repeat: off, all (the list from its start again), or one (the file).

Under the position, the level along the piece shows where it is loud. It is read from the recording next to the
MIDI file (the same name: WAV, FLAC, MP3, Opus, or a take's render); without one, it is estimated from the notes'
velocities. A click on it moves there.

Presets saves the whole instrument under a name (the engine and its settings, the tuning, the room and the
listening position, the curves, the volume) and switches between them; Presets > Export settings / Import settings
keeps them, the curves and the rooms of one's own in a .pfsettings file (the audio device and the folders of the
computer are left out).

A safety limiter at the output (Audio > Output) keeps everything heard, recorded and streamed under -1 dBFS, with
no latency and untouched while under it.

Keyboard > Pedal curve shapes the MIDI keyboard's sustain pedal like the velocity curve (the travel it really
covers, where the half pedal falls); the Pedal button fills with the pedal's travel. Rooms and listening positions
can be saved under names of your own (Room window: Save..., Delete; Engine > Room presets).

The modal engine models each note's coupled strings (Weinreich): the soft pedal (CC67, the N key) makes the hammer
miss a string, and Engine settings > Unison tuning draws the strings together (0 %: no beating) or apart.

One string per note: a note struck again strikes its string while it rings, and the strikes add up (repeated notes
with the pedal down build up as on the reference); the hammer absorbs part of the vibration over its contact
(Engine settings > Re-strike absorption, 50 % fitted on the reference). No voice is ever taken from another note:
the 88 notes can ring at once.

Record (Ctrl+R) writes the sound live as a 32-bit float WAV; when it stops, the formats ticked in File > Record
formats are made from it in the background: WAV 24-bit, FLAC 24-bit, MP3 320 kbit/s (Windows' Media Foundation
encoder), Opus 192 kbit/s; plus the MIDI, aligned to the sample. Recordings go to Documents/PianoForge/Recordings.
By default (File > Start recordings at the first note) Record arms: the recording starts 0.25 s before the first
note, no silence before the music.

Keyboard > Piano gestures: keys struck together that no piece asks for, or one key struck twice quickly, played on
the MIDI keyboard, do what is chosen there (learned by playing them): arm or stop a recording (A0 + A#0 + B0 by
default; stopped so, it ends before the gesture's notes), save the last take (C8 twice, without the gesture), play or
pause the MIDI file (A0 twice), and in OBS through the plugin: start or stop the recording (A#7 + B7 + C8), the
stream, add a chapter marker, change scene. The on-screen keyboard marks the gestures' keys (a dot, two for a key
struck twice); the mouse over one says what it does.

Audio > Stream look (OBS keyboard): the look of the OBS plugin's keyboard, set here as in OBS's properties, in tabs
(keyboard, colours, notes, lights, sparkles, velocity, powerful chords) with the styles and a way back to the defaults,
and its preview as OBS renders it (while OBS runs with the plugin). Each side follows the other: an edit here reaches
every keyboard synchronised in OBS, one there comes here, the newest wins; a look set while OBS is closed reaches it
when it starts.

File > Gallery (Ctrl+T) holds the recordings (Ctrl+R) and the takes (Ctrl+S). Each is one item, whatever its
formats (WAV, FLAC, MP3, Opus, MIDI). The window has:

- on the left, the collections: everything, recordings, takes, not named yet, then one per piece;
- on top, the search (piece, composer, tags, notes, name) and the filters: formats, date, rating, length, size;
- in the middle, the list, sorted by any column (more columns from the header's menu);
- on the right, what is known of the selection: piece, composer, tags, a rating of 0 to 5 stars, notes. Apply sets
  them on every item selected; a field left as it was keeps each one's own. With a piece, Apply also renames the
  files from the name pattern and moves them to the piece's folder;
- below, listening and the actions. Listen to one (Space), or to two in A/B from the same instant (A and B switch
  between them with a 10 ms crossfade). Play one through the engine from its MIDI, render it again with the settings
  heard now, rename it, show it in its folder, or delete it (to the recycle bin).

What is listened to there is heard only: neither recorded again nor streamed. What is known of an item is kept next
to its files, in `<name>.json`, and follows them.

When a recording stops, a window asks what was played: the piece (those played before are suggested), its composer
(filled in from an earlier recording), tags and a rating. Save names the files; Escape keeps the automatic name (the
date and time); Discard sends the files to the recycle bin. The conversions to MP3 and the other formats wait for
the name, so they are made under it.

Settings > Recording and naming holds:

- the shortest recording kept, 30 s by default. A recording stopped before it is a draft: not kept, its sound sent
  to the recycle bin. Takes saved with Ctrl+S are always kept;
- whether to ask for a name when a recording stops;
- the name pattern. Its tokens are {piece}, {composer}, {date}, {time}, {n} (the recording's number among the
  piece's) and {tags}. An empty token takes the separator before it away;
- a folder per piece.

When the device runs faster than 50 kHz (the Windows mixer can impose 192 kHz), the engine runs at the device's
rate divided by 2, 4 or 8 (44.1 or 48 kHz) and its sound is brought up to the device's: Audio > Engine rate.

A room around the piano, heard from a listening position (the engine's sound is the piano heard from the keyboard,
1 m away; further, the direct sound falls as 1/r, loses its highs to the air and narrows, and the room's energy
follows Barron's revised theory): the Room button in the toolbar, its settings in Engine > Room settings (Ctrl+M).
No latency on the direct sound. Heard, recorded, streamed and in take renders alike.

On headphones (Room window: Heard on, or Ctrl+H), the piano is in front of you rather than inside your head: each
side of it reaches both ears, the farther one later and duller (a spherical head: Woodworth's delay, Brown and Duda's
shadow, equalized for a source in front so that a mono sound keeps its level), the room's reflections each from
where they come, and the tail as coherent between the ears as a diffuse field. Room on or off.

Every setting is in one window, Settings (Ctrl+,), its sections on the left: Engine (Ctrl+E), Tone and dynamics
(Ctrl+D), Room (Ctrl+M), Audio, MIDI and streaming, Piano gestures, Stream look. The preset bar on top holds two
kinds of preset, also listed in the Presets menu:

- Factory presets. They set the sound only (what the engine models, the voicing, the touch, the tone, the room and
  the listening position), and leave the tuning, the volume and the curves alone. There are ten: As measured,
  Concert hall front row, Soft and round, Softened (bright banks), Bright and present, Jazz club, Night practice,
  Stream, Cathedral, Old upright.
- Your presets. Each holds the whole instrument, and can be saved, replaced or deleted.

The bar marks a preset as modified once the sound moves away from it, and Revert applies it again.

Shaping the instrument and the output, at 0 or 100 % the sound as measured, bit for bit:

- Voicing (Engine section): the hammers needled or hardened per register (bass, middle, treble), a shelf on each
  voice's whole sound from a chosen partial up, following the velocity by a share.
- Tone and dynamics (its section):
  - Touch and resonance (the modal engine): the dynamic range, the sustain length, the damper speed, the attack's
    softness and length, and the lid (open, half stick, closed).
  - The output tone: bass, presence and air.
  - A convolution reverb: an impulse response from a WAV file of your choice, beside the room, with its level and
    the room's.
  - A stream compressor: it works on what leaves for OBS and the mirror output only; what is heard and recorded
    stays as it is.

  The first three are in the instrument presets and the take renders. The plugin has the voicing, the touch, the
  lid and the output tone as parameters, and the factory presets as its programs.

In a browser (WebAssembly, the modal engine and the room, Web MIDI): see `web/README.md`.

Offline render of a MIDI file, through the reference player or the modal engine:

```
PianoForge.exe --render take.mid take.wav corpus/reference/noire-dry/single-notes [--engine modal]
```

## VST3 and LV2 plugins

On Linux the same plugin is an LV2 one too (Ardour, Carla, Qtractor, Zrythm): `cmake --build --preset linux-release
--target install-lv2-plugin` copies `PianoForge.lv2` to `~/.lv2` (URI `urn:pianoforge:piano`), and `ctest -R
plugin_host_lv2` runs the host test through JUCE's LV2 host. CLAP is not built: it would take free-audio's
clap-juce-extensions, an outside dependency to vet first.

### VST3

```
cmake --build --preset windows-release --target install-vst3-plugin
```

builds `PianoForge.vst3` and copies it to the per-user VST3 folder (`%LOCALAPPDATA%\Programs\Common\VST3`, which
REAPER scans; FL Studio and others may need it added to their plugin folders), no administrator rights needed. A
host that has it loaded keeps its copy (moved aside) until it loads the plugin again; rescan the plugins in the
host the first time. The installer offers it too.

- The sound is the application's: the modal engine, the room and the listening position (headphones included), the
  output tone, the limiter, with the same settings as automatable parameters (volume, what the engine models,
  sympathetic level, unison tuning, re-strike absorption, the hammers' voicing, the dynamic range, the sustain, the
  dampers, the attack, the lid, room, distance, keep loudness, speakers or headphones, bass, presence, air, A4,
  temperament, limiter). The application's factory presets are its programs (the host's preset list, or the Preset
  menu in its window): the sound only; the tuning, the volume, the limiter and the curve stay.
  MIDI: notes, CC64 continuous sustain, CC66 sostenuto, CC67 soft pedal. A velocity curve of its own, drawn, or the
  application's (Keyboard > Velocity curve) copied in.
- The modal profile is never inside the plugin: it plays the one of the application's corpus (its settings say
  where), or a `modal-profile.bin` chosen in the plugin (Choose profile...), whose path the project keeps. It loads
  in the background, once per process for every instance.
- Any block size, sample-accurate MIDI. Over 50 kHz the engine runs at 44.1 or 48 kHz as in the application and is
  brought up to the host's rate: a latency of 127 samples at 192 kHz, reported to the host. None otherwise.
- Tested as a host loads it (`ctest -R plugin_host`, `plugin/tests/host_test.cpp`): the same sound as the engine,
  blocks of 1 to 512 samples, automation, headphones, a project saved and opened again, a host at 192 kHz, the cost.
  `PianoForgePluginTest <PianoForge.vst3> --show-editor 10` opens its window.
- The engine needs a processor with AVX2. VST3 only for now (CLAP later).

## Install (Windows)

With Inno Setup 6 installed:

```
cmake --build --preset windows-release --target installer
```

makes `build/windows-msvc/installer/output/PianoForge-<version>-setup.exe` (the version is the project's, in the
root `CMakeLists.txt`), with `latest.json` (the update manifest) and the installer's SHA-256 next to it. The
installer holds the application, the Visual C++ runtime it needs (next to it: nothing else to install), the VST3
plugin (in the VST3 folder of every user, or of the current user) and, optionally, the OBS plugin (when OBS Studio is installed; a plugin OBS is using moves aside and the new one loads at
its next start). It installs for the current user without administrator rights (`%LOCALAPPDATA%\Programs\PianoForge`),
or for every user if chosen. Settings, takes and recordings are never touched, by an update or an uninstall.
It never holds the corpus nor a profile calibrated on it (the build refuses anything but its ten files): the
installed application asks for its corpus (File > Change corpus). Not code-signed: Windows SmartScreen warns
about it the first time.

## Releases

A version tag makes a release (`.github/workflows/release.yml`): set the version in `CMakeLists.txt`
(`project(PianoForge VERSION x.y.z)`), commit, then `git tag vx.y.z && git push origin vx.y.z`. The workflow checks
that the tag is that version, builds and tests on Windows, Ubuntu 24.04 and macOS, and opens a **draft** release with:

| Asset | For |
|---|---|
| `PianoForge-x.y.z-setup.exe` | Windows 10 and 11: the application, the VST3 plugin, the OBS plugin (built against OBS 33.0.1) |
| `pianoforge_x.y.z_amd64.deb` | Ubuntu 24.04 and later, Debian 13: the application, the VST3 and LV2 plugins |
| `pianoforge-x.y.z-1.x86_64.rpm` | Fedora 40 and later: the same |
| `PianoForge-x.y.z-macOS-arm64.zip` | macOS 11 and later on Apple silicon: the application, VST3 and AU, not signed (`INSTALL.txt` inside) |
| `SHA256SUMS` | every asset's SHA-256 |
| `<bank>.modal-profile.bin` and `.json` | a modal profile from a free bank and its credit (v0.1.0: the Salamander Grand Piano V3, CC-BY 3.0), uploaded by hand to the release; the web page offers it |

Read and complete the notes GitHub wrote (the commits since the last release), then publish the draft: only then does
Help > Check for updates see it (with `https://api.github.com/repos/<owner>/<repo>/releases/latest` as its address).
Nothing is signed, so Windows' SmartScreen and macOS's Gatekeeper ask for a confirmation at the first start. No
release holds a corpus or a profile.

## Updates

Help > Update settings (Windows and Linux): the address of an update manifest (`latest.json` as written by the
installer build; on Linux its `deb` entry: `url`, `sha256`, `size`; its
`url` may be relative to it, or set at build time with `-DPIANOFORGE_UPDATE_BASE_URL=https://...`) or of a GitHub
release (`https://api.github.com/repos/<owner>/<repo>/releases/latest`: the asset ending in `-setup.exe`, on Linux in
`_amd64.deb`, its digest
or a `.sha256` asset). https or a file; empty by default, and then nothing is looked for. Help > Check for updates,
or at start once a day if ticked. A newer version is offered (install, skip this version, later); it is downloaded
to the temporary folder, its size and SHA-256 checked, then PianoForge closes, the installer runs silently and opens
it again; on Linux the package goes to the system's software installer (which asks for the right to install it), and
PianoForge is restarted once it is in. `PianoForge.exe --check-update <address> report.txt` reads an address and
checks its installer without running it.

## Reference bench

```
python -m venv .venv
.venv\Scripts\python -m pip install -e tools\bench
.venv\Scripts\python -m unittest discover -s tools\bench\tests
.venv\Scripts\bench --help
```

See `tools/bench/README.md`.

## Reference banks

The engine was studied on one commercial library; free sample banks are references too, rendered by the bench's SFZ
player through the same sessions and measured by the same forge. The three banks are kept as references; which one
the published sound rests on is decided at the end of the project (a profile built from a free bank can be published,
with its credit).

| Bank | Piano | Recording | Licence | Corpus state |
|---|---|---|---|---|
| Noire (Kontakt) | Yamaha CFX | Native Instruments | commercial (EULA): the renders and the profile stay local | `noire-dry` |
| [Salamander Grand Piano V3](https://github.com/sfzinstruments/SalamanderGrandPiano) | Yamaha C5 | Alexander Holm: 48 kHz, 24-bit, 16 velocity layers every minor third, an AB pair of AKG C414; SFZ by SFZ Instruments | [CC-BY 3.0](https://creativecommons.org/licenses/by/3.0/) | `salamander-dry` |
| [Piano in 162](https://www.ivyaudio.com/Piano-in-162) | Steinway B | Ivy Audio: 44.1 kHz, 24-bit, every key, 5 dynamics, 2 round robins, pedal up and down, close and ambient microphones; SFZ by Marcelo Colina | free for personal and commercial use (no licence text comes with it) | `in162-close` |

The banks are not in the repository, nor are the profiles made from them. Anyone can make them again, or make one
from any other SFZ piano, with the profile builder. It fetches the bank (Salamander automatically; Piano in 162 by
hand, from Ivy Audio's torrent), renders it through the bench, measures it with the forge, and resumes after a stop:

```
.venv\Scripts\python -m pip install -e tools\bench -e tools\forge -e tools\profile
.venv\Scripts\pianoforge-profile doctor
.venv\Scripts\pianoforge-profile build salamander
.venv\Scripts\pianoforge-profile build --sfz "D:\Banks\My Grand.sfz" --state my-grand --license "..."
```

The forge monitor's *New profile...* does the same in a window. **[docs/building-a-profile.md](docs/building-a-profile.md)**
explains the whole method: what the bench renders, what each forge step measures and how, the choices and how to
change them, how to check a profile, times, space and licences. `studies/compare_banks.py` sets the banks' measures
side by side: tuning, inharmonicity, sustain, brightness, dynamics, dampers.

## Forge

```
.venv\Scripts\python -m pip install -e tools\forge
.venv\Scripts\python -m unittest discover -s tools\forge\tests
```

| Command | Output (in the corpus state folder) |
|---|---|
| `forge release-curves` | Damper curves measured from the release takes (`release-curves.json`). |
| `forge analyze` | f0, inharmonicity, stretched tuning, decays per note; first profile (`physics.toml`). |
| `forge modes` | Every partial resolved into its modes, the unison strings and their polarisations: beating and double decay (`modes-v064.json`). |
| `forge soundboard` | The soundboard body: resonances shared by every note, amplitudes per note (`soundboard-v064.json`). Needs `forge modes`. The resonances are measured at velocity 64 and reused for the other velocities. |
| `forge export` | The modal profile played by the engine (`modal-profile.bin`), from the modes and the soundboard of every measured velocity layer, the damper curves and the attack residuals; and each layer's measures for the engine between two layers (section LEVL): the attack's energy as recorded, and each partial's level as it sounds over the sustain. A profile exported before them still plays, with the attack at the overall gain between two layers. |

`forge pipeline` runs every step of a state's profile in order (analyze, modes and soundboard at each layer,
release-curves, release-noise, half-pedal, sympathetic, export), logs to `forge-log.txt` and writes its progress
to `progress.json` in the state's folder:

```
.venv\Scripts\forge pipeline --corpus corpus\reference\noire-dry
.venv\Scripts\forge pipeline --corpus corpus\reference\in162-close --layers 64,32,80,96,127 --skip sympathetic
```

(`--resume` keeps the layers already measured since the last render.) The forge monitor follows every pipeline and
every profile build of a corpus (the steps, the one under way, the time left and the end of the log), and starts
new builds (New profile..., see docs/building-a-profile.md):

```
cmake --build --preset windows-release --target PianoForgeForgeMonitor
build\windows-msvc\tools\forge\monitor\PianoForgeForgeMonitor_artefacts\Release\PianoForgeForgeMonitor.exe corpus
```

The forge's heavy loops have native kernels (`tools/forge/native`, C++, built with the project, or alone:
`cmake -S tools/forge/native -B build/forge-native`), loaded by the Python forge when built; the Python code stays
the reference and the fallback (`PIANOFORGE_FORGE_NATIVE=0`), and the tests check one against the other:

| Kernel | Used by | Python | Native |
|---|---|---|---|
| Variable projection of a partial's modes (Kaufman's Jacobian) | modes | 59 s a bass note | 10 s |
| Sum of damped sinusoids (the modes, the body) | soundboard, export | 16 s a note | 1.3 s |
| Bank of one-pole resonators (the free strings) | sympathetic | 64 s a pedal pair | 4.9 s |
| Three-string unison (coupled strings, closed-form cubic) | export | 88 s a layer | 2.9 s |

With them (and one BLAS thread per worker: the pipeline sets it), a velocity layer's modes take about 3 minutes
instead of half an hour. A few ill-conditioned partials depend on the order of floating-point sums: their modes
change with a multithreaded BLAS, never between the native and the Python code under the same settings.

Every forge command takes `--corpus corpus/reference/<state>` (default: `noire-dry`). A profile calibrated on Noire
stays local, like its corpus, and is never published; one built from a free bank may be, with the bank's credit.

## Licence

PianoForge is free software under the [GNU Affero General Public License](LICENSE), version 3 or (at your option)
any later version (`AGPL-3.0-or-later`): the whole repository and its whole history. You may use, study, change and
pass it on; whoever passes on a changed version, or offers one to others over a network (the web build), gives its
source under the same licence. [NOTICE.md](NOTICE.md) lists the third-party components (JUCE, used under its AGPLv3
option; the VST3, AU, LV2 and ASIO SDKs; PipeWire's and JACK's headers) and the licences of the sample banks.

The engine still takes no GPL or AGPL code from others and no `juce::`: its own licence is a choice, not a
consequence. The bench uses pedalboard (GPL-3.0) as a development tool only; it is never linked into the engine.
A modal profile follows the licence of the bank it was measured from, not this one (see the banks above).

Noire (Native Instruments) served during development only as a local measurement and listening reference: none of
its audio, nor any profile or data set made from it, is in this repository or in the releases (NOTICE.md). Product
and maker names are trademarks of their owners; PianoForge is not affiliated with them.
