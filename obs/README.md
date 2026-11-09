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

## Build and install (Windows)

The plugin is built with the rest of the project when OBS Studio is installed (`C:/Program Files/obs-studio`,
or `-DPIANOFORGE_OBS_DIR=...`). The libobs and frontend API headers of the matching version are fetched
(`PIANOFORGE_OBS_VERSION`, checked against `PIANOFORGE_OBS_HASH`); the import libraries are made from the installed
`obs.dll` and `obs-frontend-api.dll`.

```
cmake --build build/windows-msvc --config Release --target install-obs-plugin
```

copies it to `%ProgramData%/obs-studio/plugins/pianoforge-obs` (no administrator rights needed), OBS running or not:
a plugin in use is moved aside (removed by a later install) and the new one loads at OBS's next start. The
application and the plugin must be of the same version (the feed's layout changed with this one: an older plugin
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

**PianoForge keyboard**: width; the notes above the keys (height, time to the top; 0: the keyboard alone), rounded
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

- `pianoforge-obs-overlay-test` (ctest `obs_overlay`): the keyboard's geometry, the keys and pedals at a frame, the
  notes kept for the trails and struck for the sparkles, the colours by hue, the sparkles (a stream rises, slows and
  goes out; fireworks burst), the powerful chords (found once their window has passed; not a soft chord, an arpeggio
  or two notes; not too soon after another), the text template.
- `pianoforge-obs-test` (ctest `obs_plugin`, about 45 s) loads the plugin into the installed libobs without the OBS
  application (audio, and video through OBS's Direct3D 11 module, offscreen; OBS's text source beside it), writes
  the feed as PianoForge does (three mixes of a 1 kHz sine, on a clock 200 ppm fast, the volume at -6 dB, MIDI
  events) and checks: the speakers mix by default at its own level; continuous audio and timestamps; the frequency
  on OBS's clock (within a few ppm); no glitch; the status and the key-to-OBS latency; a new latency, the resync
  button; another mix with the volume followed, through fades (no step); the readers the application sees; the
  keyboard rendered (each hand's colour, a key not lit, a note rising, the sustain pedal; a phrase played, its
  images kept beside the report; sparkles after strikes, leaning as asked, and none when off; a soft note thinner
  and duller than a strong one; a powerful chord lighting the room and throwing fireworks, nothing for a soft chord
  nor with them off; the rainbow; the time a busy image takes; no word from Direct3D); the text filled and resized;
  a mark logged while OBS does not record.
  The test's feed has a name of its own (`PIANOFORGE_AUDIO_FEED`): it runs beside a PianoForge and an OBS in use.
