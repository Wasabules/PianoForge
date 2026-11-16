# PianoForge OBS plugin

The PianoForge sources for OBS Studio, read straight from the application through shared memory: no virtual cable,
no system mixer in between, and the main audio output (ASIO) untouched.

- **PianoForge** (audio): the application's sound, in the mix chosen, at the stream's own level.
- **PianoForge keyboard** (video): the 88 keys as played, the pedals, and the notes rising from the keys as they
  sound, with lights and sparkles; it moves with the audio source's sound.
- **PianoForge text** (video): a template filled with the application's state (the MIDI file, the instrument
  preset, the room...), drawn by OBS's own text source.
- **Chapter markers**: at each take saved, recording started or stopped and MIDI file played in PianoForge, a chapter
  in OBS's recording (OBS 30.2 and later, recordings in Hybrid MP4).
- **The stream look** in PianoForge (Audio > Stream look): the keyboard's settings there and here, kept the same both
  ways, with the keyboard's preview as OBS renders it.
- **Piano gestures**: gestures played on PianoForge's keyboard (chosen in PianoForge, Keyboard > Piano gestures)
  start or stop OBS's recording or stream, add a chapter marker, or move to the next or previous scene, if the audio
  source lets them (on by default).

The texts are in English and French (`data/locale`).

## How it works

- **PianoForge** writes every block into a shared-memory feed ("PianoForgeFeed", `engine/include/pianoforge/audio_feed.hpp`):
  three mixes of the same frames, before its volume and limiter (as heard, on speakers, dry), its volume, the
  instant of each block on the machine's steady clock, the MIDI events at their frames, its state for overlays,
  and marks. Memory copies and atomic stores on the audio thread, nothing it could wait on. The speakers mix is
  rendered (a second room, the head left out) only while an OBS source takes it.
- **The audio source** reads the feed on its own thread every 2 ms and hands the sound to OBS at OBS's rate, stamped
  on OBS's clock. The feed runs on the audio device's clock, which drifts from the system's by tens to hundreds of
  ppm: the source measures the application's rate from its blocks' instants (a least-squares line, about a minute
  long) and resamples at it (`pf::DriftResampler`, 16-tap windowed sinc, straight to OBS's rate: one resampling), a
  calm proportional loop holding the audio waiting at the latency set. OBS never sees a drift (fed device timestamps,
  it would let the gap grow to 70 ms and then jump). The fill level is counted from the blocks' instants: the
  sawtooth of the application's blocks (ragged when they come in bursts) does not reach the ratio.
- Every start fades in, every planned stop fades out over 10 ms (a new mix, latency or format, the resync button,
  the application's audio stopping): no click in the stream. A limiter keeps the peaks under -1 dBFS.
- **The keyboard** reads the events from the feed and shows the frame the audio source stamps at that instant (or,
  without an audio source, the application's newest frame less 20 ms): the image meets the sound. It is drawn by a
  shader of its own (`data/pianoforge-keyboard.effect`, installed beside the locales): rounded boxes with a gradient,
  a rim and a light within (keys, notes, pedals, shadows), halos and beams added over them, and the sparkles, many to
  a draw. A busy image (fifteen notes, their lights, sparkles) takes about a millisecond of the graphics thread.
- Each source says it reads the feed (a slot in it, every quarter of a second): PianoForge's Session card shows
  what OBS reads, and how late the sound is from the key to OBS.
- **The stream look** goes through a shared memory of its own (`pianoforge/look_channel.hpp`, "PianoForgeFeedLook",
  either side creating it): each side's look as `key=value` lines (`pianoforge/stream_look.hpp`, the table both sides
  build their settings from: keys, ranges, defaults, pages, styles), with the wall-clock instant of its edit. Each
  side takes the other's when it is newer (at a tie, OBS's), and sends its own when it changes; the plugin brings
  every keyboard source synchronised with PianoForge ("Synchronised with PianoForge", on by default: they share one
  look) to it, and an open properties window shows it. While PianoForge's window asks for the preview, a private
  keyboard source (in no scene) is rendered after OBS's main view, at most 30 times a second and as large as the
  window shows it, staged on the GPU one frame and read back the next, and its frames written to the channel.

## Build and install

### Windows

The plugin is built with the rest of the project when OBS Studio is installed (`C:/Program Files/obs-studio`,
or `-DPIANOFORGE_OBS_DIR=...`). The libobs and frontend API headers of the matching version are fetched
(`PIANOFORGE_OBS_VERSION`, checked against `PIANOFORGE_OBS_HASH`); the import libraries are made from the installed
`obs.dll` and `obs-frontend-api.dll`.

```
cmake --build build/windows-msvc --config Release --target install-obs-plugin
```

copies it to `%ProgramData%/obs-studio/plugins/pianoforge-obs` (no administrator rights needed), OBS running or not:
a plugin in use is moved aside (removed by a later install) and the new one loads at OBS's next start.

### Linux

**A Flatpak OBS** (Flathub's, the usual one on Ubuntu): the plugin is built inside that OBS's own SDK, against its
libobs (the SDK of OBS's runtime, which `flatpak info com.obsproject.Studio` names: `flatpak install flathub
org.freedesktop.Sdk//26.08` for OBS 33), and `install-obs-plugin` puts it where that OBS looks,
`~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/pianoforge-obs`:

```
flatpak run --devel --command=sh com.obsproject.Studio -c \
  "cmake -S obs -B build/obs-flatpak -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/app && \
   cmake --build build/obs-flatpak --target install-obs-plugin"
flatpak override --user --device=shm com.obsproject.Studio
```

The override, once: a Flatpak keeps a `/dev/shm` of its own (OBS's `devices=all` notwithstanding), where
PianoForge's feed is not; `--device=shm` gives it the system's. Without it, the sources wait for PianoForge for ever.
The tests run there too (`ctest --test-dir build/obs-flatpak`, in the same `flatpak run --devel` shell): with OBS
33.0.1 (OpenGL on Wayland, an Intel UHD 630), every check passes, a busy keyboard image drawn in about 2.5 ms.

As a Flatpak extension, for Flathub (`obs/flatpak/com.obsproject.Studio.Plugin.PianoForge.json`, with its AppStream
metadata): `flatpak-builder build/flatpak-obs obs/flatpak/com.obsproject.Studio.Plugin.PianoForge.json` builds it
against that OBS and its SDK (lib/obs-plugins and share/obs/obs-plugins under /app/plugins/PianoForge, where OBS merges
its extensions). An extension cannot grant OBS the shared memory: the override above stays the user's (PianoForge's
Audio > OBS plugin offers it in one click).

**A system OBS** (the distribution's, with its development files, `libobs-dev`; or OBS's PPA): built with the rest of
the project when libobs is found at configure time (`-DPIANOFORGE_OBS_PLUGIN=ON` forces it), and

```
cmake --build --preset linux-release --target install-obs-plugin
```

copies it to `~/.config/obs-studio/plugins/pianoforge-obs` (`$XDG_CONFIG_HOME`); `cmake --install` puts it in a
prefix's lib/obs-plugins (a package, component obs). Not tried yet.

The keyboard's shader is the same for OBS's two renderers: OpenGL takes no array of parameters (OBS writes `uniform
float4 a[64]` as a single vec4, and the whole effect fails to load), so the melody's points reach it as a 64 x 1
texture, three of them in turns (OpenGL waits for the GPU before writing a texture again).

### macOS

A bundle, `pianoforge-obs.plugin` (its data in `Contents/Resources`), copied to
`~/Library/Application Support/obs-studio/plugins` by `install-obs-plugin`. Not run yet.

### Then

The application and the plugin must be of the same version (the feed's layout changed with this one: an older plugin
waits for PianoForge, a newer application is not seen by it). Then add the sources: **PianoForge** (audio),
**PianoForge keyboard** and **PianoForge text**. Turn the audio source's monitoring off in OBS (Advanced Audio
Properties) so that it is not heard twice.

## Properties

**PianoForge** (audio):

- **Status** (at the bottom, so that its lines never move the controls): connected or not, the rates and block
  size, how long it has run; the latency aimed at and the audio waiting, from the key to OBS (to set a camera's Sync
  Offset), the drift compensated, the resyncs of this audio session; the limiter when it acts. It refreshes itself
  when its state changes; the measures, with Refresh.
- **Mix**: on speakers (default: the room as speakers give it, even while PianoForge is heard on headphones, whose
  binaural rendering colours the sound on speakers), as heard, or dry (the piano without the room).
- **Follow PianoForge's volume** (off by default: the stream keeps its level whatever volume is heard), **Gain**.
- **Latency** (10 to 100 ms, 20 by default), **Resynchronise**.
- **Chapter markers in OBS recordings** (on by default).
- **Let PianoForge's gestures control OBS** (on by default).

**PianoForge keyboard**: a **style** sets the whole look at once (PianoForge's defaults, Concert, Neon, Galaxy,
Rainbow, Minimal; the size, the keys shown and the timing kept), and a button brings every setting back to its
default (the width kept). The settings are grouped: Keyboard, Colours, Notes, Lights, Sparkles, Velocity, Powerful
chords.

The keys shown: all 88, a smaller keyboard's (76, 61, 49, 37), from one key to another, or following the playing
(the view glides to the keys played in the last 4 s, held or ringing, at least so many octaves wide, and stays put
while the playing stays inside it); the keys keep a piano's proportions for their width.

The notes: they last as the sound does (a note whose key is up but a pedal still holds, sustain or sostenuto, goes on
as a fainter tail until the pedal lets it go or the key is struck again, then its end draws in and fades out: the
tails' width, opacity and fine end are set; while the sustain pedal is down, a line of light along the keyboard and
a soft halo rising from it, each its own strength, 0 for none); along its bar a note dims as a string dies away (slowly in the bass, quickly in the treble);
the melody (the right hand's top notes) is a line of light through them.

The lights also: a bloom (the keyboard drawn into an image, its bright coloured parts, not the white keys, blurred
at a quarter of its size and added over it), a reflection (the band over the keys a black lacquer mirroring the
notes and lights above it), an aurora (curtains of light over the room, as strong as PianoForge's sound is loud: its
strength, height, swaying speed, and what stays of it when nothing sounds).

Width; the notes above the keys (height, time to the top; 0: the keyboard alone), rounded
bars with a bright rim and a halo, fading as they rise; the pedals. The keys have rounded fronts and a front edge
that sinks as they go down; the black keys cast their shadow. The colours, chosen by hue (a slider in degrees) and
saturation: each hand's (estimated from the notes: where each hand plays, how far it stretches, chords too wide for
one hand split at their widest interval; or a fixed split; blue and orange by default), one for the white keys and
one for the black, one for all, or the rainbow from the bass to the treble; the black keys take a deeper shade of
their hue. The lights: beams rising from the keys held, halos, the keys lit from within (0 to 100 %); sparkles
thrown up by each strike in a short stream, the more and the higher the stronger it is, a few rising while a key is
held (0 to 100 %), their height (a share of the room above the keys), spread (0: a thread straight up, 90 degrees: a
wide fountain) and direction (leaning left or right); a burst and a flare along the keyboard at the strike; an
afterglow once a key is up; an offset against the sound.

The velocity (group *Velocity*): its accent (soft notes thinner, deeper and more transparent, strong ones wider with
a white-hot core and a wider halo, a ripple of light from the key at the strike; the keys' light by it), against the
keyboard's strongest velocity (112 by default, a Korg B2's; a button takes the strongest strike of the last minute).

**Powerful chords** (a checkable group, on by default): a chord of at least so many notes (3) struck within 35 ms,
their mean strength over a threshold (75 % of the strongest velocity), is known once its window has passed (all its
notes in) and becomes an event, at its intensity: a wave of light running along the keys and the rail, the room
above the keys lit in each hand's hue, fireworks (a rocket from each hand, one more for six notes or more, bursting
at about half the room), the chord's notes tied at their tops by a bar of light, and, if asked, the keyboard
shaking a few pixels. After one, the next waits 0.6 s: a fortissimo passage is not a firework at every chord.

**PianoForge text**: the template (fields `{file}`, `{state}`, `{preset}`, `{room}`, `{engine}`, `{recording}`; a
line whose fields are all empty disappears), then OBS's text properties (font, colours, outline, background...).

## Tests

- `stream_look_test` (ctest `stream_look`): the settings' table, their text read back, the styles; the channel
  (each side's look, once; the preview's request and frames).
- `pianoforge-obs-overlay-test` (ctest `obs_overlay`): the keyboard's geometry, the keys and pedals at a frame, the
  notes kept for the trails and struck for the sparkles, the colours by hue, the sparkles (a stream rises, slows and
  goes out; fireworks burst), the powerful chords (found once their window has passed; not a soft chord, an arpeggio
  or two notes; not too soon after another), how long notes sound (the sustain pedal, the sostenuto, a key struck
  again), the keys shown (a range; the view following the playing), the melody, the text template.
- `pianoforge-obs-test` (ctest `obs_plugin`, about 45 s) loads the plugin into the installed libobs without the OBS
  application (audio, and video through OBS's Direct3D 11 module, offscreen; OBS's text source beside it), writes
  the feed as PianoForge does (three mixes of a 1 kHz sine, on a clock 200 ppm fast, the volume at -6 dB, MIDI
  events) and checks: the speakers mix by default at its own level; continuous audio and timestamps; the frequency
  on OBS's clock (within a few ppm); no glitch; the status and the key-to-OBS latency; a new latency, the resync
  button; another mix with the volume followed, through fades (no step); the readers the application sees; the
  keyboard rendered (each hand's colour, a key not lit, a note rising, the sustain pedal; a phrase played, its
  images kept beside the report; sparkles after strikes, leaning as asked, and none when off; a soft note thinner
  and duller than a strong one; a powerful chord lighting the room and throwing fireworks, nothing for a soft chord
  nor with them off; a pedal's tail, the melody's line, 61 keys and the view following the playing, the bloom, the
  reflection, the ambiance; the styles and the reset; the rainbow; the time a busy image takes; no word from
  Direct3D); the text filled and resized;
  a mark logged while OBS does not record.
  The test's feed has a name of its own (`PIANOFORGE_AUDIO_FEED`): it runs beside a PianoForge and an OBS in use.
