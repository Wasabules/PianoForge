// SPDX-License-Identifier: AGPL-3.0-or-later

// PianoForge in a browser: the page (keyboard, MIDI, settings, the profile kept in this browser) and the audio
// worklet that runs the engine (worklet.js, pianoforge.wasm).
const $ = (id) => document.getElementById(id);

// ---- Settings kept in this browser (per viewer, best effort) ----
const store = {
  get(key, fallback) {
    try {
      const v = localStorage.getItem('pianoforge.' + key);
      return v === null ? fallback : JSON.parse(v);
    } catch { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem('pianoforge.' + key, JSON.stringify(value)); } catch { /* private window */ }
  },
};

// ---- The profile, kept in IndexedDB (never uploaded) ----
const profileDb = {
  open() {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open('pianoforge', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('files');
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  },
  async run(mode, action) {
    const db = await this.open();
    return new Promise((resolve, reject) => {
      const tx = db.transaction('files', mode);
      const request = action(tx.objectStore('files'));
      tx.oncomplete = () => resolve(request?.result);
      tx.onerror = () => reject(tx.error);
    });
  },
  get() { return this.run('readonly', (s) => s.get('modal-profile')).catch(() => undefined); },
  put(entry) { return this.run('readwrite', (s) => s.put(entry, 'modal-profile')); },
  remove() { return this.run('readwrite', (s) => s.delete('modal-profile')); },
};

// ---- State ----
const ROOM_PRESETS = [ // as the lab's (app/src/ui/RoomPanel.cpp): size (60 to 40 000 m3), decay s, brightness, width
  { name: 'Studio', size: 0.14, decay: 0.6, brightness: 0.6, width: 0.8 },
  { name: 'Chamber', size: 0.5, decay: 1.4, brightness: 0.55, width: 1 },
  { name: 'Concert hall', size: 0.85, decay: 2.2, brightness: 0.5, width: 1 },
  { name: 'Large hall', size: 0.93, decay: 3.0, brightness: 0.45, width: 1 },
  { name: 'Cathedral', size: 1.0, decay: 5.5, brightness: 0.35, width: 1 },
];
const ROOM_POSITIONS = [ // as the lab's: metres from the piano (1 m: the engine's sound, heard from the keyboard)
  { name: 'At the keyboard (1 m)', distance: 1 },
  { name: 'On stage (3 m)', distance: 3 },
  { name: 'Front row (6 m)', distance: 6 },
  { name: 'Middle of the hall (12 m)', distance: 12 },
  { name: 'Back of the hall (20 m)', distance: 20 },
];
const ROOM_MODEL = 2; // the stored settings' model: 2, the listening position (1: a level against the dry sound)
const OPTIONS = [ // pf_set_option
  ['Soundboard body', true], ['Attack residual', true], ['Sympathetic resonance', true],
  ['Held keys resonate', true], ['Pedal catches releases', true], ['Release noise', true],
];
const COMPUTER_KEYS = ['KeyA', 'KeyW', 'KeyS', 'KeyE', 'KeyD', 'KeyF', 'KeyT', 'KeyG', 'KeyY', 'KeyH', 'KeyU', 'KeyJ',
  'KeyK', 'KeyO', 'KeyL', 'KeyP', 'Semicolon']; // physical places: QWERTY names, AZERTY keys alike

let context = null, node = null, profile = null, midi = null;
let midiFile = null;           // { name, bytes, notes, tracks, length, parts } once the worklet has read it
let transposition = 0;
let filePosition = 0, filePlaying = false, seeking = false;
let fileKeys = new Set();      // the keys the MIDI file has sounding (from the worklet)
let octave = store.get('octave', 4), velocity = store.get('velocity', 100);
const held = new Map();        // computer key code -> note
const pointers = new Map();    // pointer id -> note
const sounding = new Uint8Array(128); // notes down, any source (for the keyboard's display)
const keys = new Map();        // note -> element

function message(text, kind = '') {
  $('message').textContent = text;
  $('message').className = 'message ' + kind;
}

// ---- What the page is doing: the header's line, a bar filling as a download goes, moving while the size is unknown ----
const tasks = new Map(); // name -> { text, fraction }: the last one started is shown

function task(name, text, fraction = null) {
  tasks.delete(name);
  tasks.set(name, { text, fraction });
  showTasks();
}

function taskDone(name) {
  tasks.delete(name);
  showTasks();
}

function showTasks() {
  const shown = [...tasks.values()].pop();
  $('busy').hidden = !shown;
  if (!shown) return;
  $('busyText').textContent = shown.text + (tasks.size > 1 ? ` (and ${tasks.size - 1} more)` : '');
  $('busyText').title = [...tasks.values()].map((t) => t.text).join('\n');
  const bar = $('busyBar');
  if (shown.fraction === null) bar.removeAttribute('value'); // indeterminate: it moves
  else bar.value = Math.min(1, Math.max(0, shown.fraction));
}

const MB = (bytes) => (bytes / 1e6).toFixed(0);

function send(command, transfer) {
  if (node) node.port.postMessage(command, transfer ?? []);
}

// ---- The keyboard ----
function buildKeyboard() {
  const board = $('keyboard');
  const whites = [];
  for (let note = 21; note <= 108; note++) {
    const black = [1, 3, 6, 8, 10].includes(note % 12);
    const key = document.createElement('div');
    key.className = black ? 'black' : 'white';
    key.dataset.note = note;
    if (!black) {
      if (note % 12 === 0) key.innerHTML = `<span class="label">C${note / 12 - 1}</span>`;
      board.appendChild(key);
      whites.push(key);
    } else {
      board.appendChild(key); // positioned after layout
    }
    keys.set(note, key);
  }
  const place = () => {
    const width = board.clientWidth / whites.length;
    let white = 0;
    for (let note = 21; note <= 108; note++) {
      const key = keys.get(note);
      if (key.className.startsWith('white')) { white++; continue; }
      key.style.width = `${width * 0.6}px`;
      key.style.left = `${white * width - width * 0.3}px`;
    }
  };
  new ResizeObserver(place).observe(board);
  place();

  const noteAt = (event) => {
    const element = document.elementFromPoint(event.clientX, event.clientY);
    return element?.dataset?.note ? { note: Number(element.dataset.note), element } : null;
  };
  const strike = (pointerId, hit, event) => {
    const rect = hit.element.getBoundingClientRect();
    const v = Math.max(1, Math.min(127, Math.round(1 + 126 * (event.clientY - rect.top) / rect.height)));
    noteOn(hit.note, v, false);
    pointers.set(pointerId, hit.note);
  };
  board.addEventListener('pointerdown', (event) => {
    const hit = noteAt(event);
    if (!hit) return;
    board.setPointerCapture(event.pointerId);
    strike(event.pointerId, hit, event);
  });
  board.addEventListener('pointermove', (event) => { // a glissando
    if (!pointers.has(event.pointerId)) return;
    const hit = noteAt(event);
    if (hit && hit.note !== pointers.get(event.pointerId)) {
      noteOff(pointers.get(event.pointerId));
      strike(event.pointerId, hit, event);
    }
  });
  const release = (event) => {
    if (!pointers.has(event.pointerId)) return;
    noteOff(pointers.get(event.pointerId));
    pointers.delete(event.pointerId);
  };
  board.addEventListener('pointerup', release);
  board.addEventListener('pointercancel', release);
}

function show(note, down) {
  if (note < 0 || note > 127) return;
  sounding[note] = Math.max(0, sounding[note] + (down ? 1 : -1));
  keys.get(note)?.classList.toggle('down', sounding[note] > 0 || fileKeys.has(note));
}

/** The MIDI file's sounding keys, from the worklet: the display follows. */
function showFileKeys(list) {
  const now = new Set(list);
  for (const note of new Set([...fileKeys, ...now]))
    keys.get(note)?.classList.toggle('down', sounding[note] > 0 || now.has(note));
  fileKeys = now;
}

function noteOn(note, v, curved) {
  send({ type: 'midi', status: 0x90, data1: note, data2: v, curved });
  show(note, true);
}

function noteOff(note) {
  send({ type: 'midi', status: 0x80, data1: note, data2: 64, curved: false });
  show(note, false);
}

function pedal(controller, down) {
  send({ type: 'midi', status: 0xb0, data1: controller, data2: down ? 127 : 0, curved: false });
}

// ---- The computer keyboard ----
function typing(event) {
  return event.target instanceof HTMLInputElement && event.target.type !== 'range' && event.target.type !== 'checkbox';
}

window.addEventListener('keydown', (event) => {
  if ($('settings').open) return; // the settings dialog keeps its keys (Esc closes it); releases still pass
  if (event.ctrlKey && !event.altKey && !typing(event)) { // the MIDI file player
    if (event.code === 'Space') { event.preventDefault(); togglePlay(); }
    else if (event.code === 'Home') { event.preventDefault(); stopFile(); }
    else if (event.code === 'ArrowLeft' || event.code === 'ArrowRight') { event.preventDefault(); nudge(event.code === 'ArrowLeft' ? -5 : 5); }
    return;
  }
  if (typing(event) || event.ctrlKey || event.metaKey || event.altKey) return;
  if (event.code === 'Space') { event.preventDefault(); if (!event.repeat) pedal(64, true); return; }
  if (event.repeat) return;
  const index = COMPUTER_KEYS.indexOf(event.code);
  if (index >= 0) {
    const note = 12 * (octave + 1) + index;
    if (!held.has(event.code) && note >= 21 && note <= 108) {
      held.set(event.code, note);
      noteOn(note, velocity, false);
    }
  } else if (event.code === 'KeyZ' || event.code === 'KeyX') {
    octave = Math.max(0, Math.min(7, octave + (event.code === 'KeyX' ? 1 : -1)));
    $('octave').textContent = octave;
    store.set('octave', octave);
  } else if (event.code === 'KeyC' || event.code === 'KeyV') {
    velocity = Math.max(1, Math.min(127, velocity + (event.code === 'KeyV' ? 10 : -10)));
    $('velocityOut').textContent = velocity;
    store.set('velocity', velocity);
  } else if (event.code === 'KeyB') {
    pedal(66, true);
  } else if (event.code === 'KeyN') {
    pedal(67, true);
  }
});

window.addEventListener('keyup', (event) => {
  if (event.code === 'Space' && !event.ctrlKey) { pedal(64, false); return; }
  if (event.code === 'KeyB') { pedal(66, false); return; }
  if (event.code === 'KeyN') { pedal(67, false); return; }
  const note = held.get(event.code);
  if (note !== undefined) {
    held.delete(event.code);
    noteOff(note);
  }
});

window.addEventListener('blur', () => { // keys released while the page was not looking
  for (const note of held.values()) noteOff(note);
  held.clear();
});

// ---- MIDI keyboards (Web MIDI) ----
async function connectMidi() {
  if (!navigator.requestMIDIAccess) {
    $('midiLine').textContent = 'MIDI: this browser has no Web MIDI (Chrome, Edge and Firefox do)';
    return;
  }
  try {
    midi = await navigator.requestMIDIAccess({ sysex: false });
  } catch (error) {
    $('midiLine').textContent = 'MIDI: not allowed (' + error.message + ')';
    return;
  }
  const attach = () => {
    const names = [];
    for (const input of midi.inputs.values()) {
      input.onmidimessage = ({ data }) => {
        const [status, data1 = 0, data2 = 0] = data;
        const type = status & 0xf0;
        if (type !== 0x80 && type !== 0x90 && type !== 0xb0) return;
        if (type === 0xb0 && data1 === 64) { // the sustain pedal, through its curve
          send({ type: 'midi', status, data1, data2: pedalCurve(data2), curved: false });
          return;
        }
        send({ type: 'midi', status, data1, data2, curved: true });
        if (type === 0x90 && data2 > 0) show(data1, true);
        else if (type === 0x80 || type === 0x90) show(data1, false);
      };
      names.push(input.name);
    }
    $('midiLine').textContent = names.length ? 'MIDI: ' + names.join(', ') : 'MIDI: no keyboard connected';
  };
  midi.onstatechange = attach;
  attach();
}

// ---- The MIDI file player (it plays in the worklet, to the sample: the lab's sequence player) ----
const clock = (seconds) => {
  const s = Math.max(0, Math.floor(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

/** The time at the speed set: one second per second, against the duration at that speed; away from 100 %, the
 *  file's own time in brackets. */
function timeText(position, length, speed) {
  if (Math.abs(speed - 1) < 1e-3) return `${clock(position)} / ${clock(length)}`;
  return `${clock(position / speed)} / ${clock(length / speed)}   (file ${clock(position)} / ${clock(length)})`;
}

const speedFactor = () => Number($('midiSpeed').value) / 100;

function showPlayer() {
  const loaded = !!midiFile;
  for (const id of ['midiPlay', 'midiStop', 'midiPosition']) $(id).disabled = !loaded;
  $('midiPlay').innerHTML = filePlaying ? '&#10074;&#10074;' : '&#9654;';
  $('midiPlay').classList.toggle('on', filePlaying);
  if (!loaded) {
    $('midiTime').textContent = '-:-- / -:--';
    return;
  }
  $('midiPosition').max = String(Math.max(0.01, midiFile.length));
  if (!seeking) $('midiPosition').value = String(filePosition);
  const at = seeking ? Number($('midiPosition').value) : filePosition;
  $('midiTime').textContent = timeText(at, midiFile.length, speedFactor());
}

function openMidiBytes(name, bytes, credit = '') {
  midiFile = { name, bytes };
  $('midiCredit').textContent = credit; // a library piece's (its licence asks for it where it plays)
  if (node) send({ type: 'midiFile', bytes: bytes.slice(0) });
  else message(`${midiFile.name}: it goes to the engine once started (Start).`);
}

function openMidi(file) {
  if (!file) return;
  file.arrayBuffer().then((bytes) => openMidiBytes(file.name.replace(/\.(mid|midi|smf|kar)$/i, ''), bytes));
}

// ---- The MIDI library: media/midi (library.json and its files), copied next to the page by its build ----
let library = null;

async function loadLibrary() {
  try {
    const response = await fetch('midi/library.json', { cache: 'no-cache' });
    if (!response.ok) return;
    library = await response.json();
  } catch { return; } // a page served without it: only one's own files
  const list = $('libraryList');
  const groups = new Map();
  library.pieces.forEach((piece, i) => {
    if (!groups.has(piece.composer)) {
      const group = document.createElement('optgroup');
      group.label = piece.composer;
      groups.set(piece.composer, group);
      list.append(group);
    }
    const length = `${Math.floor(piece.seconds / 60)}:${String(piece.seconds % 60).padStart(2, '0')}`;
    groups.get(piece.composer).append(new Option(`${piece.title} (${length})`, String(i)));
  });
  $('library').hidden = !library.pieces.length;
}

$('libraryOpen').addEventListener('click', async () => {
  const piece = library?.pieces[Number($('libraryList').value)];
  if (!piece) return;
  try {
    const response = await fetch('midi/' + piece.file);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const collection = library.collections?.[piece.collection];
    openMidiBytes(`${piece.composer}: ${piece.title}`, await response.arrayBuffer(),
                  collection ? `MIDI: ${collection.credit} (${collection.license})` : '');
  } catch (error) {
    message(`Cannot open ${piece.title}: ${error.message}`, 'bad');
  }
});

function showParts() {
  const parts = midiFile?.parts ?? [];
  const list = $('partsList');
  list.textContent = '';
  parts.forEach((part, i) => {
    const label = document.createElement('label');
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = part.enabled;
    box.addEventListener('change', () => {
      part.enabled = box.checked;
      send({ type: 'midiPart', part: i, on: box.checked });
      showParts();
    });
    const name = part.name || `Track ${part.track + 1}`;
    label.append(box, ` ${i + 1}. ${name}   (channel ${part.channel + 1}${part.instrument ? ', ' + part.instrument : ''}, ${part.notes} notes)`);
    list.appendChild(label);
  });
  const playing = parts.filter((p) => p.enabled).length;
  $('midiPartsSummary').textContent = parts.length > 1 ? `Tracks ${playing}/${parts.length}` : 'Tracks';
  $('midiParts').style.visibility = midiFile ? 'visible' : 'hidden';
}

function setAllParts(on) {
  (midiFile?.parts ?? []).forEach((part, i) => {
    part.enabled = on;
    send({ type: 'midiPart', part: i, on });
  });
  showParts();
}

function transposeBy(semitones) {
  if (!midiFile) return;
  transposition = Math.max(-48, Math.min(48, transposition + semitones));
  send({ type: 'midiTranspose', semitones: transposition });
  showTranspose();
}

function showTranspose() {
  const t = transposition, n = Math.abs(t), sign = t > 0 ? '+' : '-';
  $('transposeOut').textContent = t === 0 ? '0' : `${sign}${n} st` + (n % 12 === 0 ? ` (${sign}${n / 12} oct)` : '');
  $('transposeOut').classList.toggle('on', t !== 0);
}

function togglePlay() {
  if (!midiFile) return;
  send({ type: filePlaying ? 'midiPause' : 'midiPlay' });
  filePlaying = !filePlaying;
  showPlayer();
}

function stopFile() {
  if (!midiFile) return;
  send({ type: 'midiStop' });
  filePlaying = false;
  filePosition = 0;
  showPlayer();
}

function nudge(seconds) {
  if (!midiFile) return;
  send({ type: 'midiSeek', seconds: Math.max(0, Math.min(midiFile.length, filePosition + seconds)) });
}

function buildPlayer() {
  $('midiFile').addEventListener('change', (event) => openMidi(event.target.files?.[0]));
  $('midiPlay').addEventListener('click', togglePlay);
  $('partsAll').addEventListener('click', () => setAllParts(true));
  $('partsNone').addEventListener('click', () => setAllParts(false));
  $('octDown').addEventListener('click', () => transposeBy(-12));
  $('semiDown').addEventListener('click', () => transposeBy(-1));
  $('semiUp').addEventListener('click', () => transposeBy(1));
  $('octUp').addEventListener('click', () => transposeBy(12));
  document.addEventListener('click', (event) => { // the parts menu closes on a click elsewhere
    if (!$('midiParts').contains(event.target)) $('midiParts').open = false;
  });
  showTranspose();
  $('midiStop').addEventListener('click', stopFile);
  $('midiPosition').addEventListener('input', () => { seeking = true; showPlayer(); });
  $('midiPosition').addEventListener('change', () => {
    seeking = false;
    filePosition = Number($('midiPosition').value);
    send({ type: 'midiSeek', seconds: filePosition });
    showPlayer();
  });
  $('midiSpeed').value = String(store.get('midiSpeed', 100));
  $('midiLoop').checked = store.get('midiLoop', false);
  const speedChanged = () => {
    $('midiSpeedOut').textContent = `${$('midiSpeed').value} %`;
    store.set('midiSpeed', Number($('midiSpeed').value));
    send({ type: 'midiSpeed', factor: speedFactor() });
    showPlayer();
  };
  $('midiSpeed').addEventListener('input', speedChanged);
  $('midiSpeed').addEventListener('dblclick', () => { $('midiSpeed').value = '100'; speedChanged(); });
  $('midiLoop').addEventListener('change', () => {
    store.set('midiLoop', $('midiLoop').checked);
    send({ type: 'midiLoop', on: $('midiLoop').checked });
  });
  $('midiSpeedOut').textContent = `${$('midiSpeed').value} %`;
  // A MIDI file dropped anywhere on the page.
  const midiIn = (event) => [...(event.dataTransfer?.items ?? [])].some((i) => i.kind === 'file');
  window.addEventListener('dragover', (event) => { if (midiIn(event)) { event.preventDefault(); document.body.classList.add('dropping'); } });
  window.addEventListener('dragleave', () => document.body.classList.remove('dropping'));
  window.addEventListener('drop', (event) => {
    event.preventDefault();
    document.body.classList.remove('dropping');
    const file = [...(event.dataTransfer?.files ?? [])].find((f) => /\.(mid|midi|smf|kar)$/i.test(f.name));
    if (file) openMidi(file);
  });
  showPlayer();
  showParts();
}

// ---- Settings ----
// The rooms of one's own: a built-in room's character, its level, a distance and keep loudness, named.
let userRooms = store.get('userRooms', []);
const roomChoice = () => { // the room list's selection: a built-in preset, or one of one's own
  const v = $('roomPreset').value;
  return v.startsWith('u') ? userRooms[Number(v.slice(1))] : ROOM_PRESETS[Number(v)];
};
function fillRooms(selected) {
  const list = $('roomPreset');
  list.innerHTML = '';
  ROOM_PRESETS.forEach((p, i) => list.add(new Option(p.name, String(i))));
  if (userRooms.length) {
    const group = document.createElement('optgroup');
    group.label = 'My presets';
    userRooms.forEach((p, i) => group.appendChild(new Option(p.name, 'u' + i)));
    list.appendChild(group);
  }
  list.value = selected;
  if (list.selectedIndex < 0) list.value = '2';
  $('roomDelete').disabled = !list.value.startsWith('u');
}

// The sustain pedal's curve: the travel it really covers, then a power law (the lab draws five points).
let pedalLearning = null;
function pedalCurve(value) {
  const lo = Number($('pedalMin').value) || 0, hi = Math.max(lo + 1, Number($('pedalMax').value) || 127);
  if (pedalLearning) {
    pedalLearning.min = Math.min(pedalLearning.min, value);
    pedalLearning.max = Math.max(pedalLearning.max, value);
  }
  const out = value <= lo ? 0 : value >= hi ? 127 : Math.round(127 * Math.pow((value - lo) / (hi - lo), Number($('pedalShape').value) || 1));
  $('pedalReadout').textContent = `Pedal: sends ${value} -> the engine receives ${out}`
    + (pedalLearning ? `   (learning: lowest ${pedalLearning.min}, highest ${pedalLearning.max})` : '');
  return out;
}

function settings() {
  const preset = roomChoice() ?? ROOM_PRESETS[2];
  return {
    volume: Number($('volume').value),
    room: { size: preset.size, decay: preset.decay, brightness: preset.brightness, width: preset.width, on: $('roomOn').checked,
            levelDb: Number($('roomLevel').value), distance: Number($('roomDistance').value), keepLoudness: $('roomKeep').checked,
            headphones: $('roomHeard').value === 'headphones' },
    pedal: { min: Number($('pedalMin').value), max: Number($('pedalMax').value), shape: $('pedalShape').value },
    options: OPTIONS.map((_, i) => $('option' + i).checked),
    sympathetic: Number($('sympathetic').value),
    unison: Number($('unison').value),
    absorption: Number($('absorption').value),
    a4: Number($('a4').value) || 440,
    curveMax: Math.max(20, Math.min(127, Number($('curveMax').value) || 127)),
  };
}

function apply() {
  const s = settings();
  $('volumeOut').textContent = `${s.volume.toFixed(1)} dB`;
  $('roomLevelOut').textContent = `${s.room.levelDb} dB`;
  $('roomDistanceOut').textContent = `${s.room.distance.toFixed(1)} m`;
  $('sympatheticOut').textContent = `${s.sympathetic} dB`;
  $('unisonOut').textContent = `${s.unison} %`;
  $('absorptionOut').textContent = `${s.absorption} %`;
  store.set('settings', { ...s, roomPreset: $('roomPreset').value, roomPosition: $('roomPosition').value, roomModel: ROOM_MODEL });
  send({ type: 'volume', db: s.volume });
  send({ type: 'room', ...s.room });
  s.options.forEach((on, option) => send({ type: 'option', option, on }));
  send({ type: 'sympathetic', db: s.sympathetic });
  send({ type: 'unison', spread: s.unison / 100 });
  send({ type: 'absorption', share: s.absorption / 100 });
  send({ type: 'tuning', a4: s.a4 });
  send({ type: 'curve', inputMin: 1, inputMax: s.curveMax, points: [0, 0.25, 0.5, 0.75, 1] });
}

// The settings dialog: a tab per subject, the last one opened remembered.
function buildSettingsDialog() {
  const dialog = $('settings');
  const show = (tab) => {
    document.querySelectorAll('.tab').forEach((t) => t.setAttribute('aria-selected', String(t.dataset.tab === tab)));
    document.querySelectorAll('.pane').forEach((p) => { p.hidden = p.dataset.pane !== tab; });
    store.set('settingsTab', tab);
  };
  document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => show(t.dataset.tab)));
  show(store.get('settingsTab', 'sound'));
  $('openSettings').addEventListener('click', () => dialog.showModal());
  $('closeSettings').addEventListener('click', () => dialog.close());
  dialog.addEventListener('click', (event) => { if (event.target === dialog) dialog.close(); }); // the backdrop
}

function buildSettings() {
  buildSettingsDialog();
  fillRooms('2');
  ROOM_POSITIONS.forEach((p, i) => $('roomPosition').add(new Option(p.name, String(i))));
  $('roomPosition').add(new Option('Custom', 'custom'));
  OPTIONS.forEach(([name, on], i) => {
    const label = document.createElement('label');
    label.innerHTML = `<input id="option${i}" type="checkbox"${on ? ' checked' : ''}> ${name}`;
    $('options').appendChild(label);
  });
  const saved = store.get('settings', null);
  fillRooms(String(saved?.roomPreset ?? 2));
  $('roomPosition').value = String(saved?.roomPosition ?? 0);
  $('roomDistance').value = saved?.room?.distance ?? (ROOM_POSITIONS[Number($('roomPosition').value)]?.distance ?? 1);
  $('roomKeep').checked = saved?.room?.keepLoudness ?? true;
  $('roomHeard').value = saved?.room?.headphones ? 'headphones' : 'speakers';
  if (saved?.pedal) {
    $('pedalMin').value = saved.pedal.min;
    $('pedalMax').value = saved.pedal.max;
    $('pedalShape').value = saved.pedal.shape;
  }
  if (saved) {
    $('volume').value = saved.volume;
    $('roomOn').checked = saved.room.on;
    $('roomLevel').value = saved.roomModel === ROOM_MODEL ? saved.room.levelDb : 0; // (the earlier model's level: not a trim)
    saved.options.forEach((on, i) => { $('option' + i).checked = on; });
    $('sympathetic').value = saved.sympathetic;
    $('unison').value = saved.unison ?? 100;
    $('absorption').value = saved.absorption ?? 50;
    $('a4').value = saved.a4;
    $('curveMax').value = saved.curveMax;
  }
  $('roomPreset').addEventListener('change', () => { // one of one's own brings its level, distance and keep loudness
    const choice = roomChoice();
    if ($('roomPreset').value.startsWith('u') && choice) {
      $('roomLevel').value = choice.levelDb;
      $('roomDistance').value = choice.distance;
      $('roomKeep').checked = choice.keepLoudness;
      const i = ROOM_POSITIONS.findIndex((p) => Math.abs(p.distance - choice.distance) < 0.05);
      $('roomPosition').value = i >= 0 ? String(i) : 'custom';
    }
    $('roomDelete').disabled = !$('roomPreset').value.startsWith('u');
    apply();
  });
  $('roomPosition').addEventListener('change', () => {
    const p = ROOM_POSITIONS[Number($('roomPosition').value)];
    if (p) $('roomDistance').value = p.distance;
    apply();
  });
  $('roomDistance').addEventListener('input', () => {
    const d = Number($('roomDistance').value);
    const i = ROOM_POSITIONS.findIndex((p) => Math.abs(p.distance - d) < 0.05);
    $('roomPosition').value = i >= 0 ? String(i) : 'custom';
    apply();
  });
  $('roomKeep').addEventListener('change', apply);
  $('roomHeard').addEventListener('change', apply);
  $('roomSave').addEventListener('click', () => {
    const current = roomChoice();
    const name = (prompt('A name for this room and this listening position (an existing name is replaced):',
      $('roomPreset').value.startsWith('u') ? current.name : '') ?? '').trim();
    if (!name) return;
    const s = settings();
    const preset = { name, size: s.room.size, decay: s.room.decay, brightness: s.room.brightness, width: s.room.width,
                     levelDb: s.room.levelDb, distance: s.room.distance, keepLoudness: s.room.keepLoudness };
    const i = userRooms.findIndex((p) => p.name === name);
    if (i >= 0) userRooms[i] = preset; else userRooms.push(preset);
    store.set('userRooms', userRooms);
    fillRooms('u' + (i >= 0 ? i : userRooms.length - 1));
    apply();
  });
  $('roomDelete').addEventListener('click', () => {
    const v = $('roomPreset').value;
    if (!v.startsWith('u')) return;
    const p = userRooms[Number(v.slice(1))];
    if (!confirm(`Delete the preset "${p.name}"? The room as it sounds now stays.`)) return;
    userRooms.splice(Number(v.slice(1)), 1);
    store.set('userRooms', userRooms);
    // The room as it sounds: the built-in one with the same character, or the default.
    const same = ROOM_PRESETS.findIndex((r) => r.size === p.size && r.decay === p.decay);
    fillRooms(String(same >= 0 ? same : 2));
    apply();
  });
  for (const id of ['pedalMin', 'pedalMax', 'pedalShape'])
    $(id).addEventListener('change', apply);
  $('pedalLearn').addEventListener('click', () => {
    if (!pedalLearning) {
      pedalLearning = { min: 127, max: 0 };
      $('pedalLearn').textContent = 'Stop learning';
      return;
    }
    if (pedalLearning.max > pedalLearning.min) {
      $('pedalMin').value = pedalLearning.min;
      $('pedalMax').value = pedalLearning.max;
    }
    pedalLearning = null;
    $('pedalLearn').textContent = 'Learn range';
    apply();
  });
  for (const id of ['volume', 'roomOn', 'roomLevel', 'sympathetic', 'unison', 'absorption', 'a4', 'curveMax'])
    $(id).addEventListener('input', apply);
  $('options').addEventListener('change', apply);
  $('octave').textContent = octave;
  $('velocityOut').textContent = velocity;
}

// ---- The profile ----
function describeProfile() {
  $('profileLine').textContent = profile
    ? `${profile.name}, ${(profile.size / 1e6).toFixed(1)} MB` + (profile.notes ? ` (${profile.notes} notes x ${profile.layers} layers)` : '')
    : 'No profile: load a published piano or choose modal-profile.bin';
  $('profileCredit').textContent = profile?.credit || ''; // a published profile's credit (CC-BY: shown where it plays)
  $('forget').disabled = !profile;
}

/// A profile to play from now on: kept in this browser (IndexedDB) with its name and credit, sent to the engine.
async function useProfile(name, bytes, credit = '') {
  profile = { name, size: bytes.byteLength, bytes, credit };
  describeProfile();
  sendProfile(); // the engine first; keeping it in the browser takes a while for tens of MB
  task('save', `Keeping ${name} in this browser (${MB(bytes.byteLength)} MB)...`);
  try { await profileDb.put({ name, bytes, credit }); } catch { message('Kept for this session only (the browser refused to store it).'); }
  taskDone('save');
}

function sendProfile() {
  if (!node || !profile) return;
  task('engine', `Loading ${profile.name} into the engine (the sound pauses a moment)...`);
  const copy = profile.bytes.slice(0); // the page keeps its own
  send({ type: 'profile', bytes: copy }, [copy]);
}

$('profileFile').addEventListener('change', async (event) => {
  const file = event.target.files?.[0];
  if (!file) return;
  await useProfile(file.name, await file.arrayBuffer());
});

// ---- Published pianos: the profiles attached to PianoForge's releases ----
// A page cannot download a release asset from GitHub (no CORS header on it): the deployment
// (.github/workflows/pages.yml) copies each release's *.modal-profile.bin next to the page and lists them in
// profiles.json, each with its credit, size and SHA-256, checked here before the engine gets it.
let published = [];

function showPublishedCredit() {
  const entry = published[Number($('publishedList').value)];
  $('publishedCredit').textContent = entry ? entry.credit : '';
}

async function listPublished() {
  try {
    const response = await fetch('profiles.json', { cache: 'no-cache' });
    if (!response.ok) return;
    published = await response.json();
  } catch { return; } // a local server without it: only one's own file
  if (!Array.isArray(published) || !published.length) return;
  $('publishedList').replaceChildren(...published.map((entry, i) =>
    new Option(`${entry.name} (${entry.piano}), ${(entry.size / 1e6).toFixed(0)} MB, ${entry.release}`, String(i))));
  $('published').hidden = false;
  showPublishedCredit();
}

/// Downloads `url` (`size` bytes, when known), its progress in the header.
async function download(url, size, label) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const total = size || Number(response.headers.get('Content-Length')) || 0;
  if (!response.body || !total) {
    task('download', `Downloading ${label}...`);
    return response.arrayBuffer();
  }
  let bytes = new Uint8Array(total), at = 0;
  const reader = response.body.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (at + value.length > bytes.length) { // longer than said (a compressed length): room for the rest
      const larger = new Uint8Array(Math.max(bytes.length * 2, at + value.length));
      larger.set(bytes.subarray(0, at));
      bytes = larger;
    }
    bytes.set(value, at);
    at += value.length;
    task('download', `Downloading ${label}: ${Math.round(100 * at / total)} % (${MB(at)} of ${MB(total)} MB)`, at / total);
  }
  return at === bytes.length ? bytes.buffer : bytes.buffer.slice(0, at);
}

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

$('publishedList').addEventListener('change', showPublishedCredit);
$('loadPublished').addEventListener('click', async () => {
  const entry = published[Number($('publishedList').value)];
  if (!entry) return;
  $('loadPublished').disabled = true;
  try {
    message('');
    const bytes = await download(entry.file, entry.size, entry.name);
    task('download', `Checking ${entry.name} (SHA-256)...`);
    if (entry.sha256 && (await sha256(bytes)) !== entry.sha256) throw new Error('its SHA-256 is not the one published');
    taskDone('download');
    if (!node) message(`${entry.name}: ready, it plays once the engine is started (Start).`, 'good');
    await useProfile(entry.name, bytes, entry.credit);
  } catch (error) {
    taskDone('download');
    message(`Cannot load ${entry.name}: ${error.message}`, 'bad');
  } finally {
    $('loadPublished').disabled = false;
  }
});

$('forget').addEventListener('click', async () => {
  await profileDb.remove();
  profile = null;
  describeProfile();
  message('The profile is no longer kept in this browser.', 'good');
});

// ---- Start: the audio context, the worklet, the module ----
$('start').addEventListener('click', async () => {
  if (context) { // a second click: stop
    send({ type: 'allSoundOff' });
    await context.close();
    context = node = null;
    taskDone('engine');
    $('start').textContent = 'Start';
    $('engineLine').textContent = 'Stopped';
    return;
  }
  $('start').disabled = true;
  $('engineLine').textContent = 'Starting...';
  task('start', 'Starting the audio...');
  try {
    // At the profile's rate (44.1 kHz): the attack residual plays as recorded, and the engine does not work for a
    // device set to 96 or 192 kHz (the browser converts to the device's rate). Otherwise, the browser's own rate.
    try {
      context = new AudioContext({ latencyHint: 'interactive', sampleRate: 44100 });
    } catch {
      context = new AudioContext({ latencyHint: 'interactive' });
    }
    await context.audioWorklet.addModule('worklet.js');
    task('start', 'Starting the engine (WebAssembly)...');
    const wasm = await (await fetch('pianoforge.wasm')).arrayBuffer();
    node = new AudioWorkletNode(context, 'pianoforge', {
      numberOfInputs: 0,
      outputChannelCount: [2],
      processorOptions: { wasm },
    });
    node.connect(context.destination);
    node.port.onmessage = ({ data }) => received(data);
    $('start').textContent = 'Stop';
    connectMidi();
  } catch (error) {
    taskDone('start');
    $('engineLine').textContent = 'Stopped';
    message('Cannot start the audio: ' + error.message, 'bad');
    context = node = null;
  } finally {
    $('start').disabled = false;
  }
});

function received(data) {
  switch (data.type) {
    case 'ready': {
      taskDone('start');
      $('engineLine').textContent = 'Modal engine running';
      const latency = (context.baseLatency || 0) + (context.outputLatency || 0);
      $('audioLine').textContent = `${(data.sampleRate / 1000).toFixed(1)} kHz, blocks of 128 (the browser converts to the device's rate)`;
      $('latencyLine').textContent = latency > 0 ? `Output latency about ${(latency * 1000).toFixed(0)} ms (browser and system)` : 'Output latency: not reported by this browser';
      apply();
      send({ type: 'midiSpeed', factor: speedFactor() });
      send({ type: 'midiLoop', on: $('midiLoop').checked });
      if (midiFile) send({ type: 'midiFile', bytes: midiFile.bytes.slice(0) }); // opened before the start
      if (profile) sendProfile();
      else message('Choose modal-profile.bin to hear the piano.');
      break;
    }
    case 'profile':
      taskDone('engine');
      if (data.ok) {
        profile.notes = data.notes;
        profile.layers = data.layers;
        describeProfile();
        message(`Ready (the piano loaded in ${(data.ms / 1000).toFixed(1)} s): play the keyboard, the mouse, the computer keyboard or a MIDI keyboard.`, 'good');
      } else {
        message('The profile did not load: ' + data.error, 'bad');
      }
      break;
    case 'midiFile':
      if (data.ok) {
        Object.assign(midiFile, data);
        filePosition = 0;
        filePlaying = false;
        $('midiName').textContent = `${midiFile.name}   (${data.notes} notes, ${data.tracks} ${data.tracks === 1 ? 'track' : 'tracks'})`;
        transposition = 0; // a new file starts untransposed
        showTranspose();
        showParts();
        $('midiName').classList.remove('dim');
        message(`Opened ${midiFile.name}: Ctrl+Space plays it, with its own velocities.`, 'good');
      } else {
        midiFile = null;
        message('Cannot open the MIDI file: ' + data.error, 'bad');
      }
      showPlayer();
      break;
    case 'info':
      $('voicesLine').textContent = `${data.voices} ${data.voices === 1 ? 'voice' : 'voices'} of 64, ${data.resonators.toLocaleString('en')} resonators`;
      if (data.lastNote >= 0)
        $('noteLine').textContent = `Last note ${data.lastNote}, velocity ${data.lastVelocity} (layer ${data.lastLayer})`;
      const depth = data.pedalValue / 127; // the pedal's travel, as the engine receives it
      $('pedal').classList.toggle('on', depth >= 1);
      $('pedal').classList.toggle('partial', depth > 0 && depth < 1);
      $('pedal').style.setProperty('--depth', `${Math.round(depth * 100)}%`);
      $('pedal').textContent = depth > 0 && depth < 1 ? `Pedal ${Math.round(depth * 100)} %` : 'Pedal';
      $('sostenuto').classList.toggle('on', data.sostenuto);
      $('soft').classList.toggle('on', data.soft);
      $('meterFill').style.width = `${Math.min(100, Math.max(0, (20 * Math.log10(data.peak + 1e-9) + 60) / 60 * 100))}%`;
      filePosition = data.file.position;
      filePlaying = data.file.playing;
      showFileKeys(data.file.keys);
      showPlayer();
      break;
    case 'error':
      taskDone('start');
      taskDone('engine');
      message('The engine did not start: ' + data.message, 'bad');
      break;
    default:
      break;
  }
}

// ---- Page start ----
buildKeyboard();
buildSettings();
buildPlayer();
listPublished();
loadLibrary();
task('stored', 'Reading the piano kept in this browser...');
profileDb.get().then((entry) => {
  taskDone('stored');
  if (entry && !profile) { // (unless one was loaded meanwhile)
    profile = { name: entry.name, size: entry.bytes.byteLength, bytes: entry.bytes, credit: entry.credit || '' };
    sendProfile(); // when the engine was started meanwhile
  }
  describeProfile();
});
