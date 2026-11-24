// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge (formerly Vintage Chonk) ORBIT panel (plugin): direction B "Streamline Lounge" made interactive.
// Layout and drawing follow the design mock-up (1600 x 1000, scaled to the window).
// Bridge: window.__JUCE__.backend.emitEvent('mfm1', {op, a}) with ops "o.*" (plugin/src/PluginEditor.cpp,
// PluginProcessor::orbitOp); the plugin calls window.ORB.state / values / views / meter / toast.
// Fast: everything is built once; values move SVG transforms, element transforms and text only; no
// animated filters.
'use strict';
(function () {
  // ------------------------------------------------------------------------------- constants ---
  const NG = 24, NL = 42, NCORE = NG + 3 * NL;
  const LP = (l, p) => NG + l * NL + p;
  const G = { PUCK_X: 0, PUCK_Y: 1, NODE1_X: 2, DRY_X: 8, SHARP: 10, PATH: 11, RATE: 12, SYNC: 13, SIZE_X: 14, WOBBLE: 16, DIR: 17,
    ROTATE: 18, DRIFT: 19, DRIVE: 20, MASTER: 21 };
  const L = { LEVEL: 0, PAN: 1, BODY: 2, TONE: 3, GRAIN: 4, MOTION: 5, PITCH: 6, TUNE: 7, FINE: 8, A: 9, D: 10, S: 11, R: 12, VEL: 13,
    FTYPE: 14, CUT: 15, RES: 16, FDRIVE: 17, FAMT: 18, FA: 19, FD: 20, FS: 21, FR: 22, MODSRC: 23, SCAN_RATE: 24, SCAN_POS: 25,
    SCAN_LOOP: 26, IR_PITCH: 27, KEYTRACK: 28, START: 29, VEL_START: 30, LOOP: 31, LOOP_START: 32, LOOP_END: 33, XFADE: 34,
    REVERSE: 35, STRETCH: 36, TAPE: 37, WOW: 38, FLUTTER: 39, BUMP: 40, HISS: 41 };
  const SP = { RATE: 1000, SPREAD: 1001, AGE: 1002, FX0: 1003, MAC0: 1009 };
  const DIMS = ['BODY', 'TONE', 'GRAIN', 'MOTION', 'PITCH'];
  const FTYPES = ['OFF', 'LADDER 24', 'SVF LOW', 'SVF BAND', 'SVF HIGH'];
  const FXN = ['OCT FUZZ', 'TAPE', 'ENSEMBLE', 'BBD ECHO', 'OIL CAN', 'TWIN TANK'];
  const ENS = ['', 'I', 'II', 'I + II', 'VIBRATO'];
  const SYNCN = ['FREE', '1/32', '1/16T', '1/16', '1/8T', '1/16.', '1/8', '1/4T', '1/8.', '1/4', '1/4.', '1/2', '1 BAR', '2 BARS'];
  const MSRC = ['NONE', 'MACRO 1', 'MACRO 2', 'MACRO 3', 'MACRO 4', 'MOD WHEEL', 'AFTERTOUCH', 'VELOCITY', 'KEY', 'LFO 1', 'LFO 2',
    'ORBIT X', 'ORBIT Y', 'EXT1 BAR SINE', 'EXT2 STEPPER', 'EXT3 S&H', 'EXT4 BEND', 'EXT5 SUSTAIN', 'EXT6 NOTE ENV', 'EXT7 BEAT', 'EXT8 WALK'];
  const NOTES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  const noteName = (n) => { n = Math.round(n); return n < 0 || n > 127 ? '?' : NOTES[n % 12] + (Math.floor(n / 12) - 1); };
  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const pct = (v) => Math.round(v * 100) + ' %';

  // ---------------------------------------------------------------------------------- bridge ---
  function send(op, a) {
    try { const j = window.__JUCE__; if (j && j.backend) j.backend.emitEvent('mfm1', { op, a: a || [] }); } catch (e) { /* closing */ }
  }

  // ----------------------------------------------------------------------------------- model ---
  let ST = null;            // the last full state from the plugin
  let S = null;             // its settings (edited locally as the panel sends changes)
  let HV = [];              // host parameter values, normalised
  let CORE = [];            // core parameter info [{name,min,max,def,int,log}]
  const hostIdx = new Map(); // target (core id or special) -> host index
  const hostId = new Map();  // host parameter id ('orbit_motion', ...) -> host index (1.2.0 parameters are found by id)
  const hid = (id) => (hostId.has(id) ? hostId.get(id) : -1);
  const hval = (id, d) => { const i = hid(id); return i < 0 || HV[i] == null ? d : HV[i]; };
  function hostSet(id, n) { const i = hid(id); if (i < 0) return false; HV[i] = n; send('o.host', [i, n]); return true; }
  // live input (sidechain) and orbit motion: the host parameters lead (DAW automation), the settings are the fallback
  function inL(i) {
    if (!S) return { src: false, imp: false, gate: 0, window: 0.25 };
    if (!S.input) S.input = { trim: 0, layers: [] };
    if (!S.input.layers) S.input.layers = [];
    while (S.input.layers.length < 3) S.input.layers.push({ src: false, imp: false, gate: 0, window: 0.25 });
    return S.input.layers[i];
  }
  const srcIn = (i) => hval(`orbit_l${i + 1}_src_input`, inL(i).src ? 1 : 0) > 0.5;
  const impIn = (i) => hval(`orbit_l${i + 1}_imp_input`, inL(i).imp ? 1 : 0) > 0.5;
  const gateOf = (i) => (hval(`orbit_l${i + 1}_input_gate`, inL(i).gate ? 1 : 0) > 0.5 ? 1 : 0);
  const shapeOf = () => (S && S.shape >= 1 && S.shape <= 3 ? S.shape : S && S.path ? S.path : 1);
  const motionOn = () => hval('orbit_motion', S && S.path ? 1 : 0) > 0.5;
  const trimDb = () => -24 + 48 * hval('orbit_input_trim', S && S.input ? (S.input.trim + 24) / 48 : 0.5);
  const widgets = [];       // everything with update()
  let scale = 1;

  const isHost = (t) => hostIdx.has(t);
  function hostMapOf(i) { return ST.host[i].map; }
  function coreToNorm(id, v) {
    const c = CORE[id]; if (!c) return 0;
    if (c.log) return clamp(Math.log(Math.max(v, c.min) / c.min) / Math.log(c.max / c.min), 0, 1);
    return clamp((v - c.min) / (c.max - c.min), 0, 1);
  }
  function normToCore(id, n) {
    const c = CORE[id]; if (!c) return 0;
    let v = c.log ? c.min * Math.pow(c.max / c.min, n) : c.min + n * (c.max - c.min);
    if (c.int) v = Math.round(v);
    return v;
  }
  function hostToCore(i, n) {
    const h = ST.host[i], c = CORE[h.target];
    if (h.map === 'sq2') return c.max * n * n;
    if (h.map === 'log') return c.min * Math.pow(c.max / c.min, n);
    return c.min + n * (c.max - c.min);
  }
  // a core parameter's current value (host-automated ones from the host value)
  function coreVal(id) { return isHost(id) ? hostToCore(hostIdx.get(id), HV[hostIdx.get(id)]) : S.core[id]; }
  function markEdited() { if (S && !S.preset.dirty) { S.preset.dirty = true; updateHeader(); } }

  function rateText(n) {
    if (S && S.sync) { const b = 64 >> clamp(Math.round(n * 6), 0, 6); return b >= 4 ? (b / 4) + (b === 4 ? ' BAR' : ' BARS') : b + (b === 1 ? ' BEAT' : ' BEATS'); }
    if (n <= 0.0005) return 'STOP';
    const hz = 0.01 * Math.pow(2000, n);
    return hz < 1 ? (1 / hz).toFixed(1) + ' s' : hz.toFixed(2) + ' Hz';
  }
  function hzText(hz) { return hz >= 1000 ? (hz / 1000).toFixed(2) + ' kHz' : Math.round(hz) + ' Hz'; }
  function dbText(g) { if (g < 1e-4) return '-inf dB'; const d = 20 * Math.log10(g); return (Math.abs(d) < 0.05 ? '0.0' : d.toFixed(1)) + ' dB'; }
  function secText(s) { return s < 1 ? Math.round(s * 1000) + ' ms' : s.toFixed(2) + ' s'; }

  // bindings: {get() -> 0..1, set(n, final), text(), def, target (core id for macro assign, or null)}
  function hostBind(target, textFn, def) {
    return {
      target,
      get() { const i = hostIdx.get(target); return i === undefined ? 0 : HV[i]; },
      set(n) { const i = hostIdx.get(target); if (i === undefined) return; HV[i] = n; send('o.host', [i, n]); markEdited(); },
      text() { return textFn(this.get()); },
      def: def,
    };
  }
  function idBind(id, textFn, def) {
    return {
      target: null,
      get() { return hval(id, def); },
      set(n) { if (hostSet(id, n)) markEdited(); },
      text() { return textFn(this.get()); },
      def,
    };
  }
  function coreBind(id, textFn, opts) {
    opts = opts || {};
    return {
      target: id,
      get() { return coreToNorm(id, S.core[id]); },
      set(n) { const v = normToCore(id, n); if (v === S.core[id]) return; S.core[id] = v; send('o.set', [id, v]); markEdited(); },
      text() { return textFn ? textFn(S.core[id]) : pct(this.get()); },
      def: CORE[id] ? coreToNorm(id, CORE[id].def) : 0,
      step: CORE[id] && CORE[id].int ? 1 / (CORE[id].max - CORE[id].min) : 0,
    };
  }

  // ------------------------------------------------------------------------- drawing helpers ---
  const NS = 'http://www.w3.org/2000/svg';
  function rng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; }; }
  function sv(tag, attrs, parent) {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }
  function H(html, parent) { const t = document.createElement('template'); t.innerHTML = html.trim(); const n = t.content.firstChild; (parent || body).appendChild(n); return n; }
  const px = (x, y, w, h) => `left:${x}px;top:${y}px;` + (w != null ? `width:${w}px;` : '') + (h != null ? `height:${h}px;` : '');

  // walnut, the Chonk cheek recipe (drawn once)
  function wood(canvas, seed) {
    const r = canvas.getBoundingClientRect();
    const w = r.width / scale, h = r.height / scale;
    const horiz = w > h;
    const Lg = Math.round(Math.max(w, h) * 2), Sm = Math.round(Math.min(w, h) * 2);
    if (Lg < 2 || Sm < 2) return;
    const tmp = document.createElement('canvas'); tmp.width = Sm; tmp.height = Lg;
    const g = tmp.getContext('2d'), rnd = rng(seed);
    const bg = g.createLinearGradient(0, 0, Sm, 0);
    bg.addColorStop(0, '#2a160b'); bg.addColorStop(0.3, '#4a2a16'); bg.addColorStop(0.55, '#5b3520'); bg.addColorStop(0.8, '#43261a'); bg.addColorStop(1, '#25130a');
    g.fillStyle = bg; g.fillRect(0, 0, Sm, Lg);
    const lines = Math.round(70 * Math.max(1, Sm / 128));
    for (let k = 0; k < lines; k++) {
      const x0 = rnd() * Sm, amp = 2 + rnd() * 9, f = 0.0015 + rnd() * 0.004, ph = rnd() * 6.28, dark = rnd() < 0.62;
      g.strokeStyle = dark ? `rgba(20,8,3,${0.18 + rnd() * 0.35})` : `rgba(170,110,66,${0.07 + rnd() * 0.14})`;
      g.lineWidth = 0.6 + rnd() * (dark ? 2.6 : 1.6);
      g.beginPath();
      for (let y = 0; y <= Lg; y += 12) {
        const x = x0 + Math.sin(y * f + ph) * amp + Math.sin(y * f * 3.7 + ph * 2) * amp * 0.25;
        if (y === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
    }
    const pores = Math.round(Sm * Lg / 60);
    for (let k = 0; k < pores; k++) { g.fillStyle = `rgba(15,6,2,${0.15 + rnd() * 0.3})`; g.fillRect(rnd() * Sm, rnd() * Lg, 0.8, 2 + rnd() * 5); }
    for (let k = 0; k < Math.max(1, Math.round(Lg / 1200)); k++) {
      const cx = Sm * (0.25 + rnd() * 0.5), cy = 200 + rnd() * Math.max(1, Lg - 400);
      for (let rr = 3; rr < 26; rr += 2.5) { g.strokeStyle = `rgba(25,10,4,${0.35 - rr * 0.01})`; g.lineWidth = 1.2; g.beginPath(); g.ellipse(cx, cy, rr * 0.55, rr * 1.8, 0, 0, 6.283); g.stroke(); }
    }
    const sh = g.createLinearGradient(0, 0, Sm, 0);
    sh.addColorStop(0, 'rgba(255,220,180,0)'); sh.addColorStop(0.42, 'rgba(255,220,180,0.10)'); sh.addColorStop(0.5, 'rgba(255,220,180,0.03)'); sh.addColorStop(1, 'rgba(0,0,0,0.2)');
    g.fillStyle = sh; g.fillRect(0, 0, Sm, Lg);
    if (horiz) {
      canvas.width = Lg; canvas.height = Sm;
      const c = canvas.getContext('2d'); c.translate(Lg, 0); c.rotate(Math.PI / 2); c.drawImage(tmp, 0, 0);
    } else { canvas.width = Sm; canvas.height = Lg; canvas.getContext('2d').drawImage(tmp, 0, 0); }
  }

  // knob drawing (the mock-up's styles; only the rotating group and the arc change afterwards)
  const A0 = -135, A1 = 135;
  function pol(r, deg) { const a = (deg - 90) * Math.PI / 180; return [r * Math.cos(a), r * Math.sin(a)]; }
  function arc(r, d0, d1) { const [x0, y0] = pol(r, d0), [x1, y1] = pol(r, d1); return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${d1 - d0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`; }
  function defs() {
    const s = sv('svg', { id: 'oc-defs', width: 0, height: 0, style: 'position:absolute;width:0;height:0' }, document.body);
    const d = sv('defs', {}, s);
    function lin(id, stops, x2, y2) { const g = sv('linearGradient', { id, x1: 0, y1: 0, x2: x2 == null ? 1 : x2, y2: y2 == null ? 1 : y2 }, d); stops.forEach(([o, c, a]) => sv('stop', { offset: o, 'stop-color': c, 'stop-opacity': a == null ? 1 : a }, g)); }
    function rad(id, stops, cx, cy, r) { const g = sv('radialGradient', { id, cx: cx == null ? 0.5 : cx, cy: cy == null ? 0.5 : cy, r: r || 0.5 }, d); stops.forEach(([o, c, a]) => sv('stop', { offset: o, 'stop-color': c, 'stop-opacity': a == null ? 1 : a }, g)); }
    lin('ocChrome', [[0, '#ffffff'], [0.25, '#c9cbcc'], [0.5, '#7d8183'], [0.62, '#e9ebec'], [1, '#6f7376']]);
    rad('ocCream', [[0, '#fffaf0'], [0.6, '#efe6d2'], [0.9, '#d6cbb2'], [1, '#b8ad95']], 0.42, 0.32, 0.8);
    rad('ocShadow', [[0, '#000', 0.8], [0.78, '#000', 0.6], [0.9, '#000', 0.25], [1, '#000', 0]]);
    lin('ocHi', [[0, '#fff', 0.38], [1, '#fff', 0]], 0, 1);
    rad('ocWalnutCap', [[0, '#7a4a2a'], [0.6, '#4a2a16'], [1, '#26130a']], 0.42, 0.32, 0.8);
    rad('ocHalo', [[0, '#3fb8ac', 0.30], [1, '#3fb8ac', 0]]);
    rad('ocPuckHalo', [[0, '#ffb347', 0.55], [0.45, '#ff8a1a', 0.22], [1, '#ff8a1a', 0]]);
    rad('ocNodeGlow', [[0.55, '#3fb8ac', 0.85], [1, '#3fb8ac', 0]]);
  }
  function drawKnob(svg, size, o) {
    o = o || {};
    const R = size / 2 / (o.ticks ? 1.34 : 1.12), Sz = size / 2;
    svg.setAttribute('viewBox', `${-Sz} ${-Sz} ${2 * Sz} ${2 * Sz}`);
    if (o.ticks) {
      for (let i = 0; i <= 10; i++) {
        const a = A0 + 27 * i, [x0, y0] = pol(R * 1.12, a), [x1, y1] = pol(R * (i % 5 ? 1.2 : 1.27), a);
        sv('line', { x1: x0, y1: y0, x2: x1, y2: y1, stroke: o.tickColor || '#5a4a3a', 'stroke-width': i % 5 ? 0.8 : 1.3, opacity: i % 5 ? 0.6 : 0.95 }, svg);
      }
    }
    let arcEl = null;
    if (o.arc) {
      sv('path', { d: arc(R * 1.16, A0, A1), fill: 'none', stroke: o.arcBg || 'rgba(90,60,30,.15)', 'stroke-width': R * 0.1, 'stroke-linecap': 'round' }, svg);
      arcEl = sv('path', { d: '', fill: 'none', stroke: o.arc, 'stroke-width': R * 0.1, 'stroke-linecap': 'round' }, svg);
    }
    sv('circle', { cx: R * 0.04, cy: R * 0.14, r: R * 1.06, fill: 'url(#ocShadow)', opacity: 0.8 }, svg);
    const rot = sv('g', {}, svg);
    sv('circle', { r: R, fill: 'url(#ocChrome)', stroke: '#6a5a48', 'stroke-width': 0.7 }, rot);
    const n = 40, c = 2 * Math.PI * R * 0.93 / n;
    sv('circle', { r: R * 0.93, fill: 'none', stroke: 'rgba(0,0,0,0.28)', 'stroke-width': R * 0.1, 'stroke-dasharray': `${(c * 0.3).toFixed(2)} ${(c * 0.7).toFixed(2)}` }, rot);
    sv('circle', { r: R * 0.8, fill: 'url(#ocWalnutCap)', stroke: 'rgba(0,0,0,0.5)', 'stroke-width': 0.6 }, rot);
    sv('circle', { r: R * 0.42, fill: 'url(#ocCream)', stroke: 'rgba(0,0,0,0.25)', 'stroke-width': 0.5 }, rot);
    sv('line', { x1: 0, y1: -R * 0.5, x2: 0, y2: -R * 0.9, stroke: o.pointer || '#f3b23a', 'stroke-width': Math.max(1.6, R * 0.11), 'stroke-linecap': 'round' }, rot);
    sv('ellipse', { cx: 0, cy: -R * 0.3, rx: R * 0.55, ry: R * 0.32, fill: 'url(#ocHi)', 'pointer-events': 'none' }, svg);
    return { rot, arcEl, R };
  }

  // ---------------------------------------------------------------------------------- widgets ---
  let assigning = 0;        // a mod source waiting for a click on a control (0 = none)
  function tryAssign(target, e) {
    if (!assigning) return false;
    e.preventDefault(); e.stopPropagation();
    if (target == null) { toast('THIS CONTROL CANNOT BE MODULATED'); return true; }
    send('o.assign', [assigning, target, 0.5]);
    toast(MSRC[assigning] + ' → ' + coreName(target));
    setAssign(0);
    return true;
  }
  function coreName(id) { return CORE[id] ? CORE[id].name : '#' + id; }

  // drag helper: vertical drag on el; fine with shift / ctrl
  function dragValue(el, bind, onChange, opts) {
    opts = opts || {};
    let y0 = 0, v0 = 0, active = false;
    el.addEventListener('pointerdown', (e) => {
      if (tryAssign(bind.target, e)) return;
      if (e.button !== 0) return;
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      active = true; y0 = e.clientY; v0 = bind.get();
      if (opts.jump) { const nv = opts.jump(e); if (nv != null) { v0 = nv; bind.set(nv, false); onChange(); } }
    });
    el.addEventListener('pointermove', (e) => {
      if (!active) return;
      const range = (opts.range || 150) * (e.shiftKey || e.ctrlKey ? 6 : 1);
      let v = clamp(v0 + (y0 - e.clientY) / scale / range, 0, 1);
      if (bind.step) v = Math.round(v / bind.step) * bind.step;
      if (v !== bind.get()) { bind.set(v, false); onChange(); }
    });
    const end = () => { if (active) { active = false; if (bind.commit) bind.commit(); } };
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      const st = bind.step || ((e.shiftKey || e.ctrlKey) ? 0.004 : 0.025);
      const v = clamp(bind.get() + (e.deltaY < 0 ? st : -st), 0, 1);
      bind.set(v, true); onChange(); if (bind.commit) bind.commit();
    }, { passive: false });
    el.addEventListener('dblclick', (e) => { e.preventDefault(); if (bind.def != null) { bind.set(bind.def, true); onChange(); if (bind.commit) bind.commit(); } });
  }

  function Knob(parent, x, y, size, opts, bind, textEl) {
    const svg = sv('svg', { class: 'k', style: px(x, y, size, size), width: size, height: size }, parent);
    const k = drawKnob(svg, size, opts);
    const w = {
      el: svg, bind,
      update() {
        const v = bind.get(), ang = A0 + (A1 - A0) * v;
        k.rot.setAttribute('transform', `rotate(${ang.toFixed(2)})`);
        if (k.arcEl) k.arcEl.setAttribute('d', v > 0.005 ? arc(k.R * 1.16, A0, ang) : '');
        if (textEl) textEl.textContent = bind.text();
      },
    };
    svg.__w = w;
    dragValue(svg, bind, () => w.update(), {});
    widgets.push(w);
    return w;
  }

  function nix(parent, x, y, str, sm) {
    const e = H(`<div class="nix ${sm ? 'sm' : ''}" style="${px(x, y)}"></div>`, parent);
    setNix(e, str);
    return e;
  }
  function setNix(e, str) {
    if (e.__s === str) return;
    e.__s = str;
    while (e.children.length < str.length) e.appendChild(document.createElement('i'));
    while (e.children.length > str.length) e.removeChild(e.lastChild);
    [...str].forEach((c, i) => { const t = e.children[i]; t.className = c === '_' ? 'off' : ''; t.textContent = c === '_' ? '0' : c; });
  }
  const two = (v) => { const n = clamp(Math.round(v), 0, 99); return n < 10 ? '_' + n : String(n); };

  function pill(parent, x, y, list, cls, onPick) {
    const p = H(`<div class="pill ${cls || ''}" style="${px(x, y)}">${list.map((k, i) => `<b data-i="${i}">${k}</b>`).join('')}</div>`, parent);
    p.addEventListener('pointerdown', (e) => { const b = e.target.closest('b'); if (!b) return; e.preventDefault(); onPick(+b.dataset.i); });
    p.setOn = (i) => [...p.children].forEach((b, k) => b.classList.toggle('on', k === i));
    return p;
  }

  function win(parent, x, y, w, h, r) {
    const c = H(`<div class="win" style="${px(x, y, w, h)}border-radius:${r}px"><div class="g"></div></div>`, parent);
    return c;
  }

  // vertical matrix slider with its nixie
  function Slider(parent, cx, bind, label) {
    const th = 84, ty = 70, tw = 12;
    const n = nix(parent, cx - 14, 40, '__', true);
    const slot = H(`<div class="slot" style="${px(cx - 16, ty - 6, 32, th + 12)}">
      <div class="track" style="${px(10, 6, tw, th)}"><div class="fill" style="height:${th - 4}px"></div></div><div class="thumb"></div></div>`, parent);
    const fill = slot.querySelector('.fill'), thumb = slot.querySelector('.thumb');
    H(`<div class="lbl" style="${px(cx - 29, 164, 58)}font-size:9.5px;letter-spacing:1.6px">${label}</div>`, parent);
    const w = {
      el: slot, bind,
      update() {
        const v = bind.get();
        fill.style.transform = `scaleY(${v.toFixed(4)})`;
        thumb.style.transform = `translate(1px, ${(6 + th - 2 - (th - 4) * v - 5).toFixed(1)}px)`;
        thumb.classList.toggle('zero', v < 0.005);
        setNix(n, two(v * 100));
      },
    };
    slot.__w = w;
    dragValue(slot, bind, () => w.update(), {
      range: th,
      jump(e) {
        if (e.target === thumb) return null;
        const r = slot.getBoundingClientRect();
        return clamp((r.bottom / scale - 6 - 2 - e.clientY / scale) / (th - 4), 0, 1);
      },
    });
    widgets.push(w);
    return w;
  }

  function wavePath(peaks, w, h, root) {
    if (!peaks || peaks.length < 4) return `M0 ${h / 2} L${w} ${h / 2}`;
    if (root) peaks = peaks.map((v) => Math.sign(v) * Math.sqrt(Math.abs(v)));
    const cols = peaks.length / 2;
    let d = `M0 ${(h / 2).toFixed(1)} `;
    for (let c = 0; c < cols; c++) d += `L${((c + 0.5) * w / cols).toFixed(1)} ${(h / 2 - clamp(peaks[2 * c + 1], -1, 1) * h * 0.46).toFixed(1)} `;
    d += `L${w} ${(h / 2).toFixed(1)} `;
    for (let c = cols - 1; c >= 0; c--) d += `L${((c + 0.5) * w / cols).toFixed(1)} ${(h / 2 - clamp(peaks[2 * c], -1, 1) * h * 0.46).toFixed(1)} `;
    return d + 'Z';
  }
  function tracePath(vals, w, h) {
    if (!vals || vals.length < 2) return '';
    let d = '';
    vals.forEach((v, i) => { d += (i ? 'L' : 'M') + (i / (vals.length - 1) * w).toFixed(1) + ' ' + (h / 2 - clamp(v, -1, 1) * h * 0.42).toFixed(1) + ' '; });
    return d;
  }

  // ---------------------------------------------------------------------------------- layout ---
  let body = null;
  const layers = [];
  const hdr = {};
  const orb = {};
  const bot = {};

  function build() {
    body = document.getElementById('body');
    defs();
    // walnut header band
    H(`<div class="band" style="${px(0, 0, 1476, 78)}"><canvas class="wood" data-seed="4242"></canvas></div>`);
    H(`<div class="chrome-line" style="${px(0, 78, 1476)}"></div>`);
    H(`<div class="abs" style="${px(22, 15)}font-size:25px;font-weight:300;letter-spacing:7px;color:#f3e9d2;text-shadow:0 1px 2px rgba(0,0,0,.6)">ATOMIC&thinsp;LOUNGE</div>`);
    H(`<div class="abs" style="${px(24, 49)}font-size:9px;font-weight:600;letter-spacing:4px;color:rgba(243,233,210,.7)">POLY-CONVOLUTION · ORBIT MODE</div>`);
    const mode = H(`<div id="modesw" class="abs" style="${px(326, 20, 214, 38)}border-radius:19px;padding:3px;background:linear-gradient(160deg,#fff,#a9aaa9 35%,#f0f0ee 55%,#7f807e 85%,#d9dad8);box-shadow:0 2px 6px rgba(0,0,0,.5)"></div>`);
    H(`<div style="position:relative;height:100%;border-radius:16px;background:linear-gradient(#2a1c12,#140c07);box-shadow:inset 0 2px 5px #000;display:flex">
      <div class="modeb off" data-m="chonk" title="REGOLITH: the wavetable synth">REGOLITH</div><div class="modeb on">ORBIT</div></div>`, mode);
    mode.querySelector('[data-m=chonk]').addEventListener('pointerdown', (e) => { e.preventDefault(); send('o.mode', ['chonk']); });
    const pw = win(body, 556, 16, 470, 46, 23).firstChild;
    hdr.num = nix(pw, 18, 7, '__', true);
    H(`<div class="abs" style="${px(54, 13, 1, 20)}background:rgba(255,200,140,.25)"></div>`, pw);
    hdr.name = H(`<div class="vfd abs clk" style="${px(66, 10, 270)}font-size:19px;font-weight:300;letter-spacing:4px;overflow:hidden">—</div>`, pw);
    hdr.kind = H(`<div class="vfd abs" style="right:18px;top:15px;font-size:9px;letter-spacing:1.6px;opacity:.6;text-align:right">FACTORY</div>`, pw);
    hdr.name.addEventListener('pointerdown', (e) => { e.preventDefault(); openBrowser(); });
    const arr = (x, t, d) => { const a = H(`<div class="arrow" style="${px(x, 23)}">${t}</div>`); a.addEventListener('pointerdown', (e) => { e.preventDefault(); stepPreset(d); }); };
    arr(1036, '&#9664;', -1); arr(1072, '&#9654;', 1);
    const hb = (x, t, f) => { const b = H(`<div class="hbtn" style="${px(x, 25, 74, 26)}">${t}</div>`); b.addEventListener('pointerdown', (e) => { e.preventDefault(); f(); }); return b; };
    hb(1124, 'BROWSE', openBrowser); hb(1206, 'SAVE', openSave);
    hdr.stat = H(`<div class="abs" style="${px(1288, 20)}font-size:9px;letter-spacing:2px;line-height:14px;font-weight:600;color:rgba(243,233,210,.75)">VOICES 00/48<br>CPU 0 %<br>48 kHz</div>`);
    hdr.midi = H(`<div class="jewel t" style="${px(1390, 20)}"></div>`);
    H(`<div class="abs" style="${px(1410, 21)}font-size:9px;letter-spacing:2px;font-weight:600;color:rgba(243,233,210,.75)">MIDI</div>`);
    hdr.drift = H(`<div class="jewel" style="${px(1390, 42)}"></div>`);
    H(`<div class="abs" style="${px(1410, 43)}font-size:9px;letter-spacing:2px;font-weight:600;color:rgba(243,233,210,.75)">DRIFT</div>`);

    for (let i = 0; i < 3; i++) buildLayer(i);
    buildOrbit();
    [0, 7, 14].forEach((d, k) => H(`<div class="chrome-line" style="${px(16 + k * 40, 738 + d, 1444 - k * 80)}height:2px;opacity:${1 - k * 0.2}"></div>`));
    buildBottom();
    // overlays
    H(`<div id="toast" class="hidden"></div>`, document.getElementById('stage'));
    H(`<div id="assignBar" class="hidden"></div>`, document.getElementById('stage'));
    document.getElementById('assignBar').addEventListener('pointerdown', () => setAssign(0));
  }

  // ------------------------------------------------------------------------------------ layers ---
  const LX = 16, LW = 1002, LH = 196;
  function buildLayer(i) {
    const y = 98 + i * (LH + 14);
    const b = H(`<div class="card" style="${px(LX, y, LW, LH)}"></div>`);
    const Ly = { i, card: b };
    layers[i] = Ly;
    const tab = H(`<div class="band" style="${px(10, 10, 64, LH - 20)}border-radius:22px;box-shadow:inset 0 2px 5px rgba(0,0,0,.6),0 1px 0 rgba(255,255,255,.8)"><canvas class="wood" data-seed="${300 + i * 91}"></canvas></div>`, b);
    nix(tab, 21, 16, String(i + 1));
    H(`<div class="abs" style="${px(0, 50, 64)}text-align:center;font-size:8px;letter-spacing:2.4px;font-weight:700;color:rgba(243,233,210,.8)">LAYER</div>`, tab);
    Ly.jewel = H(`<div class="jewel t" style="${px(25, 70)}"></div>`, tab);
    H(`<div class="abs" style="${px(0, 96, 64)}text-align:center;font-size:8px;letter-spacing:1.6px;font-weight:600;color:rgba(243,233,210,.65)">LEVEL</div>`, tab);
    const lvT = H(`<div class="abs" style="${px(0, 152, 64)}text-align:center;font-size:8.5px;letter-spacing:.8px;color:#f3e9d2;z-index:2">0.0 dB</div>`, tab);
    Ly.level = Knob(tab, 14, 110, 36, {}, hostBind(LP(i, L.LEVEL), (n) => dbText(2 * n * n), Math.sqrt(0.5)), lvT);
    // source
    const sx = 90;
    H(`<div class="ttl" style="${px(sx + 4, 16)}">SOURCE</div>`, b);
    Ly.srcPill = pill(b, sx + 74, 11, ['SAMPLE', 'SYNTH', 'INPUT'], 'tight', (k) => setSourceKind(i, ['sample', 'synth', 'input'][k]));
    const sw = win(b, sx, 40, 212, 92, 46); sw.classList.add('wave');
    Ly.srcWin = sw;
    Ly.srcSvg = sv('svg', { viewBox: '0 0 160 80', preserveAspectRatio: 'none', style: 'left:26px;width:160px;top:6px;height:80px' }, sw.firstChild);
    Ly.srcPath = sv('path', { d: '', fill: '#ffb347', opacity: 0.85 }, Ly.srcSvg);
    Ly.srcLine = sv('line', { x1: 0, y1: 40, x2: 160, y2: 40, stroke: '#ffb347', 'stroke-width': 0.6, opacity: 0.6 }, Ly.srcSvg);
    Ly.srcHint = H(`<div class="hint"></div>`, sw.firstChild);
    Ly.srcName = H(`<div class="lbl clk" style="${px(sx + 4, 142, 220)}text-align:left;font-size:12px;letter-spacing:2.2px;overflow:hidden">—</div>`, b);
    Ly.srcSub = H(`<div class="sub" style="${px(sx + 4, 160)}">—</div>`, b);
    H(`<div class="op" style="${px(sx + 222, 73)}"><svg viewBox="-13 -13 26 26" style="position:absolute;inset:0;width:26px;height:26px"><circle r="8.5" fill="#f6efde" stroke="#208a80" stroke-width="1.5"/>
      <g stroke="#1d6f67" stroke-width="1.4" stroke-linecap="round"><line x1="0" y1="-5" x2="0" y2="5"/><line x1="-4.3" y1="-2.5" x2="4.3" y2="2.5"/><line x1="-4.3" y1="2.5" x2="4.3" y2="-2.5"/></g></svg></div>`, b);
    // impulse
    const ix = 340, iw = 196;
    H(`<div class="ttl" style="${px(ix + 4, 16)}">IMPULSE</div>`, b);
    Ly.impPill = pill(b, ix + 78, 11, ['SMP', 'SYN', 'LIVE', 'INPUT'], 'tight', (k) => setImpulseKind(i, ['sample', 'synth', 'live', 'input'][k]));
    const iwn = win(b, ix, 40, iw, 92, 0); iwn.style.borderRadius = '50%'; iwn.classList.add('wave');
    Ly.impWin = iwn;
    Ly.impSvg = sv('svg', { viewBox: '0 0 136 76', preserveAspectRatio: 'none', style: 'left:30px;width:136px;top:8px;height:76px' }, iwn.firstChild);
    Ly.impPath = sv('path', { d: '', fill: '#ffb347', opacity: 0.85 }, Ly.impSvg);
    Ly.impHint = H(`<div class="hint"></div>`, iwn.firstChild);
    Ly.impName = H(`<div class="lbl clk" style="${px(ix, 142, iw)}font-size:11.5px;letter-spacing:2px;overflow:hidden">—</div>`, b);
    Ly.impSub = H(`<div class="sub c" style="${px(ix, 160, iw)}">—</div>`, b);
    for (const [el, which] of [[sw, 'src'], [Ly.srcName, 'src'], [iwn, 'imp'], [Ly.impName, 'imp']])
      el.addEventListener('pointerdown', (e) => { if (e.button === 0) { e.preventDefault(); openInspector(i, which); } });
    dropTarget(sw, i, 'src');
    dropTarget(iwn, i, 'imp');
    H(`<div class="vrule" style="${px(552, 18, null, LH - 36)}"></div>`, b);
    // matrix
    const mx = 566, cw = 58;
    H(`<div class="ttl" style="${px(mx, 16)}">CONVOLVE MATRIX</div>`, b);
    Ly.sliders = DIMS.map((d, k) => Slider(b, mx + k * cw + cw / 2, hostBind(LP(i, L.BODY + k), pct, k === 0 ? 1 : 0), d));
    H(`<div class="vrule" style="${px(864, 18, null, LH - 36)}"></div>`, b);
    // filter + envelope
    const fx = 878;
    H(`<div class="ttl" style="${px(fx, 16)}">FILTER</div>`, b);
    Ly.ftype = H(`<div class="abs clk" style="${px(fx, 34)}font-size:9px;letter-spacing:1.8px;font-weight:700;color:var(--teal-d)">LADDER 24 &#9662;</div>`, b);
    Ly.ftype.addEventListener('pointerdown', (e) => { if (tryAssign(LP(i, L.FTYPE), e)) return; e.preventDefault(); filterMenu(i, Ly.ftype); });
    const cutT = H(`<div class="sub c" style="${px(fx - 8, 109, 62)}font-size:9px">—</div>`, b);
    const resT = H(`<div class="sub c" style="${px(fx + 46, 109, 62)}font-size:9px">—</div>`, b);
    Ly.cut = Knob(b, fx - 2, 46, 50, { ticks: true }, hostBind(LP(i, L.CUT), (n) => hzText(20 * Math.pow(1000, n)), 1), cutT);
    Ly.res = Knob(b, fx + 56, 50, 42, { ticks: true }, hostBind(LP(i, L.RES), pct, 0), resT);
    H(`<div class="lbl" style="${px(fx - 8, 97, 62)}font-size:9px">CUTOFF</div>`, b);
    H(`<div class="lbl" style="${px(fx + 46, 97, 62)}font-size:9px">RES</div>`, b);
    const eg = win(b, fx - 4, 126, 116, 56, 16);
    Ly.env = buildEnv(eg.firstChild, i);
  }

  function buildEnv(g, i) {
    const W = 96, Hh = 42;
    const s = sv('svg', { viewBox: `0 0 ${W} ${Hh}`, style: `left:8px;top:5px;width:${W}px;height:${Hh}px;overflow:visible` }, g);
    const fillP = sv('path', { d: '', fill: '#ffb347', opacity: 0.14 }, s);
    const line = sv('path', { d: '', fill: 'none', stroke: '#ffb347', 'stroke-width': 1.6, 'stroke-linejoin': 'round' }, s);
    const dots = [0, 1, 2].map(() => sv('circle', { r: 2.6, fill: '#ffcf8a', style: 'cursor:grab' }, s));
    const tag = H(`<div class="abs clk" style="right:9px;top:3px;font-size:7.5px;letter-spacing:1.4px;font-weight:700;color:rgba(255,190,110,.8);z-index:3">AMP</div>`, g);
    const env = { mode: 0, i };   // 0 amp, 1 filter
    const ids = () => env.mode ? [LP(i, L.FA), LP(i, L.FD), LP(i, L.FS), LP(i, L.FR)] : [LP(i, L.A), LP(i, L.D), LP(i, L.S), LP(i, L.R)];
    const pad = 3, Wd = W - 2 * pad, Hd = Hh - 2 * pad, seg = 0.28 * Wd;
    function geo() {
      const [ia, id_, is, ir] = ids();
      const a = coreVal(ia), d = coreVal(id_), su = coreVal(is), r = coreVal(ir);
      const xa = pad + seg * Math.sqrt(a / 10), xd = xa + seg * Math.sqrt(d / 10), xs = xd + 0.16 * Wd, xr = xs + seg * Math.sqrt(r / 20);
      const ys = pad + (1 - su) * Hd;
      return { xa, xd, xs, xr, ys, bottom: pad + Hd };
    }
    env.update = () => {
      const q = geo();
      const ln = `M${pad} ${q.bottom} L${q.xa.toFixed(1)} ${pad} Q${(q.xa + (q.xd - q.xa) * 0.25).toFixed(1)} ${q.ys.toFixed(1)} ${q.xd.toFixed(1)} ${q.ys.toFixed(1)} L${q.xs.toFixed(1)} ${q.ys.toFixed(1)} Q${(q.xs + (q.xr - q.xs) * 0.2).toFixed(1)} ${q.bottom.toFixed(1)} ${q.xr.toFixed(1)} ${q.bottom.toFixed(1)}`;
      line.setAttribute('d', ln);
      fillP.setAttribute('d', ln + ` L${pad} ${q.bottom} Z`);
      [[q.xa, pad], [q.xd, q.ys], [q.xr, q.bottom]].forEach(([x, y], k) => { dots[k].setAttribute('cx', x.toFixed(1)); dots[k].setAttribute('cy', y.toFixed(1)); });
      tag.textContent = env.mode ? 'FLT ' + (S.core[LP(i, L.FAMT)] >= 0 ? '+' : '') + Math.round(S.core[LP(i, L.FAMT)] * 100) + '%' : 'AMP';
    };
    tag.addEventListener('pointerdown', (e) => { e.preventDefault(); e.stopPropagation(); env.mode ^= 1; env.update(); });
    tag.addEventListener('wheel', (e) => {
      e.preventDefault();
      if (!env.mode) return;
      const id = LP(i, L.FAMT), v = clamp(S.core[id] + (e.deltaY < 0 ? 0.05 : -0.05), -1, 1);
      S.core[id] = Math.round(v * 100) / 100; send('o.set', [id, S.core[id]]); markEdited(); env.update();
    }, { passive: false });
    dots.forEach((dot, k) => {
      let on = false;
      dot.addEventListener('pointerdown', (e) => { if (tryAssign(ids()[k === 0 ? 0 : k === 1 ? 1 : 3], e)) return; e.preventDefault(); e.stopPropagation(); dot.setPointerCapture(e.pointerId); on = true; });
      dot.addEventListener('pointermove', (e) => {
        if (!on) return;
        const r = s.getBoundingClientRect();
        const x = (e.clientX - r.left) / r.width * W, y = (e.clientY - r.top) / r.height * Hh;
        const q = geo(), [ia, id_, is, ir] = ids();
        const put = (id, v) => { if (v !== S.core[id]) { S.core[id] = v; send('o.set', [id, v]); markEdited(); } };
        const t = (dx, max) => Math.round(max * Math.pow(clamp(dx / seg, 0, 1), 2) * 1000) / 1000;
        if (k === 0) put(ia, t(x - pad, 10));
        else if (k === 1) { put(id_, t(x - q.xa, 10)); put(is, Math.round(clamp(1 - (y - pad) / Hd, 0, 1) * 100) / 100); }
        else put(ir, t(x - q.xs, 20));
        env.update();
      });
      dot.addEventListener('pointerup', () => { on = false; });
    });
    widgets.push(env);
    return env;
  }

  // --------------------------------------------------------------------------------- the orbit ---
  const FW = 388, FH = 330, FX0 = 20, FY0 = 46;
  const FIW = FW - 14, FIH = FH - 14, FCX = FIW / 2, FCY = FIH / 2, FK = 0.82;
  const fx2px = (x) => FCX + x * FCX * FK, fy2px = (y) => FCY - y * FCY * FK;
  const px2fx = (x) => (x - FCX) / (FCX * FK), px2fy = (y) => -(y - FCY) / (FCY * FK);
  function buildOrbit() {
    const ob = H(`<div class="card" style="${px(1032, 98, 428, 3 * LH + 28)}"></div>`);
    orb.card = ob;
    H(`<div class="ttl" style="${px(26, 18)}">ORBIT</div>`, ob);
    // MOTION ON / OFF: off = the puck stays exactly where it is put (HOLD); on = it travels the chosen path
    orb.motion = H(`<div id="motionsw" class="motion clk" style="${px(80, 12)}" title="orbit motion on / off (double-click the field to start, drag the puck to park it)"><i class="jewel"></i><span>MOTION</span><b>OFF</b></div>`, ob);
    orb.motion.addEventListener('pointerdown', (e) => { if (e.button !== 0) return; e.preventDefault(); setMotion(!motionOn()); });
    orb.pathPill = pill(ob, 182, 13, ['HOLD', 'CIRCLE', 'FIG 8', 'DRAWN'], 'or', (k) => setPath(k));
    const fwin = H(`<div class="abs" style="${px(FX0, FY0, FW, FH)}border-radius:50%;padding:7px;background:conic-gradient(from 200deg,#fff,#8e8f8d 15%,#f4f4f2 30%,#7a7b79 50%,#e9e9e7 66%,#9a9b99 82%,#fff);box-shadow:0 6px 14px rgba(60,40,20,.35),0 1px 2px rgba(60,40,20,.4)"></div>`, ob);
    const fg = H(`<div style="position:relative;width:100%;height:100%;border-radius:50%;overflow:hidden;background:radial-gradient(ellipse at 50% 45%,#2e1e12,#1a110a 55%,#0b0704);box-shadow:inset 0 3px 18px rgba(0,0,0,.95)"></div>`, fwin);
    const s = sv('svg', { id: 'field', viewBox: `0 0 ${FIW} ${FIH}`, style: 'position:absolute;inset:0;width:100%;height:100%' }, fg);
    orb.svg = s;
    [0.3, 0.55, 0.8].forEach((r) => sv('ellipse', { cx: FCX, cy: FCY, rx: r * FCX, ry: r * FCY, fill: 'none', stroke: 'rgba(255,190,110,.13)', 'stroke-width': 1 }, s));
    sv('line', { x1: 20, y1: FCY, x2: FIW - 20, y2: FCY, stroke: 'rgba(255,190,110,.10)' }, s);
    sv('line', { x1: FCX, y1: 16, x2: FCX, y2: FIH - 16, stroke: 'rgba(255,190,110,.10)' }, s);
    orb.halos = [0, 1, 2].map(() => sv('circle', { r: 70, fill: 'url(#ocHalo)', 'pointer-events': 'none' }, s));
    orb.path = sv('path', { d: '', fill: 'none', stroke: 'rgba(255,179,71,.55)', 'stroke-width': 1.6, 'stroke-dasharray': '6 5', 'stroke-linecap': 'round', 'pointer-events': 'none' }, s);
    orb.ink = sv('path', { d: '', fill: 'none', stroke: '#ffcf8a', 'stroke-width': 2, 'stroke-linecap': 'round', 'pointer-events': 'none' }, s);
    orb.trail = sv('path', { d: '', fill: 'none', stroke: '#ffb347', 'stroke-width': 2.6, 'stroke-linecap': 'round', 'pointer-events': 'none' }, s);
    orb.lines = [0, 1, 2, 3].map(() => sv('line', { stroke: '#3fb8ac', 'stroke-width': 1, 'pointer-events': 'none' }, s));
    orb.nodes = [0, 1, 2, 3].map((k) => {
      const g = sv('g', { class: 'node' }, s);
      if (k === 3) {
        sv('circle', { r: 16, fill: 'rgba(0,0,0,0)' }, g);
        sv('circle', { r: 13, fill: 'none', stroke: 'rgba(243,233,210,.6)', 'stroke-width': 1.2, 'stroke-dasharray': '2 3' }, g);
        const t = sv('text', { y: 3.5, 'text-anchor': 'middle', fill: 'rgba(243,233,210,.75)', 'font-size': 8.5, 'font-weight': 700, 'letter-spacing': 1, 'font-family': 'Bahnschrift', 'pointer-events': 'none' }, g); t.textContent = 'DRY';
      } else {
        sv('circle', { r: 21, fill: 'url(#ocNodeGlow)', 'pointer-events': 'none' }, g);
        sv('circle', { r: 15, fill: '#f3e9d2', stroke: '#3fb8ac', 'stroke-width': 2 }, g);
        const t = sv('text', { y: 4.5, 'text-anchor': 'middle', fill: '#1d6f67', 'font-size': 13, 'font-weight': 700, 'font-family': 'Bahnschrift', 'pointer-events': 'none' }, g); t.textContent = String(k + 1);
      }
      return g;
    });
    orb.puck = sv('g', { class: 'puck' }, s);
    sv('circle', { r: 20, fill: 'url(#ocPuckHalo)' }, orb.puck);
    sv('circle', { r: 11, fill: 'rgba(255,179,71,.18)', stroke: '#ffcf8a', 'stroke-width': 1.4 }, orb.puck);
    sv('circle', { r: 4.5, fill: '#fff1d6' }, orb.puck);
    orb.center = sv('g', { 'pointer-events': 'none', opacity: 0.6 }, s);
    sv('line', { x1: -5, y1: 0, x2: 5, y2: 0, stroke: '#ffcf8a', 'stroke-width': 1 }, orb.center);
    sv('line', { x1: 0, y1: -5, x2: 0, y2: 5, stroke: '#ffcf8a', 'stroke-width': 1 }, orb.center);
    orb.hint = sv('text', { x: FCX, y: FIH - 44, 'text-anchor': 'middle', fill: 'rgba(255,190,110,.5)', 'font-size': 8, 'letter-spacing': 2, 'font-family': 'Bahnschrift', 'pointer-events': 'none' }, s);
    orb.trailPts = [];
    fieldInput(s);
    // controls
    const oy = 392;
    H(`<div class="rule" style="${px(24, oy, 380)}"></div>`, ob);
    const ctl = [['RATE', SP.RATE, (n) => rateText(n), 0.35], ['WOBBLE', G.WOBBLE, pct, 0], ['SPREAD', SP.SPREAD, pct, 0.7], ['PUCK DRIFT', 'orbit_puck_drift', pct, 0]];
    orb.knobs = ctl.map(([n, t, f, d], k) => {
      const x = 24 + k * 64;
      H(`<div class="lbl" style="${px(x - 12, oy + 82, 76)}font-size:9px;letter-spacing:1.4px">${n}</div>`, ob);
      const txt = H(`<div class="sub c" style="${px(x - 12, oy + 97, 76)}">—</div>`, ob);
      const bnd = typeof t === 'string' ? idBind(t, f, d) : hostBind(t, f, d);
      if (t === SP.RATE) bnd.target = G.RATE;
      if (t === SP.SPREAD) bnd.target = G.SIZE_X;
      const kn = Knob(ob, x + 4, oy + 22, 48, { ticks: true }, bnd, txt);
      if (typeof t === 'string') kn.el.classList.add('puckdrift');
      return kn;
    });
    H(`<div class="ttl dim" style="${px(296, oy + 18)}">SYNC</div>`, ob);
    orb.sync = pill(ob, 296, oy + 32, ['ON', 'FREE'], '', (k) => { S.sync = k === 0; send('o.sync', [S.sync]); markEdited(); updateAll(); });
    H(`<div class="ttl dim" style="${px(296, oy + 64)}">DIRECTION</div>`, ob);
    orb.dir = pill(ob, 296, oy + 78, ['CW', 'CCW'], '', (k) => { S.core[G.DIR] = k; send('o.set', [G.DIR, k]); markEdited(); updateAll(); });
    const wy = oy + 126;
    H(`<div class="rule" style="${px(24, wy, 380)}"></div>`, ob);
    orb.blendT = H(`<div class="ttl dim" style="${px(26, wy + 12)}">BLEND · PUCK X 0.00 Y 0.00</div>`, ob);
    orb.wn = [['L1'], ['L2'], ['L3'], ['DRY']].map(([n], k) => {
      const x = 30 + k * 96;
      const e = nix(ob, x, wy + 34, '__');
      H(`<div class="lbl" style="${px(x + 38, wy + 41)}text-align:left;font-size:10px;color:var(--teal-d)">${n}</div>`, ob);
      return e;
    });
  }

  function fieldPoint(e) {
    const r = orb.svg.getBoundingClientRect();
    return [px2fx((e.clientX - r.left) / r.width * FIW), px2fy((e.clientY - r.top) / r.height * FIH)];
  }
  let puckLocal = null, puckLocalUntil = 0;
  function setPuck(x, y) {
    x = clamp(x, -1, 1); y = clamp(y, -1, 1);
    const ix = hostIdx.get(G.PUCK_X), iy = hostIdx.get(G.PUCK_Y);
    HV[ix] = (x + 1) / 2; HV[iy] = (y + 1) / 2;
    send('o.host', [ix, HV[ix]]); send('o.host', [iy, HV[iy]]);
    puckLocal = [x, y]; puckLocalUntil = performance.now() + 250;
    markEdited();
  }
  function fieldInput(s) {
    let drag = null, ink = null;
    s.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      const node = e.target.closest('.node'), puck = e.target.closest('.puck');
      if (assigning) {
        if (node) { const k = orb.nodes.indexOf(node); tryAssign(k < 3 ? G.NODE1_X + 2 * k : G.DRY_X, e); }
        else tryAssign(G.PUCK_X, e);
        return;
      }
      e.preventDefault();
      s.setPointerCapture(e.pointerId);
      if (node) drag = { node: orb.nodes.indexOf(node) };
      else if (puck || S.path !== 3) {
        // the puck goes where it is dragged; while the orbit moves, that parks it there (motion off)
        drag = { puck: true };
        const [x, y] = fieldPoint(e);
        if (S.path !== 0) parkPuck(x, y); else setPuck(x, y);
        updateField();
      }
      else { ink = [fieldPoint(e)]; s.classList.add('drawing'); }
    });
    s.addEventListener('pointermove', (e) => {
      const [x, y] = fieldPoint(e);
      if (drag && drag.node != null) {
        const id = drag.node < 3 ? G.NODE1_X + 2 * drag.node : G.DRY_X;
        S.core[id] = Math.round(clamp(x, -1, 1) * 1000) / 1000; S.core[id + 1] = Math.round(clamp(y, -1, 1) * 1000) / 1000;
        send('o.set', [id, S.core[id]]); send('o.set', [id + 1, S.core[id + 1]]);
        markEdited(); updateField();
      } else if (drag && drag.puck) { setPuck(x, y); updateField(); }
      else if (ink) {
        const l = ink[ink.length - 1];
        if (Math.hypot(x - l[0], y - l[1]) > 0.025) { ink.push([clamp(x, -1, 1), clamp(y, -1, 1)]); drawInk(ink); }
      }
    });
    const end = () => {
      if (ink) {
        s.classList.remove('drawing');
        if (ink.length >= 4) commitDrawn(ink);
        else toast('DRAW A LONGER LOOP');
        orb.ink.setAttribute('d', '');
      }
      drag = null; ink = null;
    };
    s.addEventListener('pointerup', end);
    s.addEventListener('pointercancel', end);
    // double-click on the field (not a node): the orbit moves again, around where the puck is
    s.addEventListener('dblclick', (e) => {
      if (e.target.closest('.node') || assigning) return;
      e.preventDefault();
      if (!motionOn()) setMotion(true);
    });
  }
  function parkPuck(x, y) {
    x = clamp(x, -1, 1); y = clamp(y, -1, 1);
    const ix = hostIdx.get(G.PUCK_X), iy = hostIdx.get(G.PUCK_Y);
    HV[ix] = (x + 1) / 2; HV[iy] = (y + 1) / 2;
    S.path = 0;
    const mi = hid('orbit_motion'); if (mi >= 0) HV[mi] = 0;
    send('o.park', [+x.toFixed(4), +y.toFixed(4)]);
    puckLocal = [x, y]; puckLocalUntil = performance.now() + 250;
    orb.trailPts.length = 0;
    markEdited(); updateMotion();
    toast('PUCK PARKED \u00b7 DOUBLE-CLICK THE FIELD TO ORBIT AGAIN');
  }
  function setMotion(on) {
    if (on) {
      const k = shapeOf();
      if (k === 3 && (!S.drawn || S.drawn.length < 3)) { S.shape = 3; S.path = 3; toast('DRAG ON THE SCREEN TO DRAW A LOOP'); }
      else S.path = k;
    } else S.path = 0;
    const mi = hid('orbit_motion'); if (mi >= 0) HV[mi] = on ? 1 : 0;
    send('o.motion', [!!on]);
    orb.trailPts.length = 0;
    markEdited(); updateAll();
  }
  function updateMotion() {
    if (!orb.motion) return;
    const on = motionOn();
    orb.motion.classList.toggle('on', on);
    orb.motion.querySelector('.jewel').classList.toggle('on', on);
    orb.motion.querySelector('b').textContent = on ? 'ON' : 'OFF';
    orb.pathPill.setOn(on ? S.path : 0);
    [...orb.pathPill.children].forEach((b, k) => b.classList.toggle('sel', !on && k === shapeOf()));
  }
  function drawInk(pts) { orb.ink.setAttribute('d', pts.map((p, i) => (i ? 'L' : 'M') + fx2px(p[0]).toFixed(1) + ' ' + fy2px(p[1]).toFixed(1)).join(' ')); }
  function commitDrawn(pts) {
    // resample to <= 64 points evenly along the stroke (closed loop)
    const cum = [0];
    for (let i = 1; i < pts.length; i++) cum.push(cum[i - 1] + Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
    const total = cum[cum.length - 1], n = Math.min(64, Math.max(8, Math.round(total / 0.05)));
    const out = [];
    for (let k = 0; k < n; k++) {
      const at = k / n * total;
      let j = 1; while (j < cum.length - 1 && cum[j] < at) j++;
      const t = (at - cum[j - 1]) / Math.max(1e-6, cum[j] - cum[j - 1]);
      out.push([+(pts[j - 1][0] + (pts[j][0] - pts[j - 1][0]) * t).toFixed(3), +(pts[j - 1][1] + (pts[j][1] - pts[j - 1][1]) * t).toFixed(3)]);
    }
    S.drawn = out;
    S.path = 3; S.shape = 3;
    { const mi = hid('orbit_motion'); if (mi >= 0) HV[mi] = 1; }
    send('o.drawn', [out]);
    const cx = out.reduce((a, p) => a + p[0], 0) / out.length, cy = out.reduce((a, p) => a + p[1], 0) / out.length;
    const si = hostIdx.get(SP.SPREAD); HV[si] = 0.7; send('o.host', [si, 0.7]);
    setPuck(cx, cy);
    updateAll();
    toast('ORBIT PATH DRAWN');
  }
  function setPath(k) {
    const mi = hid('orbit_motion');
    if (k === 3 && S.path === 3) { toast('DRAG ON THE SCREEN TO DRAW A NEW LOOP'); return; }
    if (k) S.shape = k;
    if (mi >= 0) HV[mi] = k ? 1 : 0;
    orb.trailPts.length = 0;
    if (k === 3 && (!S.drawn || S.drawn.length < 3)) {
      S.path = 3; send('o.path', [3]); updateAll(); toast('DRAG ON THE SCREEN TO DRAW A LOOP'); return;
    }
    S.path = k; send('o.path', [k]); markEdited(); updateAll();
  }
  function pathPoints() {
    const cx = coreVal(G.PUCK_X), cy = coreVal(G.PUCK_Y), sp = HV[hostIdx.get(SP.SPREAD)];
    const pts = [];
    if (S.path === 1 || S.path === 2) {
      const rot = S.core[G.ROTATE] * 2 * Math.PI, cr = Math.cos(rot), sr = Math.sin(rot), sx = sp, sy = sp * 0.64;
      for (let i = 0; i <= 96; i++) {
        const th = i / 96 * 2 * Math.PI;
        const ox = S.path === 1 ? sx * Math.cos(th) : sx * Math.sin(th), oy = S.path === 1 ? sx * Math.sin(th) : sy * Math.sin(2 * th);
        pts.push([cx + ox * cr - oy * sr, cy + ox * sr + oy * cr]);
      }
    } else if (S.path === 3 && S.drawn && S.drawn.length > 2) {
      const n = S.drawn.length, mx = S.drawn.reduce((a, p) => a + p[0], 0) / n, my = S.drawn.reduce((a, p) => a + p[1], 0) / n, k = sp / 0.7;
      S.drawn.forEach((p) => pts.push([clamp(cx + (p[0] - mx) * k, -1, 1), clamp(cy + (p[1] - my) * k, -1, 1)]));
      pts.push(pts[0]);
    }
    return pts;
  }
  function updateField() {
    const nodePos = [0, 1, 2, 3].map((k) => { const id = k < 3 ? G.NODE1_X + 2 * k : G.DRY_X; return [S.core[id], S.core[id + 1]]; });
    nodePos.forEach(([x, y], k) => {
      orb.nodes[k].setAttribute('transform', `translate(${fx2px(x).toFixed(1)} ${fy2px(y).toFixed(1)})`);
      if (k < 3) { orb.halos[k].setAttribute('cx', fx2px(x).toFixed(1)); orb.halos[k].setAttribute('cy', fy2px(y).toFixed(1)); }
    });
    const pts = pathPoints();
    orb.path.setAttribute('d', pts.map((p, i) => (i ? 'L' : 'M') + fx2px(p[0]).toFixed(1) + ' ' + fy2px(p[1]).toFixed(1)).join(' '));
    const cx = coreVal(G.PUCK_X), cy = coreVal(G.PUCK_Y);
    orb.center.setAttribute('transform', `translate(${fx2px(cx).toFixed(1)} ${fy2px(cy).toFixed(1)})`);
    orb.center.style.display = S.path ? '' : 'none';
    updateMotion();
    orb.hint.textContent = S.path === 3 ? 'DRAW A LOOP \u00b7 GRAB THE PUCK TO PARK IT' : S.path === 0 ? 'DRAG THE PUCK \u00b7 DOUBLE-CLICK TO ORBIT' : 'DRAG TO PARK THE PUCK';
    if (!lastMeter) placePuck(cx, cy, [0.25, 0.25, 0.25, 0.25]);
  }
  function placePuck(x, y, w) {
    if (puckLocal && performance.now() < puckLocalUntil && S.path === 0) [x, y] = puckLocal;
    const X = fx2px(x), Y = fy2px(y);
    orb.puck.setAttribute('transform', `translate(${X.toFixed(1)} ${Y.toFixed(1)})`);
    const nodes = [0, 1, 2, 3].map((k) => { const id = k < 3 ? G.NODE1_X + 2 * k : G.DRY_X; return [fx2px(S.core[id]), fy2px(S.core[id + 1])]; });
    nodes.forEach(([nx, ny], k) => {
      const l = orb.lines[k];
      l.setAttribute('x1', X.toFixed(1)); l.setAttribute('y1', Y.toFixed(1)); l.setAttribute('x2', nx.toFixed(1)); l.setAttribute('y2', ny.toFixed(1));
      l.setAttribute('stroke-opacity', (0.12 + 0.75 * (w[k] || 0)).toFixed(2));
    });
    const t = orb.trailPts;
    t.push([X, Y]); if (t.length > 40) t.shift();
    orb.trail.setAttribute('d', S.path ? t.map((p, i) => (i ? 'L' : 'M') + p[0].toFixed(1) + ' ' + p[1].toFixed(1)).join(' ') : '');
  }

  // -------------------------------------------------------------------------------- bottom row ---
  function buildBottom() {
    const bb = H(`<div class="card" style="${px(16, 772, 1444, 212)}border-radius:34px"></div>`);
    H(`<div class="ttl" style="${px(34, 18)}">MASTER</div>`, bb);
    bot.master = [['DRIFT', G.DRIFT, 0.2], ['TAPE AGE', SP.AGE, 0]].map(([n, t, d], k) => {
      const x = 34 + k * 98;
      H(`<div class="lbl" style="${px(x - 11, 146, 100)}">${n}</div>`, bb);
      const txt = H(`<div class="sub c" style="${px(x - 11, 162, 100)}">—</div>`, bb);
      const bnd = hostBind(t, pct, d);
      if (t === SP.AGE) bnd.target = G.DRIVE;
      return Knob(bb, x, 52, 78, { ticks: true }, bnd, txt);
    });
    H(`<div class="vrule" style="${px(236, 20, null, 172)}"></div>`, bb);
    H(`<div class="ttl" style="${px(256, 18)}">EFFECTS</div>`, bb);
    H(`<div class="ttl dim" style="${px(352, 19)}">IN SERIES</div>`, bb);
    bot.fx = FXN.map((name, k) => {
      const x = 252 + k * 112, u = H(`<div class="cup" style="${px(x, 44, 104, 150)}border-radius:52px 52px 22px 22px"></div>`, bb);
      const jw = H(`<div class="jewel btn" style="${px(45, 12)}" title="on / bypass"></div>`, u);
      const lbl = H(`<div class="lbl" style="${px(0, 104, 104)}font-size:9.5px;letter-spacing:1.6px">${name}</div>`, u);
      const sub = H(`<div class="sub c" style="${px(0, 120, 104)}">—</div>`, u);
      const bnd = hostBind(SP.FX0 + k, pct, 0.3);
      bnd.target = null;
      const kn = Knob(u, 22, 34, 60, { ticks: true }, bnd, null);
      jw.addEventListener('pointerdown', (e) => { e.preventDefault(); S.fx.on[k] = !S.fx.on[k]; send('o.fx', [k, S.fx.on[k]]); markEdited(); updateFx(); });
      if (k === 2 || k === 5) { lbl.classList.add('clk'); lbl.title = k === 2 ? 'click: ensemble mode' : 'click: twin tank / spring'; }
      if (k === 3 || k === 4) { sub.classList.add('clk'); sub.title = 'click: tempo sync'; }
      lbl.addEventListener('pointerdown', (e) => {
        if (k === 2) { S.fx.ens = S.fx.ens % 4 + 1; send('o.fxopt', [{ ens: S.fx.ens }]); }
        else if (k === 5) { S.fx.rev = S.fx.rev ? 0 : 1; send('o.fxopt', [{ rev: S.fx.rev }]); }
        else return;
        e.preventDefault(); markEdited(); updateFx();
      });
      sub.addEventListener('pointerdown', (e) => {
        if (k !== 3 && k !== 4) return;
        e.preventDefault();
        const key = k === 3 ? 'bbdSync' : 'oilSync';
        S.fx[key] = (S.fx[key] + (e.shiftKey ? 13 : 1)) % 14;
        send('o.fxopt', [{ [key]: S.fx[key] }]); markEdited(); updateFx();
      });
      kn.update = ((orig) => function () { orig.call(this); updateFx(); })(kn.update);
      return { u, jw, lbl, sub, kn };
    });
    // INPUT: the sidechain (another track's audio) as it arrives, and its trim
    H(`<div class="vrule" style="${px(934, 20, null, 172)}"></div>`, bb);
    H(`<div class="ttl" style="${px(946, 18)}">INPUT</div>`, bb);
    H(`<div class="ttl dim" style="${px(946, 31)}font-size:7.5px;letter-spacing:1.6px">SIDECHAIN</div>`, bb);
    const im = H(`<div id="inmeter" class="abs" style="${px(950, 48, 58, 72)}border-radius:9px;padding:3px;background:linear-gradient(160deg,#fff,#a9aaa9 35%,#f0f0ee 55%,#7f807e 85%,#d9dad8);box-shadow:0 2px 4px rgba(60,40,20,.35)"></div>`, bb);
    const imf = H(`<div style="position:relative;width:100%;height:100%;border-radius:6px;overflow:hidden;background:radial-gradient(ellipse at 50% 40%,#2e1e12,#120b06);box-shadow:inset 0 2px 5px rgba(0,0,0,.8)"></div>`, im);
    bot.inBars = [0, 1].map((c) => H(`<div class="inbar" style="${px(10 + c * 20, 6, 12, 54)}"><i></i></div>`, imf));
    bot.inPeak = H(`<div class="jewel" style="${px(44, 4)}width:8px;height:8px" title="input peak"></div>`, imf);
    bot.inNone = H(`<div class="abs" style="${px(0, 22, 52)}display:none;text-align:center;font-size:7px;letter-spacing:1px;line-height:9px;font-weight:700;color:rgba(255,190,110,.75)">NO INPUT<br>BUS</div>`, imf);
    bot.inSub = H(`<div class="sub c" style="${px(938, 122, 82)}font-size:8px">—</div>`, bb);
    const tt = H(`<div class="sub c" style="${px(938, 180, 82)}">—</div>`, bb);
    H(`<div class="lbl" style="${px(938, 168, 82)}font-size:8.5px;letter-spacing:1.4px">TRIM</div>`, bb);
    bot.trim = Knob(bb, 961, 132, 36, {}, idBind('orbit_input_trim', (n) => { const d = -24 + 48 * n; return (d > 0.05 ? '+' : '') + d.toFixed(1) + ' dB'; }, 0.5), tt);
    bot.trim.el.id = 'intrim';
    H(`<div class="vrule" style="${px(1018, 20, null, 172)}"></div>`, bb);
    H(`<div class="ttl" style="${px(1036, 18)}">MACROS</div>`, bb);
    const asg = H(`<div class="ttl dim clk" style="${px(1140, 19)}" title="modulation slots: sources, targets, amounts">ASSIGN ▾</div>`, bb);
    asg.addEventListener('pointerdown', (e) => { e.preventDefault(); openAssignList(); });
    bot.macros = [0, 1, 2, 3].map((k) => {
      const x = 1034 + k * 74;
      const n = nix(bb, x + 22, 44, String(k + 1), true);
      n.classList.add('clk'); n.title = 'click, then click a control to assign this macro';
      n.addEventListener('pointerdown', (e) => { e.preventDefault(); setAssign(assigning === k + 1 ? 0 : k + 1); });
      const name = H(`<div class="lbl clk" style="${px(x - 8, 140, 84)}font-size:9.5px;letter-spacing:1.2px;overflow:hidden" title="double-click to rename">MACRO</div>`, bb);
      name.addEventListener('dblclick', (e) => { e.preventDefault(); renameMacro(k); });
      const txt = H(`<div class="sub c" style="${px(x - 8, 156, 84)}">—</div>`, bb);
      const bnd = hostBind(SP.MAC0 + k, pct, 0.5);
      bnd.target = null;
      const kn = Knob(bb, x + 7, 74, 54, { pointer: '#e2681b', arc: '#e2681b' }, bnd, txt);
      return { n, name, kn };
    });
    H(`<div class="vrule" style="${px(1338, 20, null, 172)}"></div>`, bb);
    H(`<div class="ttl" style="${px(1354, 18)}">OUTPUT</div>`, bb);
    const vu = H(`<div class="abs" style="${px(1352, 44, 76, 62)}border-radius:10px;padding:3px;background:linear-gradient(160deg,#fff,#a9aaa9 35%,#f0f0ee 55%,#7f807e 85%,#d9dad8);box-shadow:0 2px 4px rgba(60,40,20,.35)"></div>`, bb);
    const vf = H(`<div style="position:relative;width:100%;height:100%;border-radius:7px;overflow:hidden;background:radial-gradient(ellipse at 50% 100%,#fff2cf,#f0dca8 70%,#d9c08a);box-shadow:inset 0 2px 5px rgba(0,0,0,.45)"></div>`, vu);
    const s = sv('svg', { viewBox: '0 0 70 56', style: 'position:absolute;inset:0;width:100%;height:100%' }, vf);
    sv('path', { d: 'M12 34 A30 30 0 0 1 58 34', fill: 'none', stroke: '#3a2a1a', 'stroke-width': 0.8 }, s);
    sv('path', { d: 'M48 26 A30 30 0 0 1 58 34', fill: 'none', stroke: '#c8432b', 'stroke-width': 2 }, s);
    for (let k = 0; k <= 8; k++) { const a = -50 + k * 12.5, [x0, y0] = pol(30, a), [x1, y1] = pol(k % 2 ? 26.5 : 25, a); sv('line', { x1: 35 + x0, y1: 50 + y0, x2: 35 + x1, y2: 50 + y1, stroke: '#3a2a1a', 'stroke-width': 0.7 }, s); }
    const t = sv('text', { x: 35, y: 44, 'text-anchor': 'middle', 'font-size': 6, 'font-family': 'Bahnschrift', 'letter-spacing': 1, fill: '#3a2a1a' }, s); t.textContent = 'VU';
    bot.needle = sv('line', { x1: 35, y1: 52, x2: 35, y2: 18, stroke: '#1a120c', 'stroke-width': 0.9, transform: 'rotate(-50 35 52)' }, s);
    sv('circle', { cx: 35, cy: 52, r: 3, fill: '#1a120c' }, s);
    bot.peak = H(`<div class="jewel" style="${px(1418, 108)}width:10px;height:10px" title="peak"></div>`, bb);
    const ot = H(`<div class="sub c" style="${px(1346, 170, 88)}color:var(--ink);font-weight:700">—</div>`, bb);
    bot.out = Knob(bb, 1366, 116, 48, { ticks: true }, hostBind(G.MASTER, (n) => dbText(2 * n * n), Math.sqrt(0.4)), ot);
    bot.needleAng = -50;
  }
  function updateFx() {
    bot.fx.forEach((f, k) => {
      const on = !!S.fx.on[k], v = HV[hostIdx.get(SP.FX0 + k)] || 0;
      f.jw.classList.toggle('on', on);
      f.u.classList.toggle('byp', !on);
      if (k === 2) f.lbl.textContent = 'ENSEMBLE ' + (ENS[S.fx.ens] || 'I');
      if (k === 5) f.lbl.textContent = S.fx.rev ? 'SPRING' : 'TWIN TANK';
      const word = k === 0 ? 'AMT ' : k === 1 ? 'DRIVE ' : 'MIX ';
      let t = on ? word + Math.round(v * 100) + ' %' : 'BYPASS';
      if (on && (k === 3 || k === 4)) { const sy = S.fx[k === 3 ? 'bbdSync' : 'oilSync']; if (sy) t += ' · ' + SYNCN[sy]; }
      f.sub.textContent = t;
    });
  }

  // --------------------------------------------------------------------------- update from state ---
  function updateHeader() {
    if (!S) return;
    const all = presetList();
    const idx = all.findIndex((p) => p.kind === S.preset.kind && p.name.toUpperCase() === S.preset.name.toUpperCase());
    setNix(hdr.num, idx >= 0 ? two(idx + 1) : '__');
    hdr.name.textContent = S.preset.name.toUpperCase();
    hdr.kind.textContent = (S.preset.kind === 'U' ? 'USER' : 'FACTORY') + (S.preset.dirty ? ' · EDITED' : '');
  }
  function updateLayer(i) {
    const Ly = layers[i], ls = S.layers[i];
    const si = srcIn(i), ii = impIn(i);
    Ly.srcPill.setOn(si ? 2 : ls.src.kind === 'sample' ? 0 : ls.src.kind === 'synth' ? 1 : -1);
    Ly.impPill.setOn(ii ? 3 : ls.imp.kind === 'sample' ? 0 : ls.imp.kind === 'synth' ? 1 : ls.imp.kind === 'live' ? 2 : -1);
    Ly.card.classList.toggle('in-src', si);
    Ly.card.classList.toggle('in-imp', ii);
    const ft = Math.round(S.core[LP(i, L.FTYPE)]);
    Ly.ftype.innerHTML = FTYPES[ft] + ' &#9662;';
    Ly.jewel.classList.toggle('on', si || ls.src.kind !== 'off');
    updateInputLabels(i);
  }
  function updateViews() {
    if (!ST || !ST.views) return;
    ST.views.forEach((v, i) => {
      const Ly = layers[i];
      if (!Ly || !v) return;
      Ly.srcName.textContent = v.srcName || '—';
      Ly.srcSub.textContent = v.srcSub || '';
      Ly.srcSub.classList.toggle('warn', !!v.srcError);
      Ly.impName.textContent = v.impName || '—';
      Ly.impSub.textContent = v.impSub || '';
      Ly.impSub.classList.toggle('warn', !!v.impError);
      const sp = v.srcPeaks || [];
      if (v.srcTrace) { Ly.srcPath.setAttribute('d', tracePath(sp, 160, 80)); Ly.srcPath.setAttribute('fill', 'none'); Ly.srcPath.setAttribute('stroke', '#ffb347'); Ly.srcPath.setAttribute('stroke-width', 1.6); Ly.srcLine.style.display = 'none'; }
      else { Ly.srcPath.setAttribute('d', sp.length ? wavePath(sp, 160, 80) : ''); Ly.srcPath.setAttribute('fill', '#ffb347'); Ly.srcPath.setAttribute('stroke', 'none'); Ly.srcLine.style.display = sp.length ? '' : 'none'; }
      Ly.srcHint.textContent = sp.length ? '' : (S.layers[i].src.kind === 'off' ? 'NO SOURCE' : 'DROP A SAMPLE HERE');
      const ip = v.impPeaks || [];
      if (v.impTrace) { Ly.impPath.setAttribute('d', tracePath(ip, 136, 76)); Ly.impPath.setAttribute('fill', 'none'); Ly.impPath.setAttribute('stroke', '#ffb347'); Ly.impPath.setAttribute('stroke-width', 1.6); }
      else { Ly.impPath.setAttribute('d', ip.length ? wavePath(ip, 136, 76, true) : ''); Ly.impPath.setAttribute('fill', '#ffb347'); Ly.impPath.setAttribute('stroke', 'none'); }
      Ly.impHint.textContent = ip.length ? '' : (S.layers[i].imp.kind === 'off' ? 'NO IMPULSE' : 'DROP AN IMPULSE HERE');
      updateInputLabels(i);
    });
  }
  // a layer whose source / impulse is the live input: its labels and a rolling level trace in its window
  // (the plugin's views may say the same; the settings decide when the views have not caught up)
  function updateInputLabels(i) {
    const Ly = layers[i];
    if (!Ly || !S) return;
    const v = (ST && ST.views && ST.views[i]) || {};
    const si = srcIn(i), ii = impIn(i);
    if (si) {
      Ly.srcName.textContent = 'LIVE INPUT';
      Ly.srcSub.textContent = 'SIDECHAIN · ' + (gateOf(i) ? 'NOTE GATE' : 'THRU');
      Ly.srcSub.classList.remove('warn');
      Ly.srcHint.textContent = inCh === 0 ? 'NO INPUT BUS' : '';
    } else if (Ly.srcName.textContent === 'LIVE INPUT') {
      Ly.srcName.textContent = v.srcName || '—'; Ly.srcSub.textContent = v.srcSub || '';
    }
    if (ii) {
      Ly.impName.textContent = 'LIVE INPUT';
      Ly.impSub.textContent = 'SIDECHAIN · ' + Math.round((inL(i).window || 0.25) * 1000) + ' ms WINDOW';
      Ly.impSub.classList.remove('warn');
      Ly.impHint.textContent = inCh === 0 ? 'NO INPUT BUS' : '';
    } else if (Ly.impName.textContent === 'LIVE INPUT') {
      Ly.impName.textContent = v.impName || '—'; Ly.impSub.textContent = v.impSub || '';
    }
    drawInputTrace(i);
  }
  let inCh = -1;                 // input bus channels from the meter (-1 = not known yet)
  const inHist = [];             // recent input levels (0..1, a dB scale) for the source / impulse windows
  function drawInputTrace(i) {
    const Ly = layers[i];
    if (!Ly) return;
    const pk = [];
    for (let k = 0; k < 60; k++) { const v = inHist[inHist.length - 60 + k] || 0; pk.push(-v, v); }
    if (srcIn(i)) {
      Ly.srcPath.setAttribute('d', wavePath(pk, 160, 80)); Ly.srcPath.setAttribute('fill', '#ffb347'); Ly.srcPath.setAttribute('stroke', 'none');
      Ly.srcLine.style.display = '';
    }
    if (impIn(i)) {
      Ly.impPath.setAttribute('d', wavePath(pk, 136, 76)); Ly.impPath.setAttribute('fill', '#ffb347'); Ly.impPath.setAttribute('stroke', 'none');
    }
  }
  function updateAll() {
    if (!S) return;
    widgets.forEach((w) => w.update());
    for (let i = 0; i < 3; i++) updateLayer(i);
    orb.sync.setOn(S.sync ? 0 : 1);
    orb.dir.setOn(S.core[G.DIR] > 0.5 ? 1 : 0);
    bot.macros.forEach((m, k) => { m.name.textContent = S.macros[k].name; m.n.classList.toggle('on', assigning === k + 1); });
    updateFx();
    updateField();
    updateHeader();
    hdr.drift.classList.toggle('on', HV[hostIdx.get(G.DRIFT)] > 0.001);
    if (inspector) renderInspector();
  }

  // ---------------------------------------------------------------------------------- meters ---
  let lastMeter = null, meterTick = 0, lastMidi = -1, midiFlash = 0;
  function meter(m) {
    if (!m || !S) return;
    lastMeter = m;
    placePuck(m.puck[0], m.puck[1], m.w);
    if (meterTick % 3 === 0) {
      m.w.forEach((w, k) => setNix(orb.wn[k], two(w * 100)));
      orb.blendT.textContent = `BLEND · PUCK X ${m.puck[0].toFixed(2)} Y ${m.puck[1].toFixed(2)}`;
    }
    if (meterTick % 10 === 0)
      hdr.stat.innerHTML = `VOICES ${String(m.voices).padStart(2, '0')}/48<br>CPU ${Math.round(m.cpu * 100)} %<br>${(m.rate / 1000).toFixed(m.rate % 1000 ? 1 : 0)} kHz · ${m.latencyMs.toFixed(1)} ms`;
    meterTick++;
    // VU: 0 VU = -14 dBFS RMS, -20 .. +3 VU across the scale
    const rms = Math.max(m.vu[0], m.vu[1]), db = 20 * Math.log10(Math.max(rms, 1e-6)) + 14;
    const target = -50 + clamp((db + 20) / 23, 0, 1) * 100;
    bot.needleAng += (target - bot.needleAng) * 0.45;
    bot.needle.setAttribute('transform', `rotate(${bot.needleAng.toFixed(1)} 35 52)`);
    bot.peak.classList.toggle('on', Math.max(m.pk[0], m.pk[1]) > 0.89);
    // input (sidechain): -60..0 dBFS bars, peak lamp, the bus as the plugin reports it
    if (m.in || m.inCh != null) {
      const a = m.in || [0, 0, 0], ch = m.inCh == null ? 2 : m.inCh;
      const lv = (x) => clamp((20 * Math.log10(Math.max(x, 1e-6)) + 60) / 60, 0, 1);
      bot.inBars.forEach((b, c) => { b.firstChild.style.transform = `scaleY(${(ch ? lv(a[c] || 0) : 0).toFixed(3)})`; });
      bot.inPeak.classList.toggle('on', ch > 0 && (a[2] || 0) > 0.89);
      if (ch !== inCh) {
        inCh = ch;
        bot.inNone.style.display = ch ? 'none' : '';
        bot.inBars.forEach((b, c) => { b.style.display = ch === 0 || (ch === 1 && c === 1) ? 'none' : ''; });
        for (let i = 0; i < 3; i++) updateInputLabels(i);
      }
      if (meterTick % 10 === 1) bot.inSub.textContent = ch === 0 ? 'NO INPUT BUS' : (ch === 1 ? 'MONO' : 'STEREO') + ' \u00b7 ' + (20 * Math.log10(Math.max(a[0], a[1] || 0, 1e-6))).toFixed(0).replace('-120', '-inf') + ' dB';
      inHist.push(ch ? lv(Math.max(a[0], a[1] || 0)) : 0); if (inHist.length > 120) inHist.splice(0, inHist.length - 120);
      if (meterTick % 2 === 0) for (let i = 0; i < 3; i++) if (srcIn(i) || impIn(i)) drawInputTrace(i);
    }
    if (m.midi !== lastMidi) { if (lastMidi >= 0) midiFlash = 4; lastMidi = m.midi; }
    hdr.midi.classList.toggle('on', midiFlash > 0 || m.voices > 0);
    if (midiFlash > 0) midiFlash--;
  }

  // ------------------------------------------------------------------------------- sources etc ---
  function setInputFlag(i, which, on) {
    inL(i)[which] = on;
    hostSet(`orbit_l${i + 1}_${which}_input`, on ? 1 : 0);
  }
  function setSourceKind(i, kind) {
    const s = S.layers[i].src;
    if (kind === 'input') {
      if (srcIn(i)) { openInspector(i, 'src'); return; }
      setInputFlag(i, 'src', true);
      markEdited(); updateLayer(i); openInspector(i, 'src');
      return;
    }
    const wasIn = srcIn(i);
    if (wasIn) setInputFlag(i, 'src', false);
    if (s.kind === kind && !wasIn) { openInspector(i, 'src'); return; }
    s.kind = kind;
    if (kind === 'sample' && !(s.file && s.file.name) && s.from !== 'print') s.from = 'file';
    send('o.src', [i, { kind, from: s.from }]);
    markEdited(); updateLayer(i);
    openInspector(i, 'src');
  }
  function setImpulseKind(i, kind) {
    const s = S.layers[i].imp;
    if (kind === 'input') {
      if (impIn(i)) { openInspector(i, 'imp'); return; }
      setInputFlag(i, 'imp', true);
      markEdited(); updateLayer(i); openInspector(i, 'imp');
      return;
    }
    const wasIn = impIn(i);
    if (wasIn) setInputFlag(i, 'imp', false);
    if (s.kind === kind && !wasIn) { openInspector(i, 'imp'); return; }
    s.kind = kind;
    if (kind === 'live' && s.live === i) s.live = (i + 1) % 3;
    send('o.imp', [i, { kind, from: s.from, live: s.live }]);
    markEdited(); updateLayer(i);
    openInspector(i, 'imp');
  }
  function dropTarget(el, i, which) {
    el.addEventListener('dragover', (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; el.classList.add('drop'); });
    el.addEventListener('dragleave', () => el.classList.remove('drop'));
    el.addEventListener('drop', (e) => {
      e.preventDefault();
      el.classList.remove('drop');
      const f = e.dataTransfer.files && e.dataTransfer.files[0];
      if (!f) return;
      if (!/\.(wav|wave|aif|aiff|flac)$/i.test(f.name)) { toast('WAV, AIFF OR FLAC PLEASE'); return; }
      if (f.size > 200 * 1024 * 1024) { toast('THAT FILE IS TOO LARGE (200 MB MAX)'); return; }
      toast('LOADING ' + f.name.toUpperCase());
      sendFile(i, which, f);
    });
  }
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());
  function sendFile(i, which, f) {
    const rd = new FileReader();
    rd.onload = () => {
      const bytes = new Uint8Array(rd.result), CH = 512 * 1024;
      send('o.dropBegin', [i, which, f.name]);
      for (let o = 0; o < bytes.length; o += CH) {
        let s = '';
        const part = bytes.subarray(o, Math.min(bytes.length, o + CH));
        for (let k = 0; k < part.length; k += 8192) s += String.fromCharCode.apply(null, part.subarray(k, k + 8192));
        send('o.dropChunk', [btoa(s)]);
      }
      send('o.dropEnd', []);
    };
    rd.onerror = () => toast('COULD NOT READ ' + f.name);
    rd.readAsArrayBuffer(f);
  }

  // ---------------------------------------------------------------------------------- overlays ---
  function overlay(id, x, y, w, h) {
    let o = document.getElementById(id);
    if (!o) {
      o = H(`<div id="${id}" class="ovl hidden"></div>`, document.getElementById('stage'));
      o.addEventListener('pointerdown', (e) => e.stopPropagation());
      o.addEventListener('wheel', (e) => e.stopPropagation());
    }
    o.style.cssText = px(x, y, w, h);
    o.classList.remove('hidden');
    return o;
  }
  function closeOverlays() { document.querySelectorAll('.ovl').forEach((o) => o.classList.add('hidden')); inspector = null; }
  document.addEventListener('pointerdown', (e) => { if (!e.target.closest('.ovl, .win.wave, .clk, .pill, .hbtn, .arrow, .nix.clk')) closeOverlays(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') { closeOverlays(); setAssign(0); } });

  function list(container, items, onPick, opts) {
    opts = opts || {};
    const l = H(`<div class="list"></div>`, container);
    if (opts.maxH) l.style.maxHeight = opts.maxH + 'px';
    items.forEach((it, k) => {
      const d = H(`<div class="${it.on ? 'on' : ''}"><span>${esc(it.name)}</span>${it.k ? `<span class="k">${esc(it.k)}</span>` : ''}${it.del ? '<span class="del" title="delete">×</span>' : ''}</div>`, l);
      d.addEventListener('pointerdown', (e) => { e.preventDefault(); if (e.target.classList.contains('del')) { opts.onDelete && opts.onDelete(it, k); return; } onPick(it, k); });
    });
    const on = l.querySelector('.on'); if (on) setTimeout(() => { l.scrollTop = on.offsetTop - 60; }, 0);
    return l;
  }
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  function btn(container, text, on, f, cls) {
    const b = H(`<span class="btn ${on ? 'on' : ''} ${cls || ''}">${text}</span>`, container);
    b.addEventListener('pointerdown', (e) => { e.preventDefault(); f(); });
    return b;
  }
  // a small knob in an overlay. bind as Knob's; commit() for spec values that rebuild on release
  function mini(container, label, bind) {
    const m = H(`<div class="mini"><div class="lbl">${label}</div><div class="sub">—</div></div>`, container);
    const k = Knob(m, 15, 2, 36, {}, bind, m.querySelector('.sub'));
    widgets.pop();          // overlay knobs live as long as the overlay (re-rendered), not in the panel's list
    k.update();
    return k;
  }
  // a spec field as a knob: shown while dragging, sent on release (the worker rebuilds the impulse)
  function specBind(i, which, key, min, max, fmt, curve) {
    const sp = () => S.layers[i][which];
    const toN = (v) => curve === 'sq' ? Math.sqrt(clamp((v - min) / (max - min), 0, 1)) : clamp((v - min) / (max - min), 0, 1);
    const fromN = (n) => curve === 'sq' ? min + (max - min) * n * n : min + (max - min) * n;
    return {
      target: null,
      get() { return toN(sp()[key]); },
      set(n, fin) { sp()[key] = Math.round(fromN(n) * 1000) / 1000; if (fin) this.commit(); },
      commit() { send(which === 'src' ? 'o.src' : 'o.imp', [i, { [key]: sp()[key] }]); markEdited(); },
      text() { return fmt(sp()[key]); },
      def: toN(({ start: 0, length: 0, pitch: 0, fadeIn: 0, fadeOut: 0.01, tilt: 0, damp: 0, age: 0, gain: 0.8, genSeconds: 2, printSeconds: 1.5 })[key] || 0),
    };
  }
  function toggleCore(container, label, id) {
    return btn(container, label, S.core[id] > 0.5, () => { const v = S.core[id] > 0.5 ? 0 : 1; S.core[id] = v; send('o.set', [id, v]); markEdited(); renderInspector(); });
  }
  function stepper(container, label, get, set) {
    const r = H(`<span class="row" style="margin:0;gap:6px"><span class="note">${label}</span></span>`, container);
    btn(r, '−', false, () => { set(get() - 1); });
    const n = nix(r, 0, 0, '', true); n.style.position = 'relative';
    const show = () => setNix(n, noteName(get()).padEnd(3, ' ').replace(/ /g, '_'));
    show();
    btn(r, '+', false, () => { set(get() + 1); });
    return { show };
  }

  let inspector = null;
  function openInspector(i, which) { inspector = { i, which }; renderInspector(); }
  function renderInspector() {
    if (!inspector || !S) return;
    const { i, which } = inspector;
    const y = 98 + i * (LH + 14);
    const o = overlay('insp', which === 'src' ? 100 : 330, Math.min(y + 20, 470), 600, 400);
    o.innerHTML = '';
    const x = H(`<div class="x">×</div>`, o); x.addEventListener('pointerdown', () => closeOverlays());
    const sp = S.layers[i][which];
    if (which === 'src') {
      H(`<h3>LAYER ${i + 1} · SOURCE</h3>`, o);
      const r = H(`<div class="row"></div>`, o);
      const si = srcIn(i);
      const plain = (kind, from) => { if (si) setInputFlag(i, 'src', false); sp.kind = kind; if (from) sp.from = from; send('o.src', [i, from ? { kind, from } : { kind }]); markEdited(); updateLayer(i); renderInspector(); };
      btn(r, 'OFF', !si && sp.kind === 'off', () => plain('off'));
      btn(r, 'REGOLITH SYNTH', !si && sp.kind === 'synth', () => { setSourceKind(i, 'synth'); renderInspector(); });
      btn(r, 'SAMPLE FILE', !si && sp.kind === 'sample' && sp.from === 'file', () => plain('sample', 'file'));
      btn(r, 'REGOLITH PRINT', !si && sp.kind === 'sample' && sp.from === 'print', () => plain('sample', 'print'));
      btn(r, 'LIVE INPUT', si, () => { setSourceKind(i, 'input'); renderInspector(); });
      const cols = H(`<div class="cols"></div>`, o), c1 = H(`<div></div>`, cols), c2 = H(`<div></div>`, cols);
      if (si) {
        H(`<div class="note">LIVE INPUT: THE PLUGIN\u2019S SIDECHAIN AUDIO IS THIS LAYER\u2019S SOURCE (ABLETON: SIDECHAIN \u201cAUDIO FROM\u201d \u00b7 STANDALONE: THE AUDIO DEVICE INPUT, UNMUTED IN ITS SETTINGS).</div>`, c1);
        const gr = H(`<div class="row"><span class="note">GATE</span></div>`, c1);
        const g = gateOf(i);
        btn(gr, 'THRU', g === 0, () => { hostSet(`orbit_l${i + 1}_input_gate`, 0); inL(i).gate = 0; markEdited(); updateLayer(i); renderInspector(); });
        btn(gr, 'NOTE', g === 1, () => { hostSet(`orbit_l${i + 1}_input_gate`, 1); inL(i).gate = 1; markEdited(); updateLayer(i); renderInspector(); });
        H(`<div class="note">${g ? 'NOTE: THE INPUT SOUNDS WHILE NOTES ARE HELD, SHAPED BY THE LAYER\u2019S AMP ENVELOPE.' : 'THRU: THE INPUT ALWAYS PLAYS; NOTES STILL STEER PITCH.'}</div>`, c1);
        const ms = H(`<div class="minis"></div>`, c2);
        mini(ms, 'TRIM', idBind('orbit_input_trim', (n) => { const d = -24 + 48 * n; return (d > 0.05 ? '+' : '') + d.toFixed(1) + ' dB'; }, 0.5));
        mini(ms, 'PAN', coreBind(LP(i, L.PAN), (v) => (Math.abs(v) < 0.01 ? 'C' : v < 0 ? 'L ' + Math.round(-v * 100) : 'R ' + Math.round(v * 100))));
        if (g) mini(ms, 'VEL', coreBind(LP(i, L.VEL)));
      } else if (sp.kind === 'synth' || (sp.kind === 'sample' && sp.from === 'print')) {
        H(`<div class="note">REGOLITH PRESET${sp.kind === 'sample' ? ' (PRINTED TO A SAMPLE)' : ''}</div>`, c1);
        list(c1, ST.chonk.map((p) => ({ name: p.name.toUpperCase(), k: p.kind === 'U' ? 'USER' : '', on: p.name.toUpperCase() === (sp.patch.name || '').toUpperCase(), p })), (it) => {
          sp.patch = { name: it.p.name, mfp: it.p.mfp || '' };
          send('o.src', [i, { patch: sp.patch }]); markEdited(); renderInspector();
        });
        if (sp.kind === 'sample') {
          const rr = H(`<div class="row"></div>`, c2);
          stepper(rr, 'NOTE', () => sp.printNote, (v) => { sp.printNote = clamp(v, 12, 108); sp.root = sp.printNote; send('o.src', [i, { printNote: sp.printNote, root: sp.root }]); markEdited(); renderInspector(); });
          const ms = H(`<div class="minis"></div>`, c2);
          mini(ms, 'LENGTH', specBind(i, 'src', 'printSeconds', 0.2, 8, secText, 'sq'));
          mini(ms, 'HOLD', specBind(i, 'src', 'printHold', 0, 8, secText, 'sq'));
        }
      } else if (sp.kind === 'sample') {
        const rr = H(`<div class="row"></div>`, c1);
        btn(rr, 'LOAD FILE…', false, () => send('o.pick', [i, 'src']), 'or');
        H(`<div class="note">OR DROP A WAV / AIFF / FLAC ON THE SOURCE WINDOW</div>`, c1);
        const f = sp.file || {};
        if (f.name) H(`<div class="note ${f.missing ? 'warn' : ''}" style="margin-top:8px">${esc(f.name)}<br>${esc(f.path || '')}<br>${f.missing ? 'MISSING: NOT EMBEDDED, NOT FOUND' : (f.embedded ? 'EMBEDDED IN THE PROJECT' : 'TOO LARGE TO EMBED: KEPT BY PATH')} ${f.seconds ? '· ' + secText(f.seconds) : ''}</div>`, c1);
        const r2 = H(`<div class="row"></div>`, c1);
        stepper(r2, 'ROOT', () => sp.root, (v) => { sp.root = clamp(v, 0, 127); send('o.src', [i, { root: sp.root }]); markEdited(); renderInspector(); });
        if (f.detected != null && f.detected >= 0) {
          H(`<span class="note">DETECTED ${noteName(f.detected)} ${(((f.detected - Math.round(f.detected)) * 100) >= 0 ? '+' : '')}${Math.round((f.detected - Math.round(f.detected)) * 100)} c</span>`, r2);
          btn(r2, 'USE', false, () => { sp.root = Math.round(f.detected); send('o.src', [i, { root: sp.root }]); markEdited(); renderInspector(); });
        }
      }
      if (sp.kind !== 'off' && !si) {
        const ms = H(`<div class="minis"></div>`, c2);
        mini(ms, 'TUNE', coreBind(LP(i, L.TUNE), (v) => (v > 0 ? '+' : '') + Math.round(v) + ' st'));
        mini(ms, 'FINE', coreBind(LP(i, L.FINE), (v) => (v > 0 ? '+' : '') + Math.round(v) + ' c'));
        mini(ms, 'PAN', coreBind(LP(i, L.PAN), (v) => (Math.abs(v) < 0.01 ? 'C' : v < 0 ? 'L ' + Math.round(-v * 100) : 'R ' + Math.round(v * 100))));
        mini(ms, 'VEL', coreBind(LP(i, L.VEL)));
        if (sp.kind === 'sample') {
          mini(ms, 'START', coreBind(LP(i, L.START)));
          mini(ms, 'VEL>START', coreBind(LP(i, L.VEL_START)));
          mini(ms, 'STRETCH', coreBind(LP(i, L.STRETCH), (v) => v.toFixed(2) + 'x'));
          const t = H(`<div class="row"></div>`, c2);
          toggleCore(t, 'LOOP', LP(i, L.LOOP)); toggleCore(t, 'REVERSE', LP(i, L.REVERSE));
          toggleCore(t, 'KEYTRACK BODY', LP(i, L.KEYTRACK));
          if (S.core[LP(i, L.LOOP)] > 0.5) {
            const m2 = H(`<div class="minis"></div>`, c2);
            mini(m2, 'LOOP IN', coreBind(LP(i, L.LOOP_START))); mini(m2, 'LOOP OUT', coreBind(LP(i, L.LOOP_END))); mini(m2, 'X-FADE', coreBind(LP(i, L.XFADE)));
          }
        }
        const t2 = H(`<div class="row"></div>`, c2);
        toggleCore(t2, 'TAPE', LP(i, L.TAPE));
        if (S.core[LP(i, L.TAPE)] > 0.5) {
          const m3 = H(`<div class="minis"></div>`, c2);
          mini(m3, 'WOW', coreBind(LP(i, L.WOW))); mini(m3, 'FLUTTER', coreBind(LP(i, L.FLUTTER)));
          mini(m3, 'BUMP', coreBind(LP(i, L.BUMP))); mini(m3, 'HISS', coreBind(LP(i, L.HISS)));
        }
      }
    } else {
      H(`<h3>LAYER ${i + 1} · IMPULSE</h3>`, o);
      const r = H(`<div class="row"></div>`, o);
      const ii = impIn(i);
      const setK = (kind, from) => { if (ii) setInputFlag(i, 'imp', false); sp.kind = kind; if (from) sp.from = from; send('o.imp', [i, { kind, from: sp.from, live: sp.live }]); markEdited(); updateLayer(i); renderInspector(); };
      btn(r, 'OFF', !ii && sp.kind === 'off', () => setK('off'));
      btn(r, 'FACTORY', !ii && sp.kind === 'sample' && sp.from === 'generated', () => setK('sample', 'generated'));
      btn(r, 'SAMPLE FILE', !ii && sp.kind === 'sample' && sp.from === 'file', () => setK('sample', 'file'));
      btn(r, 'REGOLITH PRINT', !ii && sp.kind === 'synth', () => setK('synth'));
      btn(r, 'LIVE LAYER', !ii && sp.kind === 'live', () => { if (sp.live === i) sp.live = (i + 1) % 3; setK('live'); });
      btn(r, 'LIVE INPUT', ii, () => { setImpulseKind(i, 'input'); renderInspector(); });
      const cols = H(`<div class="cols"></div>`, o), c1 = H(`<div></div>`, cols), c2 = H(`<div></div>`, cols);
      if (ii) {
        H(`<div class="note">LIVE INPUT: THE SIDECHAIN AUDIO IS THIS LAYER\u2019S IMPULSE. BODY CONVOLVES WITH THE LAST WINDOW OF INPUT (REFRESHED CONTINUOUSLY, CROSS-FADED); TONE \u00b7 GRAIN \u00b7 MOTION \u00b7 PITCH FOLLOW THE INPUT AS IT PLAYS.</div>`, c1);
        const wr = H(`<div class="row"><span class="note">WINDOW</span></div>`, c1);
        const cur = inL(i).window || 0.25;
        [0.1, 0.25, 0.5, 1.0].forEach((w) => btn(wr, Math.round(w * 1000) + ' MS', Math.abs(cur - w) < 1e-3, () => {
          inL(i).window = w; send('o.window', [i, w]); markEdited(); updateLayer(i); renderInspector();
        }));
        const ms = H(`<div class="minis"></div>`, c2);
        mini(ms, 'TRIM', idBind('orbit_input_trim', (n) => { const d = -24 + 48 * n; return (d > 0.05 ? '+' : '') + d.toFixed(1) + ' dB'; }, 0.5));
      } else if (sp.kind === 'sample' && sp.from === 'generated') {
        H(`<div class="note">FACTORY IMPULSES (GENERATED, LICENCE-CLEAN)</div>`, c1);
        list(c1, ST.gens.map((g, k) => ({ name: g, on: k === sp.gen, k: k })), (it) => { sp.gen = it.k; send('o.imp', [i, { gen: sp.gen }]); markEdited(); renderInspector(); });
        const rr = H(`<div class="row"></div>`, c1);
        btn(rr, 'VARY', false, () => { sp.seed = (sp.seed * 7 + 13) % 100000; send('o.imp', [i, { seed: sp.seed }]); markEdited(); });
        const ms = H(`<div class="minis"></div>`, rr);
        mini(ms, 'SIZE', specBind(i, 'imp', 'genSeconds', 0.1, 6, secText, 'sq'));
      } else if (sp.kind === 'sample') {
        const rr = H(`<div class="row"></div>`, c1);
        btn(rr, 'LOAD FILE…', false, () => send('o.pick', [i, 'imp']), 'or');
        H(`<div class="note">OR DROP A WAV / AIFF / FLAC ON THE IMPULSE WINDOW</div>`, c1);
        const f = sp.file || {};
        if (f.name) H(`<div class="note ${f.missing ? 'warn' : ''}" style="margin-top:8px">${esc(f.name)}<br>${esc(f.path || '')}<br>${f.missing ? 'MISSING: NOT EMBEDDED, NOT FOUND' : (f.embedded ? 'EMBEDDED IN THE PROJECT' : 'KEPT BY PATH')}</div>`, c1);
        const r2 = H(`<div class="row"></div>`, c1);
        stepper(r2, 'ROOT', () => sp.root, (v) => { sp.root = clamp(v, 0, 127); send('o.imp', [i, { root: sp.root }]); markEdited(); renderInspector(); });
        if (f.detected != null && f.detected >= 0) btn(r2, 'DETECTED ' + noteName(f.detected), false, () => { sp.root = Math.round(f.detected); send('o.imp', [i, { root: sp.root }]); markEdited(); renderInspector(); });
      } else if (sp.kind === 'synth') {
        H(`<div class="note">REGOLITH PRESET (PRINTED AS THE IMPULSE)</div>`, c1);
        list(c1, ST.chonk.map((p) => ({ name: p.name.toUpperCase(), k: p.kind === 'U' ? 'USER' : '', on: p.name.toUpperCase() === (sp.patch.name || '').toUpperCase(), p })), (it) => {
          sp.patch = { name: it.p.name, mfp: it.p.mfp || '' }; send('o.imp', [i, { patch: sp.patch }]); markEdited(); renderInspector();
        });
        const rr = H(`<div class="row"></div>`, c2);
        stepper(rr, 'NOTE', () => sp.printNote, (v) => { sp.printNote = clamp(v, 12, 108); sp.root = sp.printNote; send('o.imp', [i, { printNote: sp.printNote, root: sp.root }]); markEdited(); renderInspector(); });
        const ms = H(`<div class="minis"></div>`, c2);
        mini(ms, 'LENGTH', specBind(i, 'imp', 'printSeconds', 0.1, 6, secText, 'sq'));
        mini(ms, 'HOLD', specBind(i, 'imp', 'printHold', 0, 6, secText, 'sq'));
      } else if (sp.kind === 'live') {
        H(`<div class="note">LIVE: THIS LAYER WEARS ANOTHER LAYER'S RUNNING SOURCE (TONE · GRAIN · MOTION · PITCH). BODY NEEDS A FIXED IMPULSE.</div>`, c1);
        const rr = H(`<div class="row"></div>`, c1);
        [0, 1, 2].filter((k) => k !== i).forEach((k) => btn(rr, 'LAYER ' + (k + 1), sp.live === k, () => { sp.live = k; send('o.imp', [i, { live: k }]); markEdited(); renderInspector(); }));
      }
      if (!ii && (sp.kind === 'sample' || sp.kind === 'synth')) {
        H(`<div class="note" style="margin-top:6px">SHAPE</div>`, c2);
        const ms = H(`<div class="minis"></div>`, c2);
        mini(ms, 'START', specBind(i, 'imp', 'start', 0, 4, secText, 'sq'));
        mini(ms, 'LENGTH', specBind(i, 'imp', 'length', 0, 6, (v) => (v <= 0 ? 'ALL' : secText(v)), 'sq'));
        mini(ms, 'PITCH', specBind(i, 'imp', 'pitch', -24, 24, (v) => (v > 0 ? '+' : '') + v.toFixed(1) + ' st'));
        mini(ms, 'DAMP', specBind(i, 'imp', 'damp', 0, 1, pct));
        mini(ms, 'DECAY', specBind(i, 'imp', 'tilt', -1, 1, (v) => (v > 0 ? 'SHORT ' : v < 0 ? 'LONG ' : '') + Math.round(Math.abs(v) * 100) + '%'));
        mini(ms, 'AGE', specBind(i, 'imp', 'age', 0, 1, pct));
        mini(ms, 'BODY GAIN', specBind(i, 'imp', 'gain', 0, 3, (v) => dbText(v)));
        const t = H(`<div class="row"></div>`, c2);
        btn(t, 'REVERSE', !!sp.reverse, () => { sp.reverse = !sp.reverse; send('o.imp', [i, { reverse: sp.reverse }]); markEdited(); renderInspector(); });
        toggleCore(t, 'KEYTRACK', LP(i, L.KEYTRACK));
        toggleCore(t, 'SCAN LOOP', LP(i, L.SCAN_LOOP));
      }
      if (!ii && sp.kind !== 'off') {
        H(`<div class="note" style="margin-top:6px">STREAM (TONE · GRAIN · MOTION · PITCH)</div>`, c2);
        const m2 = H(`<div class="minis"></div>`, c2);
        mini(m2, 'SCAN RATE', coreBind(LP(i, L.SCAN_RATE), (v) => (v < 0.001 ? 'FREEZE' : v.toFixed(2) + 'x')));
        mini(m2, 'SCAN POS', coreBind(LP(i, L.SCAN_POS)));
        mini(m2, 'IR PITCH', coreBind(LP(i, L.IR_PITCH), (v) => (v > 0 ? '+' : '') + v.toFixed(1) + ' st'));
      }
    }
  }

  function filterMenu(i, anchor) {
    const y = 98 + i * (LH + 14);
    const o = overlay('fmenu', 860, y + 30, 200, 250);
    o.innerHTML = '<h3>FILTER</h3>';
    list(o, FTYPES.map((t, k) => ({ name: t, on: Math.round(S.core[LP(i, L.FTYPE)]) === k })), (it, k) => {
      S.core[LP(i, L.FTYPE)] = k; send('o.set', [LP(i, L.FTYPE), k]); markEdited(); updateLayer(i); closeOverlays();
    }, { maxH: 140 });
    const ms = H(`<div class="minis" style="margin-top:6px"></div>`, o);
    mini(ms, 'DRIVE', coreBind(LP(i, L.FDRIVE)));
    mini(ms, 'ENV AMT', coreBind(LP(i, L.FAMT), (v) => (v > 0 ? '+' : '') + Math.round(v * 100) + '%'));
  }

  function presetList() {
    if (!ST) return [];
    return ST.factory.map((n) => ({ kind: 'F', name: n })).concat((ST.user || []).map((u) => ({ kind: 'U', name: u.name })));
  }
  function loadPreset(p) {
    const all = presetList().filter((q) => q.kind === 'F');
    send('o.preset', p.kind === 'F' ? ['F', all.findIndex((q) => q.name === p.name)] : ['U', p.name]);
  }
  function stepPreset(d) {
    const all = presetList();
    if (!all.length) return;
    let idx = all.findIndex((p) => p.kind === S.preset.kind && p.name.toUpperCase() === S.preset.name.toUpperCase());
    idx = idx < 0 ? 0 : (idx + d + all.length) % all.length;
    loadPreset(all[idx]);
  }
  function openBrowser() {
    const o = overlay('browse', 556, 70, 470, 330);
    o.innerHTML = '';
    const x = H(`<div class="x">×</div>`, o); x.addEventListener('pointerdown', () => closeOverlays());
    H(`<h3>ORBIT PRESETS</h3>`, o);
    const cols = H(`<div class="cols"></div>`, o), c1 = H(`<div><div class="note">FACTORY</div></div>`, cols), c2 = H(`<div><div class="note">USER</div></div>`, cols);
    list(c1, ST.factory.map((n, k) => ({ name: n, k: two(k + 1).replace('_', '0'), on: S.preset.kind === 'F' && S.preset.name === n })), (it) => { loadPreset({ kind: 'F', name: it.name }); closeOverlays(); }, { maxH: 250 });
    if ((ST.user || []).length)
      list(c2, ST.user.map((u) => ({ name: u.name, on: S.preset.kind === 'U' && S.preset.name === u.name, del: true })), (it) => { loadPreset({ kind: 'U', name: it.name }); closeOverlays(); },
        { maxH: 250, onDelete: (it) => { send('o.delete', [it.name]); } });
    else H(`<div class="note" style="margin-top:8px">NO USER PRESETS YET: SAVE ONE.</div>`, c2);
  }
  function openSave() {
    const o = overlay('save', 556, 70, 470, 150);
    o.innerHTML = '';
    const x = H(`<div class="x">×</div>`, o); x.addEventListener('pointerdown', () => closeOverlays());
    H(`<h3>SAVE ORBIT PRESET</h3>`, o);
    const r = H(`<div class="row"></div>`, o);
    const inp = H(`<input class="txt" maxlength="40" spellcheck="false">`, r);
    inp.value = S.preset.name;
    const go = () => { const n = inp.value.trim(); if (!n) return; send('o.save', [n]); closeOverlays(); toast('SAVED ' + n.toUpperCase()); };
    btn(r, 'SAVE', false, go, 'or');
    inp.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') go(); if (e.key === 'Escape') closeOverlays(); });
    H(`<div class="note">%APPDATA%\\ATOMIC LOUNGE\\ORBIT · SAMPLES EMBEDDED UP TO 24 MB EACH</div>`, o);
    setTimeout(() => { inp.focus(); inp.select(); }, 0);
  }
  function renameMacro(k) {
    const o = overlay('rename', 1000, 640, 360, 110);
    o.innerHTML = `<h3>MACRO ${k + 1} NAME</h3>`;
    const r = H(`<div class="row"></div>`, o);
    const inp = H(`<input class="txt" maxlength="14" spellcheck="false" style="width:200px">`, r);
    inp.value = S.macros[k].name;
    const go = () => { S.macros[k].name = inp.value.trim().toUpperCase() || 'MACRO ' + (k + 1); send('o.macroName', [k, S.macros[k].name]); closeOverlays(); updateAll(); };
    btn(r, 'OK', false, go, 'or');
    inp.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') go(); if (e.key === 'Escape') closeOverlays(); });
    setTimeout(() => { inp.focus(); inp.select(); }, 0);
  }
  function setAssign(src) {
    assigning = src;
    const bar = document.getElementById('assignBar');
    bar.textContent = src ? `ASSIGN ${MSRC[src]}: CLICK A KNOB, SLIDER OR NODE · ESC TO CANCEL` : '';
    bar.classList.toggle('hidden', !src);
    document.querySelectorAll('svg.k, .slot').forEach((el) => {
      const w = el.__w;
      el.classList.toggle('assignable', !!src && !!w && w.bind && w.bind.target != null);
    });
    if (bot.macros) bot.macros.forEach((m, k) => m.n.style.outline = src === k + 1 ? '2px solid #e2681b' : '');
  }
  function openAssignList() {
    const o = overlay('assign', 900, 330, 540, 400);
    o.innerHTML = '';
    const x = H(`<div class="x">×</div>`, o); x.addEventListener('pointerdown', () => closeOverlays());
    H(`<h3>MODULATION</h3>`, o);
    H(`<div class="note">PICK A SOURCE, THEN CLICK A CONTROL ON THE PANEL. DRAG AN AMOUNT; × REMOVES.</div>`, o);
    const r = H(`<div class="row"></div>`, o);
    [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14, 15, 18, 19, 20].forEach((s) => btn(r, MSRC[s].replace('EXT', 'X'), false, () => { closeOverlays(); setAssign(s); }));
    const l = H(`<div class="list" style="max-height:170px"></div>`, o);
    const slots = S.mods || [];
    if (!slots.length) H(`<div class="note" style="padding:8px">NO MODULATION YET</div>`, l);
    slots.forEach((m) => {
      const [slot, src, dst, amt] = m;
      const row = H(`<div class="modrow"><span class="n">${esc(MSRC[src] || '?')} → ${esc(coreName(dst))}</span><span class="amt"><i></i></span><span class="v"></span><span class="del" style="cursor:pointer">×</span></div>`, l);
      const bar = row.querySelector('.amt'), fill = bar.querySelector('i'), val = row.querySelector('.v');
      const show = () => { const a = m[3]; fill.style.left = (50 + Math.min(0, a) * 50) + '%'; fill.style.width = Math.abs(a) * 50 + '%'; val.textContent = (a > 0 ? '+' : '') + Math.round(a * 100) + '%'; };
      show();
      let on = false;
      const setX = (e) => { const rc = bar.getBoundingClientRect(); m[3] = Math.round(clamp(((e.clientX - rc.left) / rc.width) * 2 - 1, -1, 1) * 100) / 100; show(); send('o.mod', [slot, src, dst, m[3]]); markEdited(); };
      bar.addEventListener('pointerdown', (e) => { e.preventDefault(); bar.setPointerCapture(e.pointerId); on = true; setX(e); });
      bar.addEventListener('pointermove', (e) => { if (on) setX(e); });
      bar.addEventListener('pointerup', () => { on = false; });
      row.querySelector('.del').addEventListener('pointerdown', (e) => { e.preventDefault(); send('o.unassign', [slot]); });
    });
  }

  let toastT = 0;
  function toast(t) {
    const e = document.getElementById('toast');
    if (!e) return;
    e.textContent = String(t).toUpperCase();
    e.classList.remove('hidden');
    clearTimeout(toastT);
    toastT = setTimeout(() => e.classList.add('hidden'), 2600);
  }

  // ----------------------------------------------------------------------------------- sizing ---
  function fit() {
    const st = document.getElementById('stage');
    scale = Math.min(window.innerWidth / 1600, window.innerHeight / 1000);
    const ox = (window.innerWidth - 1600 * scale) / 2, oy = (window.innerHeight - 1000 * scale) / 2;
    st.style.transform = `translate(${ox}px, ${oy}px) scale(${scale})`;
  }

  // ------------------------------------------------------------------------- called by the plugin ---
  window.ORB = {
    state(st) {
      if (!st || !st.settings) return;
      const reopen = inspector;
      ST = st; S = st.settings; HV = st.hostValues.slice(); CORE = st.core;
      hostIdx.clear(); hostId.clear();
      st.host.forEach((h, i) => { hostIdx.set(h.target, i); hostId.set(h.id, i); });
      if (!S.shape) S.shape = S.path || 1;
      if (!body.__built) { build(); body.__built = true; document.querySelectorAll('canvas.wood').forEach((c, i) => wood(c, +(c.dataset.seed || 1234 + i * 777))); }
      S.mods = S.mods || [];
      updateAll();
      updateViews();
      if (st.meter) meter(st.meter);
      if (st.problems && st.problems.length) toast(st.problems.join(' · '));
      if (reopen) { inspector = reopen; renderInspector(); }
      document.documentElement.dataset.ready = '1';
    },
    values(map) {
      if (!S) return;
      let layersMoved = false;
      for (const k in map) {
        HV[+k] = map[k];
        const h = ST.host[+k];
        if (!h) continue;
        if (h.id === 'orbit_motion') { S.path = map[k] > 0.5 ? shapeOf() : 0; orb.trailPts.length = 0; }
        if (/^orbit_l\d_(src_input|imp_input|input_gate)$/.test(h.id)) layersMoved = true;
      }
      widgets.forEach((w) => w.update());
      if (layersMoved) { for (let i = 0; i < 3; i++) updateLayer(i); if (inspector) renderInspector(); }
      updateField();
      hdr.drift.classList.toggle('on', HV[hostIdx.get(G.DRIFT)] > 0.001);
    },
    views(v) { if (ST) { ST.views = v; updateViews(); } },
    meter,
    toast,
    // tests
    _debug() { return { S, HV, ST, assigning, scale }; },
  };

  window.addEventListener('resize', fit);
  document.addEventListener('DOMContentLoaded', () => { fit(); body = document.getElementById('body'); send('o.hello', []); });
  if (document.readyState !== 'loading') { fit(); body = document.getElementById('body'); send('o.hello', []); }
})();
