// The engine runs here, on the browser's audio thread, away from the page: the interface cannot make it stutter.
// The module (pianoforge.wasm, built from web/src/exports.cpp) comes in the processor's options; the profile
// arrives later as bytes, from the page (which keeps it in the browser: it is never uploaded); so does a MIDI file,
// which plays here to the sample (the lab's sequence player).
const INFO_HZ = 30;

class PianoForgeProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    try {
      const module = new WebAssembly.Module(options.processorOptions.wasm);
      // The only import: the memory grew (the views below are made again whenever the buffer changes).
      this.pf = new WebAssembly.Instance(module, { env: { emscripten_notify_memory_growth() {} } }).exports;
      this.pf._initialize?.();
      this.pf.pf_init(sampleRate);
    } catch (error) {
      this.port.postMessage({ type: 'error', message: String(error) });
      return;
    }
    this.untilInfo = 0;
    this.peak = 0;
    this.port.onmessage = ({ data }) => this.command(data);
    this.port.postMessage({ type: 'ready', sampleRate });
  }

  /** A C string of the module's memory (ASCII). */
  text(pointer) {
    const bytes = new Uint8Array(this.pf.memory.buffer, pointer);
    let s = '';
    for (let i = 0; bytes[i] !== 0 && i < 1000; i++) s += String.fromCharCode(bytes[i]);
    return s;
  }

  /** Copies bytes into the module's memory, runs `use(pointer, length)`, frees them. */
  withBytes(bytes, use) {
    const { pf } = this;
    const data = new Uint8Array(bytes);
    const pointer = pf.pf_alloc(data.length);
    if (pointer === 0) return -1;
    new Uint8Array(pf.memory.buffer, pointer, data.length).set(data);
    const status = use(pointer, data.length);
    pf.pf_free(pointer);
    return status;
  }

  loadMidi(bytes) {
    const { pf } = this;
    const status = this.withBytes(bytes, (pointer, length) => pf.pf_midi_load(pointer, length));
    return status === 0
      ? { ok: true, notes: pf.pf_midi_notes(), tracks: pf.pf_midi_tracks(), length: pf.pf_midi_length(), parts: this.parts() }
      : { ok: false, error: status < 0 ? 'out of memory' : this.text(pf.pf_error()) };
  }

  /** The parts of the MIDI file (its instruments) and whether each plays. */
  parts() {
    const { pf } = this;
    const list = [];
    for (let i = 0; i < pf.pf_midi_parts(); i++) {
      list.push({ name: this.text(pf.pf_midi_part_name(i)), instrument: this.text(pf.pf_midi_part_instrument(i)),
        track: pf.pf_midi_part_track(i), channel: pf.pf_midi_part_channel(i), notes: pf.pf_midi_part_notes(i),
        enabled: pf.pf_midi_part_enabled(i) !== 0 });
    }
    return list;
  }

  loadProfile(bytes) {
    const { pf } = this;
    const status = this.withBytes(bytes, (pointer, length) => pf.pf_load_profile(pointer, length));
    return status === 0
      ? { ok: true, notes: pf.pf_profile_notes(), layers: pf.pf_profile_layers() }
      : { ok: false, error: status < 0 ? 'out of memory for the profile' : this.text(pf.pf_error()) };
  }

  /** The keys the MIDI file has sounding (for the keyboard's display). */
  fileKeys() {
    const keys = [];
    for (let key = 21; key <= 108; key++) if (this.pf.pf_midi_sounding(key) > 0) keys.push(key);
    return keys;
  }

  command(c) {
    const { pf } = this;
    switch (c.type) {
      case 'profile': {
        const result = this.loadProfile(c.bytes);
        this.port.postMessage({ type: 'profile', ...result });
        break;
      }
      case 'midi': pf.pf_event(c.status, c.data1, c.data2, c.curved ? 1 : 0); break;
      case 'midiFile': this.port.postMessage({ type: 'midiFile', ...this.loadMidi(c.bytes) }); break;
      case 'midiClose': pf.pf_midi_close(); break;
      case 'midiPlay': pf.pf_midi_play(); break;
      case 'midiPause': pf.pf_midi_pause(); break;
      case 'midiStop': pf.pf_midi_stop(); break;
      case 'midiSeek': pf.pf_midi_seek(c.seconds); break;
      case 'midiSpeed': pf.pf_midi_speed(c.factor); break;
      case 'midiLoop': pf.pf_midi_loop(c.on ? 1 : 0); break;
      case 'midiPart': pf.pf_midi_set_part(c.part, c.on ? 1 : 0); break;
      case 'midiTranspose': pf.pf_midi_transpose(c.semitones); break;
      case 'allSoundOff': pf.pf_all_sound_off(); break;
      case 'volume': pf.pf_set_volume_db(c.db); break;
      case 'option': pf.pf_set_option(c.option, c.on ? 1 : 0); break;
      case 'sympathetic': pf.pf_set_sympathetic_db(c.db); break;
      case 'unison': pf.pf_set_unison(c.spread); break;
      case 'tuning': pf.pf_set_tuning(c.a4); break;
      case 'room': pf.pf_set_room(c.on ? 1 : 0, c.levelDb, c.size, c.decay, c.brightness, c.distance, c.keepLoudness ? 1 : 0, c.width,
                                  c.headphones ? 1 : 0); break;
      case 'curve': pf.pf_set_velocity_curve(c.inputMin, c.inputMax, ...c.points); break;
      default: break;
    }
  }

  process(_inputs, outputs) {
    const { pf } = this;
    if (!pf) return false;
    const output = outputs[0];
    const frames = output[0].length;
    pf.pf_render(frames);
    if (this.buffer !== pf.memory.buffer || this.frames !== frames) {
      this.buffer = pf.memory.buffer;
      this.frames = frames;
      this.left = new Float32Array(this.buffer, pf.pf_left(), frames);
      this.right = new Float32Array(this.buffer, pf.pf_right(), frames);
    }
    output[0].set(this.left);
    if (output[1]) output[1].set(this.right);
    for (let i = 0; i < frames; i++) {
      const level = Math.max(Math.abs(this.left[i]), Math.abs(this.right[i]));
      if (level > this.peak) this.peak = level;
    }
    this.untilInfo -= frames;
    if (this.untilInfo <= 0) {
      this.untilInfo = sampleRate / INFO_HZ;
      this.port.postMessage({
        type: 'info',
        voices: pf.pf_voices(),
        resonators: pf.pf_resonators(),
        sustain: pf.pf_sustain() !== 0,
        pedalValue: pf.pf_pedal_value(),
        sostenuto: pf.pf_sostenuto() !== 0,
        soft: pf.pf_soft() !== 0,
        lastNote: pf.pf_last_note(),
        lastVelocity: pf.pf_last_velocity(),
        lastLayer: pf.pf_last_layer(),
        peak: this.peak,
        file: { position: pf.pf_midi_position(), playing: pf.pf_midi_playing() !== 0, keys: this.fileKeys() },
      });
      this.peak = 0;
    }
    return true;
  }
}

registerProcessor('pianoforge', PianoForgeProcessor);
