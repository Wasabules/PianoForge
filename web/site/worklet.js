// The engine runs here, on the browser's audio thread, away from the page: the interface cannot make it stutter.
// The module (pianoforge.wasm, built from web/src/exports.cpp) comes in the processor's options; the profile
// arrives later as bytes, from the page (which keeps it in the browser: it is never uploaded).
const INFO_HZ = 20;

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

  loadProfile(bytes) {
    const { pf } = this;
    const data = new Uint8Array(bytes);
    const pointer = pf.pf_alloc(data.length);
    if (pointer === 0) return { ok: false, error: 'out of memory for the profile' };
    new Uint8Array(pf.memory.buffer, pointer, data.length).set(data);
    const status = pf.pf_load_profile(pointer, data.length);
    pf.pf_free(pointer);
    return status === 0
      ? { ok: true, notes: pf.pf_profile_notes(), layers: pf.pf_profile_layers() }
      : { ok: false, error: this.text(pf.pf_error()) };
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
      case 'allSoundOff': pf.pf_all_sound_off(); break;
      case 'volume': pf.pf_set_volume_db(c.db); break;
      case 'option': pf.pf_set_option(c.option, c.on ? 1 : 0); break;
      case 'sympathetic': pf.pf_set_sympathetic_db(c.db); break;
      case 'tuning': pf.pf_set_tuning(c.a4); break;
      case 'room': pf.pf_set_room(c.on ? 1 : 0, c.levelDb, c.size, c.decay, c.brightness, c.predelayMs, c.width); break;
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
        sostenuto: pf.pf_sostenuto() !== 0,
        lastNote: pf.pf_last_note(),
        lastVelocity: pf.pf_last_velocity(),
        lastLayer: pf.pf_last_layer(),
        peak: this.peak,
      });
      this.peak = 0;
    }
    return true;
  }
}

registerProcessor('pianoforge', PianoForgeProcessor);
