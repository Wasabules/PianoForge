// Checks the browser module under Node, as the AudioWorklet will use it: no imports, the real profile, the same
// sound as the native engine, and its cost.
//   node web/check.mjs [modal-profile.bin] [native modal_render executable]
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const profilePath = process.argv[2] ?? path.join(root, 'corpus/reference/noire-dry/modal-profile.bin');
const nativeExe = process.argv[3] ?? path.join(root, 'build/windows-msvc/engine/Release/modal_render.exe');
let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? '[PASS]' : '[FAIL]'}  ${what}`);
  if (!ok) failures += 1;
};

// The module, instantiated as the worklet does: no Emscripten runtime, only the memory-growth notification (the
// worklet makes its views of the memory again anyway).
const module = new WebAssembly.Module(fs.readFileSync(path.join(here, 'site/pianoforge.wasm')));
const imports = WebAssembly.Module.imports(module).map((i) => `${i.module}.${i.name}`);
check(imports.every((i) => i === 'env.emscripten_notify_memory_growth'),
  `standalone module, imports: ${imports.length ? imports.join(', ') : 'none'}`);
const pf = new WebAssembly.Instance(module, { env: { emscripten_notify_memory_growth() {} } }).exports;
pf._initialize?.();

function load(rate) {
  pf.pf_init(rate);
  const bytes = fs.readFileSync(profilePath);
  const ptr = pf.pf_alloc(bytes.length);
  new Uint8Array(pf.memory.buffer, ptr, bytes.length).set(bytes);
  const t0 = performance.now();
  const status = pf.pf_load_profile(ptr, bytes.length);
  const ms = performance.now() - t0;
  pf.pf_free(ptr);
  return { status, ms, mb: bytes.length / 1e6 };
}

/** Renders `seconds` at `rate` in blocks of `block`; events: [sample, status, data1, data2], ascending. */
function render(rate, seconds, events, block) {
  const frames = Math.round(seconds * rate);
  const l = new Float32Array(frames), r = new Float32Array(frames);
  let next = 0;
  for (let s = 0; s < frames; s += block) {
    const n = Math.min(block, frames - s);
    while (next < events.length && events[next][0] < s + n) {
      const [, status, d1, d2] = events[next++];
      pf.pf_event(status, d1, d2, 0);
    }
    pf.pf_render(n);
    l.set(new Float32Array(pf.memory.buffer, pf.pf_left(), n), s);
    r.set(new Float32Array(pf.memory.buffer, pf.pf_right(), n), s);
  }
  return [l, r];
}

function readWav(file) {
  const b = fs.readFileSync(file);
  const data = b.indexOf('data', 12);
  const frames = b.readUInt32LE(data + 4) / 8;
  const l = new Float32Array(frames), r = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    l[i] = b.readFloatLE(data + 8 + i * 8);
    r[i] = b.readFloatLE(data + 12 + i * 8);
  }
  return [l, r];
}

const db = (x, a, b) => {
  let e = 0;
  for (let i = a; i < b; i++) e += x[i] * x[i];
  return 10 * Math.log10(e / (b - a) + 1e-30);
};

if (!fs.existsSync(profilePath)) {
  console.log(`no profile at ${profilePath}: only the module was checked`);
  process.exit(failures ? 1 : 0);
}

// 1. The real profile.
const loaded = load(44100);
check(loaded.status === 0 && pf.pf_profile_notes() === 88,
  `profile parsed in the module: ${pf.pf_profile_notes()} notes x ${pf.pf_profile_layers()} layers, ${loaded.mb.toFixed(1)} MB in ${loaded.ms.toFixed(0)} ms, ` +
  `memory ${(pf.memory.buffer.byteLength / 1e6).toFixed(0)} MB`);

// 2. The same note as the native engine (modal_render render: note-on at 0, off at 3 s, blocks of 64).
if (fs.existsSync(nativeExe)) {
  const cases = [[60, 64], [21, 100], [96, 40]];
  for (const [note, velocity] of cases) {
    const wav = path.join(os.tmpdir(), `pianoforge-native-${note}-${velocity}.wav`);
    execFileSync(nativeExe, [profilePath, 'render', wav, String(note), String(velocity), '3', '2', '44100']);
    const [nl, nr] = readWav(wav);
    fs.rmSync(wav);
    load(44100);
    const [wl] = render(44100, nl.length / 44100, [[0, 0x90, note, velocity], [3 * 44100, 0x80, note, 0]], 64);
    let peak = 0, diff = 0;
    for (let i = 0; i < nl.length; i++) {
      peak = Math.max(peak, Math.abs(nl[i]));
      diff = Math.max(diff, Math.abs(nl[i] - wl[i]));
    }
    const windows = [[0, 0.1], [0.1, 1], [1, 3], [3, 5]].map(([a, b]) => db(wl, a * 44100, b * 44100) - db(nl, a * 44100, b * 44100));
    const worst = Math.max(...windows.map(Math.abs));
    // Phase-insensitive: without fused multiply-adds, the phasors round differently and drift apart in phase over
    // seconds (the samples then differ by -50 dB or so), while the sound is the same.
    check(worst < 0.05,
      `note ${note} velocity ${velocity} against the native engine: level per window within ${worst.toFixed(3)} dB ` +
      `(samples apart by ${(20 * Math.log10(diff / peak + 1e-12)).toFixed(0)} dB under the peak: phase drift)`);
  }
} else {
  console.log(`no native ${nativeExe}: the comparison is skipped`);
}

// 3. Cost: the native bench's case (sustain down, 64 notes 10 ms apart from the bass up, velocity 80), 48 kHz,
//    render quanta of 128 frames, as an AudioWorklet runs it.
load(48000);
pf.pf_set_room(0, 0, 0.85, 2.2, 0.5, 1, 1, 1, 0);
const events = [[0, 0xb0, 64, 127]];
for (let i = 0; i < 64; i++) events.push([Math.floor((i * 48000) / 100), 0x90, 21 + ((i * 7) % 88), 80]);
let t0 = performance.now();
render(48000, 4.0, events, 128);
const dry = (performance.now() - t0) / 1000;
load(48000);
pf.pf_set_room(1, 0, 0.85, 2.2, 0.5, 6, 1, 1, 0); // the front row: the direct path's filters run too
t0 = performance.now();
render(48000, 4.0, events, 128);
const withRoom = (performance.now() - t0) / 1000;
load(48000);
pf.pf_set_room(1, 0, 0.85, 2.2, 0.5, 6, 1, 1, 1); // and on headphones
t0 = performance.now();
const [earL, earR] = render(48000, 4.0, events, 128);
const withEars = (performance.now() - t0) / 1000;
check(withRoom / 4.0 < 0.8 && withEars / 4.0 < 0.8 && earL.every(Number.isFinite) && earR.every(Number.isFinite),
  `64 voices with the pedal down, 48 kHz, quanta of 128: ${((100 * dry) / 4.0).toFixed(1)} % of real time ` +
  `(${((100 * withRoom) / 4.0).toFixed(1)} % with the room, ${((100 * withEars) / 4.0).toFixed(1)} % on headphones), ` +
  `${pf.pf_resonators()} resonators at the end`);

process.exit(failures ? 1 : 0);
