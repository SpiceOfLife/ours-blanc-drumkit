// ours-blanc Drum Kit — AudioWorklet running the C engine (drumkit.wasm).
// Main thread → {fn, args} calls a whitelisted export; {load: Uint8Array} restores a pattern.
// Worklet → {snap, pos} about 40 times a second, {save: Uint8Array} after each change.
const CALLS = new Set(["hit", "audition", "toggle_play", "play", "stop", "toggle_record", "clear", "set_bpm",
  "set_metronome", "set_quantize", "set_volume", "toggle_step"]);

class DrumKit extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const inst = new WebAssembly.Instance(new WebAssembly.Module(options.processorOptions.wasm), {});
    this.x = inst.exports;
    if (this.x._initialize) this.x._initialize();
    this.x.init(sampleRate);
    this.blocks = 0;
    this.rev = -1;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.load) {
        const n = Math.min(m.load.length, 11 + 4096 * 4);
        new Uint8Array(this.x.memory.buffer, this.x.state_ptr(), n).set(m.load.subarray(0, n));
        this.x.load(n);
      } else if (CALLS.has(m.fn)) this.x[m.fn](...(m.args || []));
    };
  }
  process(_inputs, outputs) {
    const out = outputs[0], x = this.x;
    x.render(out[0].length);
    const buf = new Float32Array(x.memory.buffer, x.out_ptr(), out[0].length);
    for (const ch of out) ch.set(buf);
    if (++this.blocks % 8 === 0) {
      x.snapshot();
      const snap = new Uint32Array(x.memory.buffer, x.snap_ptr(), 28).slice();
      this.port.postMessage({ snap, pos: x.position() });
      if (snap[26] !== this.rev) {
        const first = this.rev === -1;
        this.rev = snap[26];
        if (!first) { const n = x.save(); this.port.postMessage({ save: new Uint8Array(x.memory.buffer, x.state_ptr(), n).slice() }); }
      }
    }
    return true;
  }
}
registerProcessor("drumkit", DrumKit);
