# PianoForge

A concert grand piano simulator played from a MIDI keyboard: a calibrated hybrid modal engine, with
recording of every take and technician-level settings. Cross-platform (Windows first, Linux compatible),
low latency (key to sound <= 10 ms, jitter <= 1 ms).

Status: proof of concept. The modal engine is not written yet; the application currently plays a
reference corpus rendered from a sampled grand piano, which the modal engine will be compared with.

## Layout

| Folder | Content |
|---|---|
| `engine/` | Core, pure C++20 without framework. For now the `pf::Engine` interface shared by every engine. |
| `app/` | Standalone application (JUCE 9): listening lab with an 88-key keyboard, MIDI input, audio device selection. |
| `tools/bench/` | Reference bench (Python): renders a VST3 instrument through measurement sessions and checks the takes. |
| `studies/` | Small standalone computations behind design decisions (resonator precision, CPU budget). |
| `corpus/` | Rendered takes and plugin states. Local only, never committed (commercial sample library licences). |

## Build the application

Requirements: CMake >= 3.25, Visual Studio 2022 (Windows) or GCC/Ninja and the JUCE Linux dependencies (Linux).
JUCE is fetched automatically at configure time.

```
cmake --preset windows-msvc
cmake --build --preset windows-release
build/windows-msvc/app/PianoForgeApp_artefacts/Release/PianoForge.exe
```

Headless self-test of the reference player:

```
PianoForge.exe --selftest corpus/reference/noire-dry/single-notes selftest.txt
```

## Reference bench

```
python -m venv .venv
.venv\Scripts\python -m pip install -e tools\bench
.venv\Scripts\python -m unittest discover -s tools\bench\tests
.venv\Scripts\bench --help
```

See `tools/bench/README.md`.

## Licences

The engine is meant to stay under a permissive licence. The application uses JUCE (AGPLv3 or JUCE licence).
The bench uses pedalboard (GPL-3.0) as a development tool only; it is never linked into the engine.
