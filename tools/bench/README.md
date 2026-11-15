# Reference bench

Plays an instrument through a measurement session and stores every take (FLAC + JSON) under `corpus/`, which is
never committed. The instrument is a VST3 plugin (Kontakt 8 with Noire), or an SFZ sample bank played by the bench's
own SFZ player (`bench/sfz.py`): the free banks (Salamander Grand Piano, Piano in 162) go through the same sessions
and give the same corpus, which the forge measures the same way.

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
| Prepare an SFZ state (the bank, its controllers, its licence and source) | `.venv\Scripts\bench prepare-sfz --state salamander-dry --sfz "corpus\sources\salamander\SalamanderGrandPiano\Salamander Grand Piano V3.sfz" --hdcc 21=0 --hdcc 22=0 --align-onsets 3 --license "CC-BY 3.0"` |
| Render a session, or only some passes | `.venv\Scripts\bench render tools\bench\sessions\reference.toml --state noire-dry --passes layer-discovery` |
| Check determinism, and the fast render against a render paced at real time | `.venv\Scripts\bench check tools\bench\sessions\smoke.toml --state noire-dry --count 20` |
| Recompute the analyses of rendered takes (no Kontakt needed) | `.venv\Scripts\bench reanalyze tools\bench\sessions\reference.toml --state noire-dry` |
| Summarize a corpus | `.venv\Scripts\bench summary tools\bench\sessions\reference.toml --state noire-dry` |
| Probe the MIDI keyboard (pedal type, velocity range, release velocity) | `.venv\Scripts\bench keyboard` |

- **Resume:** `render` picks up where it stopped. Each take carries a key computed from its definition,
  the Kontakt state, the sample rate, the block size and the tool versions; a take is only rendered again
  when that key changes, or with `--redo-defects`.
- **SFZ banks:** a state made by `prepare-sfz` (`corpus/states/<name>.json`, `"player": "sfz"`) renders the bank at
  its own sample rate, in several processes (`--jobs`, by default the cores but two), each take from silence. The
  player follows sfizz where SFZ leaves it open; what it does not play (an opcode outside its list) is listed in the
  state and the manifest. It departs from a sampler three times, for the forge's measures: a strike plays the same
  sample in every take (the random draws depend on the strike, not on the take), a draw between two random ranges
  plays the nearest one instead of silence, and `--align-onsets MS` starts each sample MS ms before its attack
  (banks leave uneven silence before it: up to 70 ms in Piano in 162, 12 ms in most of Salamander's).
- **Log:** `corpus/<session>/<state>/log.txt`.
- **Licence:** renders of a commercial library stay local (see the vendor's EULA). A free bank's licence and
  source are kept in its state and in the corpus manifest; credit them wherever what is derived from it goes (the
  Salamander Grand Piano is CC-BY 3.0: Alexander Holm, the SFZ by SFZ Instruments).

## Sessions

- `reference.toml`: timbre of single notes, 2,393 takes, about 10 h 47 min of audio. It covers layer
  discovery (all velocities on 6 notes), 88 notes x 16 velocities, releases, repetitions and silence.
- `interactions.toml`: pedal down or up, half pedal, pedal noise, and probes written event by event (held keys,
  pedal pressed after the note or lifted under a held key, re-pedalling).
- `smoke.toml`: 20 takes to validate a Kontakt state before a long generation.

Pass types: `note`, `repetition`, `pedal_ramp`, `pedal_only`, `sequence` (takes written out event by event),
`midi_file`, `silence` (see `bench/protocol.py`).
