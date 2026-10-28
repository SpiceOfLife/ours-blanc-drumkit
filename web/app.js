// ours-blanc Drum Kit — web UI. The sound and the loop live in worklet.js (C engine compiled to WebAssembly).
(() => {
  "use strict";
  const NAMES = ["Kick", "Snare", "Closed hat", "Open hat", "High tom", "Mid tom", "Low tom", "Clap", "Crash", "Ride"];
  const SHORT = ["KICK", "SNARE", "CL HAT", "OP HAT", "TOM 1", "TOM 2", "TOM 3", "CLAP", "CRASH", "RIDE"];
  const COLORS = ["#D2372B", "#F2C230", "#3DBB80", "#2DB7BF", "#9877D8", "#7381F0", "#5B86E0", "#F58C3A", "#E65B9F", "#94B83A"];
  // pad order on screen (5 × 2) and computer keys for each instrument
  const ORDER = [0, 1, 7, 2, 3, 4, 5, 6, 8, 9];
  const KEYS = { 0: ["a", "z"], 1: ["s", "x"], 7: ["d", "c"], 2: ["f", "v"], 3: ["g", "b"], 4: ["h", "n"], 5: ["j", "m"], 6: ["k", ","], 8: ["l", "."], 9: [";", "/"] };
  const KEY_TO_INST = {};
  for (const [i, ks] of Object.entries(KEYS)) for (const k of ks) KEY_TO_INST[k] = +i;
  // gamepad (standard mapping) laid out like the R36S version: face buttons by position, D-pad toms
  const PAD_BTN = { 0: 0, 1: 1, 2: 2, 3: 3, 12: 4, 14: 5, 13: 6, 15: 7, 6: 8, 10: 9 };
  const STEPS = 32, LOOP = 768, HOLD_MS = 600, STORE = "obdk-pattern-v2";

  const $ = (s, r = document) => r.querySelector(s);
  const ui = {
    pads: $("#pads"), grid: $("#grid"), play: $("#play"), rec: $("#rec"), clear: $("#clear"),
    bpm: $("#bpm"), bpmDown: $("#bpm-down"), bpmUp: $("#bpm-up"), click: $("#click"), quant: $("#quant"),
    vol: $("#vol"), status: $("#status"), meter: $("#meter"), midi: $("#midi"), midiState: $("#midi-state"), pos: $("#pos"),
  };

  /* ---------------------------------------------------------------- audio */
  let ctx = null, node = null, ready = null, snap = null, pos = 0;
  const queue = [];
  function call(fn, ...args) {
    if (node) node.port.postMessage({ fn, args });
    else { queue.push({ fn, args }); start(); }
  }
  function start() {
    if (ready) return ready;
    ready = (async () => {
      ctx = new AudioContext({ latencyHint: "interactive" });
      ctx.resume(); // still inside the tap that started us (needed on iOS)
      const [wasm] = await Promise.all([fetch("drumkit.wasm").then((r) => r.arrayBuffer()), ctx.audioWorklet.addModule("worklet.js")]);
      node = new AudioWorkletNode(ctx, "drumkit", { numberOfInputs: 0, outputChannelCount: [2], processorOptions: { wasm } });
      node.connect(ctx.destination);
      const saved = loadSaved();
      if (saved) node.port.postMessage({ load: saved });
      node.port.postMessage({ fn: "set_volume", args: [+ui.vol.value] });
      node.port.onmessage = (e) => {
        if (e.data.snap) { snap = e.data.snap; pos = e.data.pos; }
        if (e.data.save) store(e.data.save);
      };
      for (const m of queue.splice(0)) node.port.postMessage(m);
      if (ctx.state !== "running") await ctx.resume();
      document.body.classList.add("live");
    })().catch((err) => {
      ui.status.textContent = "音が出せませんでした。ブラウザーを最新にして、もう一度読み込んでください（" + err.message + "）";
      ready = null;
    });
    return ready;
  }
  // first touch anywhere wakes the audio engine
  addEventListener("pointerdown", () => start(), { once: true, capture: true });
  addEventListener("keydown", () => start(), { once: true, capture: true });

  /* ---------------------------------------------------------------- storage (per browser) */
  function store(bytes) {
    try { localStorage.setItem(STORE, btoa(String.fromCharCode(...bytes))); } catch (e) { /* private mode: no saving */ }
  }
  function loadSaved() {
    try {
      const s = localStorage.getItem(STORE);
      return s ? Uint8Array.from(atob(s), (c) => c.charCodeAt(0)) : null;
    } catch (e) { return null; }
  }

  /* ---------------------------------------------------------------- build pads and grid */
  const padEls = [];
  for (const i of ORDER) {
    const b = document.createElement("button");
    b.className = "pad"; b.type = "button";
    b.style.setProperty("--c", COLORS[i]);
    b.setAttribute("aria-label", NAMES[i]);
    b.innerHTML = `<span class="pad-name">${SHORT[i]}</span><kbd>${KEYS[i][0].toUpperCase()}</kbd>`;
    b.addEventListener("pointerdown", (e) => { e.preventDefault(); hit(i, 112); });
    b.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); hit(i, 112); } });
    ui.pads.append(b);
    padEls[i] = b;
  }
  const cells = [];
  for (let r = 0; r < 10; r++) {
    const i = ORDER[r];
    const label = document.createElement("div");
    label.className = "row-name"; label.textContent = SHORT[i]; label.style.setProperty("--c", COLORS[i]);
    ui.grid.append(label);
    cells[i] = [];
    for (let s = 0; s < STEPS; s++) {
      const c = document.createElement("button");
      c.type = "button"; c.className = "cell"; c.dataset.i = i; c.dataset.s = s;
      c.style.setProperty("--c", COLORS[i]);
      if (s % 4 === 0) c.classList.add("beat");
      if (s === 16) c.classList.add("bar");
      c.setAttribute("aria-label", `${NAMES[i]} ${Math.floor(s / 16) + 1}小節 ${Math.floor((s % 16) / 4) + 1}拍 ${s % 4 + 1}`);
      c.setAttribute("aria-pressed", "false");
      ui.grid.append(c);
      cells[i][s] = c;
    }
  }
  ui.grid.addEventListener("click", (e) => {
    const c = e.target.closest(".cell"); if (!c) return;
    call("toggle_step", +c.dataset.i, +c.dataset.s);
    c.classList.toggle("on"); // instant feedback; the next snapshot confirms it
  });

  function hit(i, vel) {
    call("hit", i, vel);
    flash(i);
  }
  function flash(i) {
    const el = padEls[i]; if (!el) return;
    el.classList.remove("hit"); void el.offsetWidth; el.classList.add("hit");
  }

  /* ---------------------------------------------------------------- transport controls */
  ui.play.addEventListener("click", () => call("toggle_play"));
  ui.rec.addEventListener("click", () => call("toggle_record"));
  ui.click.addEventListener("click", () => call("set_metronome", snap && snap[23] ? 0 : 1));
  ui.quant.addEventListener("click", () => call("set_quantize", snap && snap[24] ? 0 : 1));
  const setBpm = (b) => call("set_bpm", Math.max(40, Math.min(240, b | 0)));
  ui.bpmDown.addEventListener("click", () => setBpm((snap ? snap[22] : 100) - 1));
  ui.bpmUp.addEventListener("click", () => setBpm((snap ? snap[22] : 100) + 1));
  ui.bpm.addEventListener("change", () => setBpm(+ui.bpm.value));
  ui.vol.addEventListener("input", () => call("set_volume", +ui.vol.value));

  // clear needs a 0.6 s hold, like holding R1 on the handheld
  let clearTimer = 0;
  const holdStart = () => {
    ui.clear.classList.add("holding");
    clearTimer = setTimeout(() => { call("clear"); ui.clear.classList.remove("holding"); ui.clear.classList.add("done"); setTimeout(() => ui.clear.classList.remove("done"), 500); }, HOLD_MS);
  };
  const holdEnd = () => { clearTimeout(clearTimer); ui.clear.classList.remove("holding"); };
  ui.clear.addEventListener("pointerdown", holdStart);
  ["pointerup", "pointerleave", "pointercancel"].forEach((t) => ui.clear.addEventListener(t, holdEnd));
  ui.clear.addEventListener("keydown", (e) => { if ((e.key === "Enter" || e.key === " ") && !e.repeat) { e.preventDefault(); holdStart(); } });
  ui.clear.addEventListener("keyup", (e) => { if (e.key === "Enter" || e.key === " ") holdEnd(); });

  /* ---------------------------------------------------------------- keyboard */
  let bkTimer = 0;
  addEventListener("keydown", (e) => {
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.target.closest("input, select, textarea")) return;
    const k = e.key.toLowerCase();
    if (k in KEY_TO_INST) { if (!e.repeat) hit(KEY_TO_INST[k], e.shiftKey ? 127 : 105); e.preventDefault(); return; }
    if (e.target.closest("button") && (k === " " || k === "enter")) return; // let the focused button act
    if (e.repeat) return;
    if (k === " ") { e.preventDefault(); call("toggle_play"); }
    else if (k === "r") call("toggle_record");
    else if (k === "q") ui.quant.click();
    else if (k === "m") ui.click.click();
    else if (k === "arrowup" && e.shiftKey) setBpm((snap ? snap[22] : 100) + 1);
    else if (k === "arrowdown" && e.shiftKey) setBpm((snap ? snap[22] : 100) - 1);
    else if (k === "backspace" || k === "delete") { e.preventDefault(); bkTimer = setTimeout(() => call("clear"), HOLD_MS); }
  });
  addEventListener("keyup", (e) => { if (e.key === "Backspace" || e.key === "Delete") clearTimeout(bkTimer); });

  /* ---------------------------------------------------------------- gamepad */
  const prevPad = [];
  let r1Down = 0, r1Done = false;
  function pollGamepads() {
    for (const gp of navigator.getGamepads ? navigator.getGamepads() : []) {
      if (!gp) continue;
      const prev = prevPad[gp.index] || [];
      const now = gp.buttons.map((b) => b.pressed);
      const pressed = (n) => now[n] && !prev[n];
      for (const [b, i] of Object.entries(PAD_BTN)) if (pressed(+b)) { start(); hit(i, Math.round(80 + 47 * (gp.buttons[b].value || 1))); }
      if (pressed(4)) call("toggle_play");
      if (pressed(5)) { r1Down = performance.now(); r1Done = false; }
      if (now[5] && !r1Done && performance.now() - r1Down > HOLD_MS) { call("clear"); r1Done = true; }
      if (!now[5] && prev[5] && !r1Done) call("toggle_record");
      if (pressed(7)) $("#sequencer").scrollIntoView({ behavior: "smooth", block: "center" });
      if (pressed(9)) ui.click.click();
      prevPad[gp.index] = now;
    }
  }

  /* ---------------------------------------------------------------- MIDI */
  ui.midi.addEventListener("click", async () => {
    if (!navigator.requestMIDIAccess) { ui.midiState.textContent = "このブラウザーは MIDI に対応していません（Chrome・Edge で使えます）"; return; }
    try {
      const access = await navigator.requestMIDIAccess();
      start();
      const GM = { 35: 0, 36: 0, 37: 1, 38: 1, 40: 1, 42: 2, 44: 2, 46: 3, 48: 4, 50: 4, 45: 5, 47: 5, 41: 6, 43: 6, 39: 7, 49: 8, 52: 8, 55: 8, 57: 8, 51: 9, 53: 9, 59: 9 };
      const bind = () => {
        let n = 0;
        for (const input of access.inputs.values()) {
          n++;
          input.onmidimessage = (m) => {
            const [st, note, vel] = m.data;
            if ((st & 0xf0) === 0x90 && vel > 0 && note in GM) hit(GM[note], vel);
          };
        }
        ui.midiState.textContent = n ? `${n} 台の MIDI 機器を受信中（General MIDI のドラム配置）` : "MIDI 機器が見つかりません。つないでからもう一度押してください";
      };
      bind(); access.onstatechange = bind;
    } catch (e) { ui.midiState.textContent = "MIDI の使用が許可されませんでした"; }
  });

  /* ---------------------------------------------------------------- draw */
  const seen = new Array(10).fill(0);
  let lastStep = -1;
  function frame() {
    pollGamepads();
    if (snap) {
      const playing = snap[20], recording = snap[21];
      ui.play.setAttribute("aria-pressed", playing ? "true" : "false");
      ui.play.querySelector("span").textContent = playing ? "停止" : "再生";
      ui.rec.setAttribute("aria-pressed", recording ? "true" : "false");
      ui.click.setAttribute("aria-pressed", snap[23] ? "true" : "false");
      ui.quant.setAttribute("aria-pressed", snap[24] ? "true" : "false");
      if (document.activeElement !== ui.bpm) ui.bpm.value = snap[22];
      for (let i = 0; i < 10; i++) {
        if (snap[10 + i] !== seen[i]) { if (seen[i]) flash(i); seen[i] = snap[10 + i]; }
        const m = snap[i];
        for (let s = 0; s < STEPS; s++) {
          const on = (m >>> s) & 1, c = cells[i][s];
          if (c.classList.contains("on") !== !!on) { c.classList.toggle("on", !!on); c.setAttribute("aria-pressed", on ? "true" : "false"); }
        }
      }
      // playhead
      const step = playing ? Math.floor(pos / 24) % STEPS : -1;
      if (step !== lastStep) {
        if (lastStep >= 0) for (let i = 0; i < 10; i++) cells[i][lastStep].classList.remove("now");
        if (step >= 0) for (let i = 0; i < 10; i++) cells[i][step].classList.add("now");
        lastStep = step;
      }
      ui.pos.style.transform = `scaleX(${playing ? pos / LOOP : 0})`;
      ui.pos.classList.toggle("rec", !!recording);
      const n = snap[25];
      ui.status.textContent = recording ? `録音中 — 叩いた音がループに重なります（${n} 打）` : playing ? `再生中（${n} 打）` : n ? `ループに ${n} 打。再生で鳴らします` : "パッドを叩くと音が出ます。録音してループを作れます";
      ui.meter.style.transform = `scaleY(${Math.min(1, snap[27] / 1e6)})`;
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
})();
