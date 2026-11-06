# PianoForge in a browser

The modal engine and its room, compiled to WebAssembly, played from a page: an 88-key keyboard (mouse, computer
keyboard, Web MIDI), the sustain and sostenuto pedals, the room, the engine's options, and a MIDI file player (the
lab's own sequence player, to the sample in the audio worklet; open or drop a .mid file, Ctrl+Space to play).

- `src/exports.cpp`: the engine's C interface (`pf_*`), a standalone module with no Emscripten JavaScript.
- `site/`: the page (`index.html`, `app.js`, `style.css`) and the audio worklet (`worklet.js`), which instantiates
  `pianoforge.wasm` on the browser's audio thread.
- `check.mjs`: runs the module under Node as the worklet does: imports, the real profile, the same sound as the
  native engine (level per window), the cost.

The profile is never uploaded: the page asks for `modal-profile.bin` (written by `forge export`), keeps it in the
browser (IndexedDB) and hands it to the worklet. Calibrated on a commercial library, it must not be published: the
page can be put online only with a profile made from free material.

## Build

Emscripten (emsdk, for instance in `~/emsdk`), CMake and Ninja:

```
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm          # also copies pianoforge.wasm into web/site/
ctest --test-dir build/wasm       # the engine's tests, run by Node
node web/check.mjs                # the module against the native engine (needs the native build and a profile)
```

## Play

A local server (the audio worklet and Web MIDI need `localhost` or HTTPS):

```
python -m http.server 8080 --bind 127.0.0.1 --directory web/site
```

then <http://localhost:8080>: choose `corpus/reference/noire-dry/modal-profile.bin` once, then Start.

Measured (Node 22, 48 kHz, render quanta of 128): the profile parses in about 0.1 s; 64 voices with the pedal down
and the sympathetic strings take about 55 % of the audio thread (the native engine, AVX2: 17 to 20 %). The output
latency is the browser's and the system's (10 to 40 ms), not ASIO's. Web MIDI: Chrome, Edge, Firefox; not Safari.
