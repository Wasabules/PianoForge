# Building a profile from a sample bank

PianoForge's engine does not play samples. It plays **modes**: for every key and every velocity layer, the damped
sinusoids a real piano's strings make, as measured on a recording, plus what the strings do not explain (the
soundboard, the hammer's knock), the dampers, the pedal and the strings that ring in sympathy. The file that holds
all of it is a **modal profile** (`modal-profile.bin`). This guide makes one from a sample bank, explains every
measure, and says how to check the result.

One command does it all (`pianoforge-profile build`), resumable, with a window to follow it (the forge monitor).
The rest of this page is what that command does and why, for whoever wants to change, check or extend it.

- [What you need](#what-you-need)
- [Quick start](#quick-start)
- [The chain](#the-chain)
- [What is measured, and how](#what-is-measured-and-how)
- [Choices, and how to change them](#choices-and-how-to-change-them)
- [Checking a profile](#checking-a-profile)
- [Times, space, resuming](#times-space-resuming)
- [Licences](#licences)
- [Troubleshooting](#troubleshooting)

## What you need

- **A sampled piano in SFZ**: the 88 keys (21, A0 to 108, C8), several dynamics. The more the bank has, the more the
  profile holds:
  - every key sampled is better than one sample stretched over several keys (a stretched sample carries the wrong
    inharmonicity and the wrong unison beating for the keys it is moved to);
  - each **dynamic layer** gives the engine a real strike to play at that velocity: 5 is the minimum for a good
    profile, 10 to 16 is better;
  - **pedal-down samples** (the strings ringing in sympathy) let the sympathetic resonance be calibrated on the bank
    itself; without them it is borrowed from a bank that has them, or left out;
  - **close, dry microphones** are best: the engine adds the room (and the listening position) itself.
- **Python 3.11 or later**, and the project's three tools in a virtual environment.
- **CMake and a C++ compiler** for the forge's native kernels: optional, but the forge is about ten times slower
  without them.
- **About 2 GB free** per bank for its renders and measures, besides the bank itself (0.7 GB for Salamander,
  4.9 GB zipped for Piano in 162).
- **The application** (or the VST3 plugin) to play the profile.

## Quick start

From the repository's root.

Windows:

```
python -m venv .venv
.venv\Scripts\python -m pip install -e tools\bench -e tools\forge -e tools\profile
cmake -S tools\forge\native -B build\forge-native
cmake --build build\forge-native --config Release
.venv\Scripts\pianoforge-profile doctor
.venv\Scripts\pianoforge-profile build salamander
```

Linux and macOS:

```
python3 -m venv .venv
.venv/bin/python -m pip install -e tools/bench -e tools/forge -e tools/profile
cmake -S tools/forge/native -B build/forge-native -DCMAKE_BUILD_TYPE=Release
cmake --build build/forge-native
.venv/bin/pianoforge-profile doctor
.venv/bin/pianoforge-profile build salamander
```

`doctor` says what is there and what is missing. `build salamander` downloads the Salamander Grand Piano (the SFZ
Instruments repository at a pinned commit, file by file, each one checked against its git hash), renders it, measures
it and writes `corpus/reference/salamander-dry/modal-profile.bin`. Run the same command again after a stop: it
resumes.

The banks the tool knows (`pianoforge-profile banks`):

| Bank | Piano | Layers | Pedal-down samples | How it is fetched | Licence |
|---|---|---|---|---|---|
| `salamander` | Yamaha C5 | 16 | no (the calibration is borrowed from `piano-in-162` when it is built) | automatically | CC-BY 3.0 |
| `piano-in-162` | Steinway B | 5 | yes | by hand (Ivy Audio's torrent), then checked | free for personal and commercial use, no licence text |

Piano in 162 is handed out by a torrent only: `pianoforge-profile fetch piano-in-162` says where to put it, then
`build piano-in-162` checks it is whole.

**Any other SFZ piano:**

```
pianoforge-profile build --sfz "D:\Banks\My Grand\My Grand.sfz" --state my-grand --license "CC-BY 4.0" --author "..."
```

**With a window:** the forge monitor (built with the project: `cmake --build --preset windows-release --target
PianoForgeForgeMonitor`) has *New profile...*: a bank the tool knows or any SFZ file, the number of cores, then
Build. Each build gets a card with its stages beside the forge's own, its log below; *Stop* ends it (the same build
resumes it). Closing the monitor does not stop a build.

**Playing the profile:**

- in PianoForge, *Engine > Profile B...*, pick the `modal-profile.bin`, then Ctrl+3 plays it (Ctrl+2 the profile
  of the corpus, Ctrl+1 the bank's samples through the reference player: an A/B against the source);
- or *File > Change corpus...*, pick `corpus/reference/<state>/single-notes`: the profile becomes the main one, and
  the reference player plays that bank's renders;
- in the VST3 plugin, *Choose profile...*.

## The chain

```
 sample bank (SFZ)
     |  bench prepare-sfz        the bank as a "state": its file, controllers, licence, source
     v
 corpus/states/<state>.json
     |  bench render             the measurement sessions, played through the bench's SFZ player
     v
 corpus/reference/<state>/   single-notes/, releases/      (FLAC takes, index.csv, manifest.json)
 corpus/interactions/<state>/ pedal-down/, pedal-up/, half-pedal/
     |  forge pipeline           analysis, modes, soundboard, dampers, noises, pedal, sympathy, export
     v
 corpus/reference/<state>/modal-profile.bin  (+ every measure as JSON, with its plots)
```

`pianoforge-profile build` runs these as seven stages, each in `build.json` (what the monitor shows) and
`build-log.txt`:

| Stage | What it does |
|---|---|
| check | The bank there (fetched if the tool knows how), whole (no sample missing), the 88 keys; its layers and pedal-down samples read from the SFZ; the tools installed. |
| prepare | `bench prepare-sfz`: the state, with the controllers the recipe sets (Salamander: its key-release and pedal noises off) and the samples' leading silence cut to 3 ms. Skipped when the state is already so. |
| render-reference | `bench render reference.toml --passes single-notes,releases`. |
| render-interactions | `bench render interactions.toml --passes pedal-down,pedal-up,half-pedal`. |
| layers | Which rendered velocities to measure, and where the sympathetic calibration comes from (written to `build-plan.json`). |
| forge | `forge pipeline --resume`, with those layers and that calibration. |
| verify | The profile there, a modal profile (its header), newer than the renders. |

### The bench: rendering a bank as one would record a piano

The bench plays the bank like a pianist would play a real piano in a studio, through **sessions** written as data
(`tools/bench/sessions/*.toml`): which notes, at which velocities, held how long, with which pedal. The SFZ player is
the bench's own (`tools/bench/bench/sfz.py`), with the opcodes pianos use (velocity layers, round robins, release
triggers, controller conditions, envelopes, amplitude curves). It departs from a sampler three times, on purpose:

- **one random draw per strike**, not per take: the same strike in two passes plays the same round robin, so a
  release take and its held twin are identical up to the key release (the forge measures the dampers on that
  difference);
- **no gap between random ranges** (Piano in 162 has one, 0.500 to 0.501: a strike there would be silent);
- **attacks aligned**: each sample's leading silence (up to 70 ms in Piano in 162) cut down to 3 ms, as a key's
  sound starts on a real piano.

Every take is written as FLAC with its metadata (the key, the velocity, when the key went down and up, where the
sample ran out), deterministic (rendered again, the same bits), and resumable: a take already rendered with the same
state and settings is kept.

The passes the forge reads:

| Session | Pass | What | Used for |
|---|---|---|---|
| reference | single-notes | the 88 keys at 16 velocities (8 to 127), held as long as each register rings | everything: the partials, modes, body, residual of every layer |
| reference | releases | one key in four, velocities 32, 80, 120, released after 0.1, 0.5 and 2 s | the dampers, the release noise |
| interactions | pedal-down / pedal-up | every key at 40 and 100, key released at 4 s with the pedal down, against held 10 s with it up | the sympathetic resonance |
| interactions | half-pedal | C2, C4, C6 at 80, the pedal lifted from 127 to 0 over 4 s | the half pedal |

## What is measured, and how

`forge pipeline` runs these steps in order (its progress in `progress.json`, its log in `forge-log.txt`); each one
writes its measures as JSON with a plot to look at.

1. **Analysis** (`analysis.json`, `physics.toml`, `analysis.png`). Every note's partials at velocity 64: the
   fundamental, the inharmonicity B of the stiff-string law f_n = n f0 sqrt(1 + B n^2), smoothed along the
   keyboard; the stretched tuning; the partials' decays (prompt and after-sound). The first, physical profile.

2. **Modes**, at each velocity layer (`modes-vNNN.json`, `.png`). Each partial of each note resolved into its
   modes: the 2 or 3 strings of a unison, each in two polarisations, with their frequency, decay and complex
   amplitude per channel. The fit is a variable projection (the frequencies and decays nonlinear, the amplitudes
   linear, solved exactly at each step; Kaufman's Jacobian) on a window that starts once the hammer has left the
   strings. This is where beating and double decay come from: the engine plays them as measured, not synthesized.

3. **Unison** (in the export). The modes of each partial fitted by the coupled-strings model (Weinreich): the strings
   detuned slightly, all pulling on the bridge, which couples them. Struck together they move together and die fast
   (the prompt sound); their detuning leaks energy into motions that hardly move the bridge and die slowly (the
   after-sound). The model is what lets the engine detune the unisons (Engine settings, *Unison tuning*) or play a
   soft pedal and still sound right.

4. **Soundboard** (`soundboard-vNNN.json`). What the strings leave over, below the fundamental, is the soundboard and
   the case. Their resonances belong to the instrument; only how hard each note drives them changes. So 200
   resonances are found once, on the treble notes (whose partials leave the low band free), by multichannel ESPRIT,
   then each note's amplitudes on them by least squares, leaving out the bins under its own partials.

5. **Release curves** (`release-curves.json`). A release take and the same note held longer are the same recording
   until the key goes up; the ratio of their powers after it is what the damper does, per velocity and per group of
   partials (the felt stops the high partials first), smoothed and made non-increasing (a damper only removes
   energy).

6. **Release noise** (`release-noise.json`). The thump of the damper landing, on the soundboard resonances, per note,
   velocity and how long the key was held.

7. **Half pedal** (`pedal-curve.json`). How far the dampers lift per pedal value (CC64), from the half-pedal ramps:
   the note's level against it held without pedal, read back through its release curve.

8. **Sympathetic resonance** (`sympathetic.json`). With the pedal down, every string is free: the engine models each
   one as resonators (one per measured mode) driven through the bridge by what is played. The coupling (a gain per
   third octave, and an extra decay) is fitted on the pedal-down / pedal-up pairs, on their powers: what the pedal
   adds to the note. (A bank's pedal-down samples are often other strikes than its pedal-up ones; when the attack
   shows two strikes apart, the comparison is made on power, which does not depend on the phases.) A bank without
   pedal-down samples borrows the coupling of one that has them (the strings' own modes are always its own).

9. **Export** (`modal-profile.bin`). Every layer's modes and body, the dampers, the noises, the pedal and the free
   strings, and the **attack residual** of each note and layer: what neither the strings nor the body explain (the
   hammer, the first instants before the modes settle), cut from the take itself, so the attack is exact. The layers
   are kept as measured, each a real strike: between two layers, the engine moves from one strike to the other
   rather than mixing them.

The method, its measures and what was learned on the way are in the analysis document
(`Pré-analyse - Concept, forme et choix techniques.md`, in French).

## Choices, and how to change them

| Choice | Default | Change it with |
|---|---|---|
| Velocity layers measured | `auto`: one rendered velocity per layer of samples, the loudest inside it (where the layer plays at full level); a bank with 12 layers or more takes the whole grid of 16 | `--layers all`, or `--layers 32,64,96,127` (rendered velocities only) |
| Sympathetic calibration | `auto`: the bank's own pedal-down samples; else the first bank in the corpus calibrated on its own; else left out (the profile plays without sympathetic strings) | `--sympathetic-from piano-in-162` (a bank or state), a `sympathetic.json` path, or `none` |
| The bank's controllers | the recipe's (Salamander: noises off with cc21 and cc22 at 0) | `--cc N=VALUE` (0 to 127), `--hdcc N=VALUE` (0 to 1), with `--sfz` |
| Leading silence | cut to 3 ms | `--align-onsets MS`, a negative value to keep it |
| Render rate | the bank's own | `--sample-rate` |
| Measures | resumed: the layers already measured since the last render are kept | `--fresh`: everything measured again (the renders are kept) |
| Cores | all but one or two | `--jobs N` |

A recipe for a new bank is a TOML file in `tools/profile/pfprofile/banks/` (see the two there): where it comes from,
its licence and credit, the state's settings, the layers and the sympathetic calibration.

## Checking a profile

- **The plots**: `analysis.png` (inharmonicity, tuning, decays along the keyboard: smooth curves, no outliers),
  `modes-vNNN.png` (each note's envelope error: a few dB; a note at tens of dB is a fit that failed).
- **The log**: `forge-log.txt` ends with the export's count of fits left out (a few tens of modes out of tens of
  thousands is normal: they are the partials too weak or too short to measure).
- **Listening against the source**: in PianoForge, Ctrl+1 plays the bank's own renders (the reference player),
  Ctrl+3 the profile chosen as B, from the same keyboard. The *Performance* window and the self-test
  (`PianoForge.exe --selftest corpus/reference/<state>/single-notes report.txt`) check the engine on it.
- **Side by side with other banks**: `studies/compare_banks.py` sets the measures of several states against each
  other (tuning, inharmonicity, sustain, brightness, dynamics, dampers).

## Times, space, resuming

Measured on a 12-core machine (8 used), native kernels built:

| Bank | Renders | Forge | Space (renders and measures) |
|---|---|---|---|
| Piano in 162 (5 layers) | 2 min | about 20 min | 1.5 GB |
| Salamander (16 layers) | 10 to 20 min | about 50 min | 2 GB |

A forge without its native kernels takes about ten times as long.

Everything resumes: a take already rendered is kept, a layer already measured (and newer than the renders) is kept.
Stop a build with Ctrl+C, or *Stop* in the monitor (`pianoforge-profile stop <state>`), and run the same build
again. `pianoforge-profile status <state>` says where a build is.

## Licences

A profile is derived from the bank it was measured on: **its licence goes with it**. The bench keeps the bank's
licence, author and source in the state and in every corpus manifest, and the tool prints the credit due at the end
of a build.

- **Salamander Grand Piano**: CC-BY 3.0. A profile made from it may be shared, crediting *Salamander Grand Piano V3
  by Alexander Holm (CC-BY 3.0), SFZ by SFZ Instruments*.
- **Piano in 162**: free for personal and commercial use by Ivy Audio's page, but no licence text comes with it: ask
  Ivy Audio before publishing what derives from it.
- **Commercial libraries** (Kontakt instruments and the like): their licences usually forbid passing on the samples
  or anything rendered from them. Keep their renders and profiles to yourself: the repository ignores the whole
  `corpus/` folder for that reason.

## Troubleshooting

| Message | What to do |
|---|---|
| `the bench package is not installed` | `pip install -e tools/bench -e tools/forge -e tools/profile` in the project's environment. |
| `the forge's native kernels are not built` | Build them (Quick start); the forge works without, ten times slower. |
| `N of M samples missing` | The bank is incomplete: fetch it again (`fetch` resumes and checks every file), or extract the archive whole. |
| `the bank's keys go from ... to ...` | A piano profile needs the 88 keys. |
| `opcodes the bench's player does not play` | Listed in the check stage: usually harmless (effects, UI); if the bank relies on one for its sound, the renders will differ from what a sampler plays. |
| A download cut | `fetch` and `build` resume each file where it stopped, with up to 8 tries; run the command again if it gives up. |
| `sympathetic resonance: ... left out` | The bank has no pedal-down samples and no bank calibrated on its own is built: build one that has them (Piano in 162), then this one again; or keep the profile without it. |
| A forge step failed | `forge-log.txt` has the reason; fix it and run the build again (it resumes). |

Another kind of source (a Kontakt instrument, through the bench's VST3 host on Windows) goes through the bench
directly: see `tools/bench/README.md` (`bench prepare`, then the same sessions and `forge pipeline`).
