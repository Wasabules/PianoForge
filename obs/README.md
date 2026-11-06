# PianoForge OBS plugin

The "PianoForge" audio source for OBS Studio: the sound of the PianoForge application, read straight from it
through shared memory. No virtual cable, no system mixer in between, and the main audio output (ASIO) untouched.

## How it works

- **PianoForge** writes every block it sends to the audio device into a shared-memory ring ("PianoForgeAudioFeed",
  `engine/include/pianoforge/audio_feed.hpp`): a memory copy and one atomic store on the audio thread, nothing it
  could wait on.
- **The plugin** reads the ring on its own thread every 2 ms and hands the audio to OBS, stamped on OBS's clock.
  The feed runs on the audio device's clock, which drifts from the system's by tens to hundreds of ppm: the plugin
  resamples it (`pf::DriftResampler`, 16-tap windowed sinc, ratio corrected by a PI loop on the frames waiting), so
  that OBS never sees a drift (fed device timestamps, OBS would let the gap grow to 70 ms and then jump). About
  20 ms of audio wait in the plugin.

## Build and install (Windows)

The plugin is built with the rest of the project when OBS Studio is installed (`C:/Program Files/obs-studio`,
or `-DPIANOFORGE_OBS_DIR=...`). The libobs headers of the matching version are fetched (`PIANOFORGE_OBS_VERSION`,
checked against `PIANOFORGE_OBS_HASH`); the import library is made from the installed `obs.dll`.

```
cmake --build build/windows-msvc --config Release --target install-obs-plugin
```

copies it to `%ProgramData%/obs-studio/plugins/pianoforge-obs/bin/64bit` (no administrator rights needed). Restart
OBS, then add a source: **PianoForge** (under audio sources). Turn its monitoring off in OBS (Advanced Audio
Properties) so that it is not heard twice.

## Test

`pianoforge-obs-test` (ctest `obs_plugin`, about 40 s, PianoForge closed) loads the plugin into the installed
libobs without the OBS application, writes the feed as PianoForge does (a 1 kHz sine on a clock 200 ppm fast) and
checks what libobs hands on: continuous audio and timestamps, the frequency on OBS's clock, no glitch.
