# Reference bench

Plays a VST3 instrument (Kontakt 8 with Noire) through a measurement session and stores every take
(FLAC + JSON) under `corpus/`, which is never committed.

## Install

```
python -m venv .venv
.venv\Scripts\python -m pip install -e tools\bench
.venv\Scripts\python -m unittest discover -s tools\bench\tests
```

## Use

All commands run from the project root.

| Step | Command |
|---|---|
| See what a session will render | `.venv\Scripts\bench list tools\bench\sessions\reference.toml` |
| Prepare a Kontakt state (a window opens: load and set up the instrument, then close it) | `.venv\Scripts\bench prepare --state noire-dry` |
| Render a session, or only some passes | `.venv\Scripts\bench render tools\bench\sessions\reference.toml --state noire-dry --passes layer-discovery` |
| Check determinism, and the fast render against a render paced at real time | `.venv\Scripts\bench check tools\bench\sessions\smoke.toml --state noire-dry --count 20` |
| Recompute the analyses of rendered takes (no Kontakt needed) | `.venv\Scripts\bench reanalyze tools\bench\sessions\reference.toml --state noire-dry` |
| Summarize a corpus | `.venv\Scripts\bench summary tools\bench\sessions\reference.toml --state noire-dry` |
| Probe the MIDI keyboard (pedal type, velocity range, release velocity) | `.venv\Scripts\bench keyboard` |

- **Resume:** `render` picks up where it stopped. Each take carries a key computed from its definition,
  the Kontakt state, the sample rate, the block size and the tool versions; a take is only rendered again
  when that key changes, or with `--redo-defects`.
- **Log:** `corpus/<session>/<state>/log.txt`.
- **Licence:** renders of a commercial library stay local (see the vendor's EULA).

## Sessions

- `reference.toml`: timbre of single notes, 2,393 takes, about 10 h 47 min of audio. It covers layer
  discovery (all velocities on 6 notes), 88 notes x 16 velocities, releases, repetitions and silence.
- `interactions.toml`: pedal down or up, half pedal, pedal noise.
- `smoke.toml`: 20 takes to validate a Kontakt state before a long generation.

Pass types: `note`, `repetition`, `pedal_ramp`, `pedal_only`, `silence` (see `bench/protocol.py`).
