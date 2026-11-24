// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk Android UI: the panel's parameter model (no DOM). Every control on the panel and in the
// MOD overlay is a parameter here: get(patch) -> display value, set(patch, value) writes the patch
// fields of the mf_core patch object (editor/src/mf_core.js defaultPatch / mfpWrite / mfpRead), the
// same struct as dsp/include/mfe.h mfe_patch_t. node loads this with require(); the page gets the
// global MFPanel. tests/test_panel.mjs checks the mappings.
'use strict';
(function (root, factory) {
  const m = factory();
  if (typeof module === 'object' && module.exports) module.exports = m;
  else root.MFPanel = m;
})(typeof self !== 'undefined' ? self : this, function () {
  // ------------------------------------------------------------ enums (mfe.h) ---
  const WT_MOOG = 6;
  const FILTER_ENUM = { OFF: 0, LP2: 1, LP4: 2, BP: 3, HP: 4, NOTCH: 5, LADDER: 6, ACID: 7 };
  const FILTER_SELECT = ['LADDER', 'ACID', 'LP4', 'LP2', 'HP', 'BP', 'NOTCH'];   // panel selector order
  const OSC_ON = 1, OSC_RESET = 2, OSC_LO = 4;
  const LFO_KEYSYNC = 1, LFO_UNIPOLAR = 2, LFO_SYNC = 4;
  const W = { SINE: 0, TRI: 1, SAW_UP: 2, SAW_DOWN: 3, SQUARE: 4, SH: 5, PERFORMER: 6, STEPPER: 7 };
  const LFO_WAVE_NAMES = ['SINE', 'TRIANGLE', 'SAW UP', 'SAW DOWN', 'SQUARE', 'S & H'];
  const SRC = { NONE: 0, ENV1: 1, ENV2: 2, ENV3: 3, ENV4: 4, LFO1: 5, LFO2: 6, LFO3: 7, LFO4: 8, VEL: 9, KEY: 10,
    MODWHEEL: 11, BEND: 12, AFTERTOUCH: 13, RANDOM: 22, ONE: 23, OSC3: 24 };
  const DST_NAMES = ['NONE', 'PITCH', 'OSC1_PITCH', 'OSC2_PITCH', 'OSC3_PITCH', 'OSC1_POS', 'OSC2_POS', 'OSC3_POS',
    'OSC1_WARP', 'OSC2_WARP', 'OSC3_WARP', 'OSC1_AMP', 'OSC2_AMP', 'OSC3_AMP', 'NOISE_AMP', 'F1_CUTOFF', 'F2_CUTOFF',
    'F1_RES', 'F2_RES', 'SERPAR', 'DRIVE', 'AMP', 'PAN', 'LFO1_RATE', 'LFO2_RATE'];
  const DST = {}; DST_NAMES.forEach((n, i) => { DST[n] = i; });
  const DST_LABELS = ['OFF', 'PITCH', 'OSC 1 PITCH', 'OSC 2 PITCH', 'OSC 3 PITCH', 'OSC 1 WAVE', 'OSC 2 WAVE', 'OSC 3 WAVE',
    'OSC 1 WARP', 'OSC 2 WARP', 'OSC 3 WARP', 'OSC 1 LEVEL', 'OSC 2 LEVEL', 'OSC 3 LEVEL', 'NOISE LEVEL', 'CUTOFF',
    'CUTOFF 2', 'EMPHASIS', 'EMPHASIS 2', 'SER/PAR', 'DRIVE', 'LOUDNESS', 'PAN', 'LFO 1 RATE', 'LFO 2 RATE'];

  // Model D-style footage: RANGE positions (index 0 = LO, the oscillator becomes an LFO)
  const RANGE_LABELS = ['LO', "32'", "16'", "8'", "4'", "2'"];
  const RANGE_SEMIS = [0, -24, -12, 0, 12, 24];
  const WAVE_NAMES = ['TRIANGLE', 'SHARK', 'REV SAW', 'SAWTOOTH', 'SQUARE', 'WIDE PULSE', 'NARROW PULSE', 'NARROWEST'];
  const WAVETABLE_NAMES = ['BASIC', 'PWM', 'SYNC', 'FORMANT', 'ORGAN', 'SQUASH', 'CLASSIC'];
  const VOICE_MODES = ['POLY', 'MONO', 'LEGATO'];
  // tempo divisions (dsp mfe_sync_beats: beats per cycle or step) as note values
  const SYNC_LABELS = ['1/32', '1/16T', '1/16', '1/8T', '1/16.', '1/8', '1/4T', '1/8.', '1/4', '1/4.', '1/2', '1 BAR', '2 BAR'];
  // Performer shapes: names and curves exactly as dsp/mfe_tables.c perf_shape()
  const PERF_NAMES = ['RAMP UP', 'RAMP DOWN', 'EXP UP', 'EXP DOWN', 'LOG UP', 'HOLD FALL', 'S UP', 'S DOWN', 'HIGH', 'SQUARE',
    'TRIANGLE', 'BUMP', 'DECAY', 'DECAY X2', 'DECAY X3', 'DECAY X4', 'ZERO', 'FULL', 'HALF SINE', 'SINE', 'BUMP', 'STAIRS UP',
    'STAIRS DN', 'SHORT GATE', 'RAMP X2', 'RAMP DN X2', 'TRI X2', 'WOBBLE', 'BOUNCE', 'LATE RISE', 'GATE 3/4', 'SPIKE'];
  function perfShape(id, x) {
    const PI = Math.PI, e4 = Math.exp(4) - 1; let f;
    switch (id) {
      case 0: return x; case 1: return 1 - x; case 2: f = x * x; return f * f;
      case 3: return (Math.exp(4 * (1 - x)) - 1) / e4; case 4: f = 1 - x; return 1 - f * f * f;
      case 5: return 1 - x * x * x; case 6: return x * x * (3 - 2 * x); case 7: return 1 - x * x * (3 - 2 * x);
      case 8: return 1; case 9: return x < 0.5 ? 1 : 0; case 10: return 1 - Math.abs(2 * x - 1);
      case 11: return 0.5 - 0.5 * Math.cos(2 * PI * x);
      case 12: case 13: case 14: case 15: { let u = x * (id - 11); u -= Math.floor(u); return (1 - u) * (1 - u); }
      case 16: return 0; case 17: return 1; case 18: return Math.sin(PI * x); case 19: return 0.5 + 0.5 * Math.sin(2 * PI * x);
      case 20: return 0.5 - 0.5 * Math.cos(2 * PI * x);
      case 21: f = Math.floor(x * 4); return Math.min(f, 3) / 3; case 22: f = Math.floor(x * 4); return 1 - Math.min(f, 3) / 3;
      case 23: return x < 0.15 ? 1 : 0; case 24: f = 2 * x; return f - Math.floor(f); case 25: f = 2 * x; return 1 - (f - Math.floor(f));
      case 26: f = 2 * x; f -= Math.floor(f); return 1 - Math.abs(2 * f - 1); case 27: return 0.5 + 0.5 * Math.sin(4 * PI * x);
      case 28: return Math.abs(Math.sin(3 * PI * x)) * (1 - x); case 29: f = x * x; return f * f;
      case 30: return x < 0.75 ? 1 : 0; case 31: return x < 0.1 ? x / 0.1 : Math.exp(-8 * (x - 0.1));
    }
    return 0;
  }

  // ----------------------------------------------------------------- scalings ---
  const clamp = (x, lo, hi) => (x < lo ? lo : x > hi ? hi : x);
  const q15 = (x) => clamp(Math.round(x * 32767), -32768, 32767);
  const uq = (v) => clamp(v / 32767, -1, 1);
  const envSec = (q) => 0.0005 * Math.pow(60000, clamp(q, 0, 1));          // MFE_TIME_RANGE
  const secToEnv = (s) => clamp(Math.log(s / 0.0005) / Math.log(60000), 0, 1);
  const cutoffHz = (c) => 440 * Math.pow(2, (c * 136 - 69) / 12);          // cutoff = MIDI note / 136
  const hzToCutoff = (hz) => clamp((69 + 12 * Math.log2(hz / 440)) / 136, 0, 1);
  const lfoHz = (r) => 0.02 * Math.pow(2000, clamp(r, 0, 1));              // 0.02 .. 40 Hz
  const hzToLfo = (hz) => clamp(Math.log(hz / 0.02) / Math.log(2000), 0, 1);
  const fmtTime = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(s < 10 ? 2 : 1)} s`);
  const fmtHz = (f) => (f >= 1000 ? `${(f / 1000).toFixed(f >= 10000 ? 1 : 2)} kHz` : `${f.toFixed(f < 10 ? 2 : f < 100 ? 1 : 0)} Hz`);
  const fmtPct = (x) => `${Math.round(x * 100)}%`;
  const fmtSigned = (x) => `${x >= 0 ? '+' : ''}${Math.round(x * 100)}%`;
  const fmtDial = (x) => (x * 10).toFixed(1);                               // the 0..10 panel scale

  // ---------------------------------------------------------- matrix routing ---
  // One (src, dst) pair = one matrix slot; amount 0 frees the slot.
  function findSlot(p, src, dst) { return p.mod.findIndex((m) => m.src === src && m.dst === dst); }
  function routeAmount(p, src, dst) { const i = findSlot(p, src, dst); return i < 0 ? 0 : p.mod[i].amount / 32767; }
  function setRoute(p, src, dst, amount) {
    let i = findSlot(p, src, dst);
    const q = q15(amount);
    if (q === 0) { if (i >= 0) p.mod[i] = { src: 0, dst: 0, amount: 0 }; return true; }
    if (i < 0) i = p.mod.findIndex((m) => m.src === 0 && m.dst === 0);
    if (i < 0) return false;                                                 // all 16 slots in use
    p.mod[i] = { src, dst, amount: q };
    return true;
  }
  // A source's "send" (MOD overlay): the first slot with that source.
  function sendSlot(p, src) { return p.mod.findIndex((m) => m.src === src && m.dst !== 0); }
  function setSendDest(p, src, dst) {
    const i = sendSlot(p, src);
    if (i >= 0) {
      if (dst === 0) { p.mod[i] = { src: 0, dst: 0, amount: 0 }; return true; }
      const other = findSlot(p, src, dst);            // the source already goes there: keep that one
      if (other >= 0 && other !== i) { p.mod[i] = { src: 0, dst: 0, amount: 0 }; return true; }
      p.mod[i].dst = dst; return true;
    }
    if (dst === 0) return true;
    return setRoute(p, src, dst, 0.5);
  }

  // Drag-to-assign (Massive style): a knob shows up to 3 modulation slots, each one matrix slot with its
  // destination; dropping a source on the knob adds one.
  const KNOB_SLOTS = 3;
  function routesTo(p, dst) { const r = []; p.mod.forEach((m, i) => { if (m.dst === dst && m.src !== 0) r.push(i); }); return r; }
  // returns the slot index, -2 when the knob already has 3 sources, -1 when all 16 slots are in use
  function addRoute(p, src, dst, amount) {
    const have = findSlot(p, src, dst); if (have >= 0) return have;
    if (routesTo(p, dst).length >= KNOB_SLOTS) return -2;
    const i = p.mod.findIndex((m) => m.src === 0 && m.dst === 0);
    if (i < 0) return -1;
    p.mod[i] = { src, dst, amount: q15(amount === undefined ? 0.5 : amount) };
    return i;
  }
  function removeRoute(p, i) { if (i >= 0 && i < p.mod.length) p.mod[i] = { src: 0, dst: 0, amount: 0 }; }
  function setRouteAmountAt(p, i, v) { p.mod[i].amount = q15(clamp(v, -1, 1)); }
  const SRC_SHORT = ['', 'E1', 'E2', 'E3', 'E4', 'L5', 'L6', 'L7', 'L8', 'VEL', 'KEY', 'MW', 'PB', 'AT',
    'M1', 'M2', 'M3', 'M4', 'M5', 'M6', 'M7', 'M8', 'RND', 'ONE', 'OS3'];
  const SRC_LABELS = ['NONE', 'ENVELOPE 1', 'ENVELOPE 2', 'ENVELOPE 3', 'ENVELOPE 4', 'MODULATOR 5', 'MODULATOR 6', 'MODULATOR 7',
    'MODULATOR 8', 'VELOCITY', 'KEY TRACK', 'MOD WHEEL', 'PITCH BEND', 'AFTERTOUCH', 'MACRO 1', 'MACRO 2', 'MACRO 3', 'MACRO 4',
    'MACRO 5', 'MACRO 6', 'MACRO 7', 'MACRO 8', 'RANDOM', 'CONSTANT', 'OSC 3'];
  // which knob carries which destination
  const KNOB_DST = { tune: 1, 'osc1.freq': 2, 'osc2.freq': 3, 'osc3.freq': 4, 'osc1.wave': 5, 'osc2.wave': 6, 'osc3.wave': 7,
    'osc1.warp': 8, 'osc2.warp': 9, 'osc3.warp': 10, 'osc1.vol': 11, 'osc2.vol': 12, 'osc3.vol': 13, 'noise.vol': 14,
    'f1.cutoff': 15, 'f2.cutoff': 16, 'f1.res': 17, 'f2.res': 18, serpar: 19, 'f1.drive': 20, master: 21,
    'lfo1.rate': 23, 'lfo2.rate': 24 };

  // ------------------------------------------------------------- oscillators ---
  // semis = footage (RANGE) + FREQUENCY offset; fine = cents. The RANGE part is the nearest footage.
  // The footage is not stored in the preset (only semis), so the panel remembers the RANGE last chosen in
  // a non-enumerable o._range while it still fits (offset within +-7); otherwise the nearest footage.
  function oscRangeIndex(o) {
    if (o.flags & OSC_LO) return 0;
    if (o._range > 0 && Math.abs(o.semis - RANGE_SEMIS[o._range]) <= 7) return o._range;
    const r = clamp(Math.round(o.semis / 12), -2, 2);
    return r + 3;
  }
  function rememberRange(o, idx) {
    if (!Object.prototype.hasOwnProperty.call(o, '_range')) Object.defineProperty(o, '_range', { value: idx, enumerable: false, writable: true });
    else o._range = idx;
  }
  function oscOffset(o) { return (o.flags & OSC_LO) ? o.semis : o.semis - RANGE_SEMIS[oscRangeIndex(o)]; }
  function setOscRange(o, idx) {
    idx = clamp(Math.round(idx), 0, 5);
    const off = oscOffset(o);
    if (idx === 0) { o.flags |= OSC_LO; o.semis = clamp(off, -64, 63); return; }
    o.flags &= ~OSC_LO;
    o.semis = clamp(RANGE_SEMIS[idx] + off, -64, 63);
    rememberRange(o, idx);
  }
  // FREQUENCY: -7..+7 semitones, continuous: whole semitones go to semis, the rest to fine (cents)
  function oscFreq(o) { return oscOffset(o) + o.fine / 100; }
  function setOscFreq(o, v) {
    v = clamp(v, -7, 7);
    const base = (o.flags & OSC_LO) ? 0 : RANGE_SEMIS[oscRangeIndex(o)];
    const st = Math.round(v);
    if (!(o.flags & OSC_LO)) rememberRange(o, oscRangeIndex(o));
    o.semis = clamp(base + st, -64, 63);
    o.fine = clamp(Math.round((v - st) * 100), -100, 100);
  }
  function oscWave(o) { return clamp(Math.round((o.position / 32767) * 7), 0, 7); }
  function setOscWave(o, frame) { o.table = WT_MOOG; o.position = Math.round((clamp(Math.round(frame), 0, 7) / 7) * 32767); }

  // ------------------------------------------------------------- definitions ---
  // kind: 'knob' (continuous), 'detent' (integer steps), 'switch' (0/1)
  const P = {};
  function def(id, o) { o.id = id; o.kind = o.kind || 'knob'; o.fmt = o.fmt || fmtDial; P[id] = o; return o; }

  // CONTROLLERS
  def('tune', { label: 'TUNE', min: -100, max: 100, def: 0, bipolar: true, scale: 'pm',
    get: (p) => p.osc[0].fine,
    set: (p, v) => {                                   // shifts all three oscillators by the same cents
      const d = Math.round(v) - p.osc[0].fine;
      for (const o of p.osc) o.fine = clamp(o.fine + d, -100, 100);
      p.osc[0].fine = clamp(Math.round(v), -100, 100);
    },
    fmt: (v) => `${v >= 0 ? '+' : ''}${Math.round(v)} cents` });
  def('glide', { label: 'GLIDE', min: 0, max: 1, def: 0.35,
    get: (p) => p.glide / 32767, set: (p, v) => { p.glide = q15(v); },
    fmt: (v) => (v <= 0 ? 'OFF' : fmtTime(envSec(v))) });
  def('modrate', { label: 'MOD RATE', min: 0, max: 1, def: 0.49,
    get: (p) => p.lfo[0].rate / 32767, set: (p, v) => { p.lfo[0].rate = q15(v); },
    fmt: (v) => fmtHz(lfoHz(v)) });
  def('voice', { label: 'VOICE', kind: 'detent', min: 0, max: 2, def: 0, labels: VOICE_MODES,
    get: (p) => p.voice_mode, set: (p, v) => { p.voice_mode = clamp(Math.round(v), 0, 2); }, fmt: (v) => VOICE_MODES[v] });
  def('velsens', { label: 'VELOCITY', min: 0, max: 1, def: 0.5,
    get: (p) => p.vel_sens / 127, set: (p, v) => { p.vel_sens = clamp(Math.round(v * 127), 0, 127); }, fmt: fmtPct });

  // OSCILLATOR BANK + MIXER
  for (let i = 0; i < 3; i++) {
    const n = i + 1;
    def(`osc${n}.range`, { label: 'RANGE', kind: 'detent', min: 0, max: 5, def: 3, labels: RANGE_LABELS,
      get: (p) => oscRangeIndex(p.osc[i]), set: (p, v) => setOscRange(p.osc[i], v), fmt: (v) => RANGE_LABELS[v] });
    def(`osc${n}.freq`, { label: 'FREQUENCY', min: -7, max: 7, def: 0, bipolar: true, scale: 'freq',
      get: (p) => clamp(oscFreq(p.osc[i]), -7, 7), set: (p, v) => setOscFreq(p.osc[i], v),
      fmt: (v) => { const st = Math.round(v), c = Math.round((v - st) * 100); return `${st >= 0 ? '+' : ''}${st} st ${c >= 0 ? '+' : ''}${c} c`; } });
    def(`osc${n}.wave`, { label: 'WAVEFORM', kind: 'detent', min: 0, max: 7, def: 3, labels: WAVE_NAMES,
      get: (p) => oscWave(p.osc[i]), set: (p, v) => setOscWave(p.osc[i], v),
      fmt: (v) => WAVE_NAMES[v] });
    def(`osc${n}.vol`, { label: `OSC ${n}`, min: 0, max: 1, def: 0.8,
      get: (p) => p.osc[i].amp / 32767, set: (p, v) => { p.osc[i].amp = q15(v); } });
    def(`osc${n}.on`, { label: 'ON', kind: 'switch', min: 0, max: 1, def: i === 0 ? 1 : 0,
      get: (p) => (p.osc[i].flags & OSC_ON ? 1 : 0),
      set: (p, v) => { p.osc[i].flags = v ? (p.osc[i].flags | OSC_ON) : (p.osc[i].flags & ~OSC_ON); }, fmt: (v) => (v ? 'ON' : 'OFF') });
  }
  def('drift', { label: 'DRIFT', min: 0, max: 1, def: 0.25,
    get: (p) => p.osc[0].drift / 127,
    set: (p, v) => { for (const o of p.osc) o.drift = clamp(Math.round(v * 127), 0, 127); },
    fmt: (v) => `±${Math.round(v * 12)} cents` });
  def('noise.vol', { label: 'NOISE', min: 0, max: 1, def: 0,
    get: (p) => p.noise.amp / 32767, set: (p, v) => { p.noise.amp = q15(v); } });
  def('noise.color', { label: 'COLOUR', min: 0, max: 1, def: 0, scale: 'color',
    get: (p) => p.noise.color / 32767, set: (p, v) => { p.noise.color = q15(v); },
    fmt: (v) => (v < 0.15 ? 'WHITE' : v > 0.85 ? 'DARK' : v < 0.5 ? 'PINK-ISH' : 'PINK') });
  def('drive', { label: 'DRIVE', min: 0, max: 1, def: 0,
    get: (p) => p.filt[0].drive / 32767, set: (p, v) => { p.filt[0].drive = q15(v); } });

  // MODIFIERS: filter 1
  def('f1.cutoff', { label: 'CUTOFF', min: 0, max: 1, def: 30000 / 32767, scale: 'cutoff',
    get: (p) => p.filt[0].cutoff / 32767, set: (p, v) => { p.filt[0].cutoff = q15(clamp(v, 0, 1)); },
    fmt: (v) => fmtHz(cutoffHz(v)) });
  def('f1.res', { label: 'EMPHASIS', min: 0, max: 1, def: 0,
    get: (p) => p.filt[0].res / 32767, set: (p, v) => { p.filt[0].res = q15(v); } });
  def('f1.contour', { label: 'AMOUNT OF CONTOUR', min: 0, max: 1, def: 0,
    get: (p) => clamp(routeAmount(p, SRC.ENV1, DST.F1_CUTOFF), 0, 1),
    set: (p, v) => setRoute(p, SRC.ENV1, DST.F1_CUTOFF, v),
    fmt: (v) => `${Math.round(v * 136)} semitones` });
  def('f1.kbd', { label: 'KEYBOARD TRACK', min: 0, max: 1, def: 0,
    get: (p) => clamp(p.filt[0].keytrack / 32767, 0, 1), set: (p, v) => { p.filt[0].keytrack = q15(v); }, fmt: fmtPct });
  def('f1.type', { label: 'TYPE', kind: 'detent', min: 0, max: 6, def: 0, labels: FILTER_SELECT,
    get: (p) => { const k = FILTER_SELECT.findIndex((n) => FILTER_ENUM[n] === p.filt[0].type); return k < 0 ? 3 : k; },
    set: (p, v) => { p.filt[0].type = FILTER_ENUM[FILTER_SELECT[clamp(Math.round(v), 0, 6)]]; },
    fmt: (v) => FILTER_SELECT[v] });
  // envelopes 1-4 (ENV 4 = loudness), Massive-style stages: delay, attack to A-LEVEL, decay to D-LEVEL,
  // then to SUSTAIN over S-TIME, release. Times exponential 0.5 ms .. 30 s. dec_level < 0 = "as sustain".
  for (let e = 0; e < 4; e++) {
    const tag = `env${e + 1}`;
    const t = (key, label, d) => def(`${tag}.${key}`, { label, min: 0, max: 1, def: d,
      get: (p) => p.env[e][key] / 32767, set: (p, v) => { p.env[e][key] = q15(v); }, fmt: (v) => (v <= 0 && key !== 'attack' ? '0 ms' : fmtTime(envSec(v))) });
    t('delay', 'DELAY', 0); t('attack', 'ATTACK', 2000 / 32767); t('decay', 'DECAY', 16000 / 32767); t('release', 'RELEASE', 12000 / 32767);
    t('sust_time', 'S-TIME', 0);
    def(`${tag}.sustain`, { label: 'SUSTAIN', min: 0, max: 1, def: 1,
      get: (p) => p.env[e].sustain / 32767, set: (p, v) => { p.env[e].sustain = q15(v); }, fmt: fmtPct });
    def(`${tag}.att_level`, { label: 'A-LEVEL', min: 0, max: 1, def: 1,
      get: (p) => clamp(p.env[e].att_level / 32767, 0, 1), set: (p, v) => { p.env[e].att_level = q15(v); }, fmt: fmtPct });
    def(`${tag}.dec_level`, { label: 'D-LEVEL', min: 0, max: 1, def: 1,
      get: (p) => clamp((p.env[e].dec_level < 0 ? p.env[e].sustain : p.env[e].dec_level) / 32767, 0, 1),
      set: (p, v) => { p.env[e].dec_level = q15(v); }, fmt: fmtPct });
    def(`${tag}.loop`, { label: 'LOOP', kind: 'switch', min: 0, max: 1, def: 0,
      get: (p) => (p.env[e].flags & 1 ? 1 : 0), set: (p, v) => { p.env[e].flags = v ? (p.env[e].flags | 1) : (p.env[e].flags & ~1); }, fmt: (v) => (v ? 'LOOP' : 'ONE SHOT') });
  }

  // oscillator intensity (warp) and filter routing (0 = filter 1, 1 = filter 2); noise routing
  for (let i = 0; i < 3; i++) {
    def(`osc${i + 1}.warp`, { label: 'INTENSITY', min: 0, max: 1, def: 0,
      get: (p) => p.osc[i].warp / 32767, set: (p, v) => { p.osc[i].warp = q15(v); }, fmt: fmtPct });
    def(`osc${i + 1}.route`, { label: 'F1 · F2', min: 0, max: 1, def: 0, scale: 'route',
      get: (p) => p.osc[i].route / 32767, set: (p, v) => { p.osc[i].route = q15(v); }, fmt: (v) => `F1 ${Math.round((1 - v) * 100)} / F2 ${Math.round(v * 100)}` });
  }
  def('noise.route', { label: 'F1 · F2', min: 0, max: 1, def: 0, scale: 'route',
    get: (p) => p.noise.route / 32767, set: (p, v) => { p.noise.route = q15(v); }, fmt: (v) => `F1 ${Math.round((1 - v) * 100)} / F2 ${Math.round(v * 100)}` });
  // FEEDBACK (Massive's section; the engine has no feedback path yet: shown, not editable)
  def('feedback.amt', { label: 'AMOUNT', min: 0, max: 1, def: 0, available: () => false, get: () => 0, set: () => {} });
  def('feedback.route', { label: 'F1 · F2', min: 0, max: 1, def: 0, scale: 'route', available: () => false, get: () => 0, set: () => {} });

  // filter 2, drives, serial / parallel and the filter mix
  const FILTER2_SELECT = ['OFF'].concat(FILTER_SELECT);
  def('f2.type', { label: 'TYPE', kind: 'detent', min: 0, max: 7, def: 0, labels: FILTER2_SELECT,
    get: (p) => { const k = FILTER2_SELECT.findIndex((n) => FILTER_ENUM[n] === p.filt[1].type); return k < 0 ? 0 : k; },
    set: (p, v) => { p.filt[1].type = FILTER_ENUM[FILTER2_SELECT[clamp(Math.round(v), 0, 7)]]; }, fmt: (v) => FILTER2_SELECT[v] });
  def('f2.cutoff', { label: 'CUTOFF', min: 0, max: 1, def: 0.6, scale: 'cutoff',
    get: (p) => p.filt[1].cutoff / 32767, set: (p, v) => { p.filt[1].cutoff = q15(clamp(v, 0, 1)); }, fmt: (v) => fmtHz(cutoffHz(v)) });
  def('f2.res', { label: 'EMPHASIS', min: 0, max: 1, def: 0, get: (p) => p.filt[1].res / 32767, set: (p, v) => { p.filt[1].res = q15(v); } });
  def('f2.kbd', { label: 'KEY TRACK', min: 0, max: 1, def: 0,
    get: (p) => clamp(p.filt[1].keytrack / 32767, 0, 1), set: (p, v) => { p.filt[1].keytrack = q15(v); }, fmt: fmtPct });
  def('f1.drive', { label: 'DRIVE', min: 0, max: 1, def: 0, get: (p) => p.filt[0].drive / 32767, set: (p, v) => { p.filt[0].drive = q15(v); } });
  def('f2.drive', { label: 'DRIVE', min: 0, max: 1, def: 0, get: (p) => p.filt[1].drive / 32767, set: (p, v) => { p.filt[1].drive = q15(v); } });
  def('serpar', { label: 'SER · PAR', min: 0, max: 1, def: 0, scale: 'serpar',
    get: (p) => 1 - p.serpar / 32767, set: (p, v) => { p.serpar = q15(1 - v); },
    fmt: (v) => (v < 0.02 ? 'SERIAL' : v > 0.98 ? 'PARALLEL' : `${Math.round((1 - v) * 100)}% SERIAL`) });
  def('f1.mix', { label: 'F1 MIX', min: 0, max: 1, def: 1, get: (p) => p.f1_mix / 32767, set: (p, v) => { p.f1_mix = q15(v); } });
  def('f2.mix', { label: 'F2 MIX', min: 0, max: 1, def: 1, get: (p) => p.f2_mix / 32767, set: (p, v) => { p.f2_mix = q15(v); } });
  // macros 1-8: initial values (sources MACRO1..8 in the matrix)
  for (let i = 0; i < 8; i++) def(`macro${i + 1}`, { label: `MACRO ${i + 1}`, min: 0, max: 1, def: 0,
    get: (p) => clamp(p.macro[i] / 32767, 0, 1), set: (p, v) => { p.macro[i] = q15(v); }, fmt: fmtPct });
  // glide on/off rocker (off keeps the time in p._glide_mem, not part of the preset image)
  def('glide.on', { label: 'GLIDE', kind: 'switch', min: 0, max: 1, def: 0, get: (p) => (p.glide > 0 ? 1 : 0),
    set: (p, v) => {
      if (!p._glide_mem) Object.defineProperty(p, '_glide_mem', { value: { t: 0 }, enumerable: false, writable: true });
      if (!v) { if (p.glide > 0) p._glide_mem.t = p.glide; p.glide = 0; } else if (!p.glide) p.glide = p._glide_mem.t || q15(0.35);
    }, fmt: (v) => (v ? 'ON' : 'OFF') });
  def('bend', { label: 'BEND RANGE', kind: 'detent', min: 1, max: 24, def: 2, labels: Array.from({ length: 24 }, (_, i) => `±${i + 1}`),
    get: (p) => clamp(p.bend_range, 1, 24), set: (p, v) => { p.bend_range = clamp(Math.round(v), 1, 24); }, fmt: (v) => `±${v} st` });

  // OUTPUT: master Q13 (8192 = 1.0, 32767 = 4.0): knob k -> gain 4 k^2, unity at 12 o'clock
  def('master', { label: 'VOLUME', min: 0, max: 1, def: 0.5,
    get: (p) => Math.sqrt(clamp(p.master, 0, 32767) / 32767), set: (p, v) => { p.master = clamp(Math.round(32767 * v * v), 0, 32767); },
    fmt: (v) => { const g = 4 * v * v; return g <= 0.0001 ? '-inf dB' : `${(20 * Math.log10(g)).toFixed(1)} dB`; } });
  // ------------------------------------------------------------- EFFECTS ---
  // Six effect units (preset v5 chain: OCT FUZZ, TAPE, ENSEMBLE, BBD ECHO, OIL CAN, REVERB). The patch
  // fields behind them live ONLY in FX_FIELDS below: a dotted path into the patch ('!' = inverted), or
  // null when the current preset format has no such field (the control is shown dimmed and does
  // nothing). Today the paths are the v4 fields (fx.drive, chorus_*, delay_*, reverb_*); when mf_core.js
  // gains the v5 fields, change the paths here and nothing else.
  const FX_FIELDS = {   // v5 fields (preset 508 B), 17 Nov 2026
    'fuzz.amount': 'fx.fuzz_amount', 'fuzz.octave': 'fx.fuzz_octave', 'fuzz.tone': 'fx.fuzz_tone', 'fuzz.level': 'fx.fuzz_level',
    'tape.drive': 'fx.tape_drive', 'tape.tone': 'fx.tape_tone', 'tape.level': 'fx.tape_level',
    'ensemble.mix': 'fx.ens_mix', 'ensemble.rate': 'fx.ens_rate', 'ensemble.depth': 'fx.ens_depth',
    'ensemble.mode': 'fx.ens_mode',            // 0 OFF, 1 I, 2 II, 3 I+II, 4 VIBRATO
    'echo.mix': 'fx.bbd_mix', 'echo.time': 'fx.bbd_time', 'echo.feedback': 'fx.bbd_feedback',
    'echo.mod': 'fx.bbd_mod', 'echo.tone': 'fx.bbd_tone',
    'oilcan.mix': 'fx.oil_mix', 'oilcan.time': 'fx.oil_time', 'oilcan.feedback': 'fx.oil_feedback', 'oilcan.warble': 'fx.oil_warble', 'oilcan.smear': 'fx.oil_smear',
    'echo.sync': 'fx.fx_sync', 'oilcan.sync': 'fx.fx_sync',   // tempo sync nibbles (BBD high, OIL CAN low): see delayPlays
    'reverb.type': 'fx.rev_type',              // 0 TWIN TANK, 1 SPRING
    'reverb.mix': 'fx.rev_amount', 'reverb.decay': null, 'reverb.tone': null, 'reverb.drip': null,   // the Chroma tanks have one AMOUNT knob
  };
  const ENSEMBLE_MODES = ['OFF', 'I', 'II', 'I+II', 'VIBRATO'];
  const FX_MODE_V4 = [null, [0.30, 0.45], [0.45, 0.60], [0.38, 0.80], [0.55, 0.95]];   // (rate, depth) per mode on v4
  const FX_UNITS = [   // level = the parameter the unit's on/off rocker switches
    { id: 'fuzz', title: 'OCT FUZZ', level: 'fuzz.amount', knobs: [['amount', 'FUZZ'], ['octave', 'OCTAVE'], ['tone', 'TONE'], ['level', 'LEVEL']] },
    { id: 'tape', title: 'TAPE', level: 'tape.drive', knobs: [['drive', 'DRIVE'], ['tone', 'TONE'], ['level', 'LEVEL']] },
    { id: 'ensemble', title: 'ENSEMBLE', level: null, knobs: [['mode', 'MODE'], ['mix', 'MIX']] },
    { id: 'echo', title: 'BBD ECHO', level: 'echo.mix', knobs: [['mix', 'MIX'], ['time', 'TIME'], ['feedback', 'REPEATS'], ['mod', 'MOD'], ['tone', 'TONE']] },
    { id: 'oilcan', title: 'OIL CAN', level: 'oilcan.mix', knobs: [['mix', 'MIX'], ['time', 'TIME'], ['feedback', 'REPEATS'], ['warble', 'WARBLE'], ['smear', 'SMEAR']] },
    { id: 'reverb', title: 'REVERB', level: 'reverb.mix', knobs: [['type', 'TYPE'], ['mix', 'MIX'], ['decay', 'DECAY'], ['tone', 'TONE'], ['drip', 'DRIP']] },
  ];
  // values a unit gets when it is switched on and has no settings of its own
  const FX_WAKE = { 'echo.mix': { 'echo.time': 0.75, 'echo.feedback': 0.35 }, 'reverb.mix': { 'reverb.decay': 0.6, 'reverb.tone': 0.6 },
    'ensemble.mix': { 'ensemble.rate': 0.3, 'ensemble.depth': 0.5 } };
  function fxAvailable(id) { return id === 'ensemble.mode' ? !!(FX_FIELDS['ensemble.mode'] || FX_FIELDS['ensemble.mix']) : !!FX_FIELDS[id]; }
  function fxRead(p, id) {
    const path = FX_FIELDS[id]; if (!path) return null;
    const inv = path[0] === '!', keys = (inv ? path.slice(1) : path).split('.');
    let o = p; for (const k of keys) o = o == null ? undefined : o[k];
    if (typeof o !== 'number') return null;
    const v = clamp(o / 32767, 0, 1); return inv ? 1 - v : v;
  }
  function fxWrite(p, id, v) {
    const path = FX_FIELDS[id]; if (!path) return false;
    const inv = path[0] === '!', keys = (inv ? path.slice(1) : path).split('.');
    let o = p; for (let k = 0; k < keys.length - 1; k++) o = o[keys[k]];
    o[keys[keys.length - 1]] = q15(inv ? 1 - v : v);
    return true;
  }
  function fxWake(p, id) {
    const w = FX_WAKE[id]; if (!w) return;
    if (Object.keys(w).every((k) => !fxRead(p, k) || (FX_FIELDS[k] || '')[0] === '!')) for (const k of Object.keys(w)) fxWrite(p, k, w[k]);
  }
  // on/off rockers: off keeps the level in p._fx_mem (not part of the preset image) and sets it to 0
  function fxIsOn(p, levelId) { return (fxRead(p, levelId) || 0) > 0; }
  function fxSetOn(p, levelId, on) {
    if (!FX_FIELDS[levelId]) return false;
    if (!p._fx_mem) Object.defineProperty(p, '_fx_mem', { value: {}, enumerable: false, writable: true });
    if (!on) { const v = fxRead(p, levelId); if (v > 0) p._fx_mem[levelId] = v; fxWrite(p, levelId, 0); return true; }
    if (fxIsOn(p, levelId)) return true;
    fxWrite(p, levelId, p._fx_mem[levelId] || 0.5); fxWake(p, levelId);
    return true;
  }
  // ---- delay tempo sync (fx_sync, 17 Nov 2026): high nibble BBD ECHO, low nibble OIL CAN, division + 1, 0 = free.
  // delayPlays mirrors dsp mfe_fx_delay_ms() for the tablet build (BBD line 22050, oil can ring 32768).
  const DELAY_WHICH = { echo: 0, oilcan: 1 };
  const DELAY_BPM = 120;   // the tablet engine's tempo (mfe default; no tempo control on the tablet yet)
  const SYNC_BEATS = [1 / 8, 1 / 6, 1 / 4, 1 / 3, 3 / 8, 1 / 2, 2 / 3, 3 / 4, 1, 3 / 2, 2, 4, 8];
  function dsyncGet(p, which) { const s = (p.fx && p.fx.fx_sync) || 0; return which ? s & 15 : s >> 4; }
  function dsyncSet(p, which, v) { const s = p.fx.fx_sync || 0; p.fx.fx_sync = which ? (s & 0xF0) | v : (s & 0x0F) | (v << 4); }
  function delayPlays(p, which, bpm = DELAY_BPM) {   // { ms, div } (div null when free)
    const SR = 44100, sync = dsyncGet(p, which);
    const max = which ? Math.floor(Math.floor((32768 - 320) * 19200 / 17640) * 147 / 160) : (22050 - 16 - 441) * 2;   // samples
    if (sync) {
      let d = Math.min(sync - 1, 12), len = SYNC_BEATS[d] * 60 / bpm * SR;
      while (len > max && d > 0) { d--; len = SYNC_BEATS[d] * 60 / bpm * SR; }
      return { ms: Math.round(Math.min(len, max) * 1000 / SR), div: d };
    }
    if (!which) {
      const t = clamp(Math.floor(clamp(p.fx.bbd_time, 0, 32767) * 22051 / 32768), 220, 22050 - 16 - 441);
      return { ms: Math.round(t * 2000 / SR), div: null };
    }
    const k = clamp(p.fx.oil_time, 0, 32767) * 2, k2 = Math.floor(k * k / 65536);
    const d48 = Math.min(1920 + Math.floor(1080 * k2 / 4096), 19200);
    return { ms: Math.round(Math.floor(d48 * 147 / 160) * 1000 / SR), div: null };
  }
  function delaySyncOn(p, which) {   // the division nearest the free time playing, among those that fit
    const want = delayPlays(p, which).ms, t = clone(p);
    let best = 5, be = Infinity;
    for (let d = 0; d < 13; d++) {
      dsyncSet(t, which, d + 1);
      const r = delayPlays(t, which);
      if (r.div !== d) break;
      if (Math.abs(r.ms - want) < be) { be = Math.abs(r.ms - want); best = d; }
    }
    dsyncSet(p, which, best + 1);
  }
  function fmtDelay(p, which) {
    const r = delayPlays(p, which);
    return r.div === null ? `${r.ms} ms` : `${SYNC_LABELS[r.div]}${r.div === dsyncGet(p, which) - 1 ? '' : '*'} · ${r.ms} ms`;
  }
  for (const [u, which] of Object.entries(DELAY_WHICH)) {
    def(`${u}.sync`, { label: 'SYNC', kind: 'switch', min: 0, max: 1, def: 0, fx: true, available: () => !!FX_FIELDS[`${u}.sync`],
      get: (p) => (dsyncGet(p, which) ? 1 : 0),
      set: (p, v) => { if (!v) dsyncSet(p, which, 0); else if (!dsyncGet(p, which)) delaySyncOn(p, which); },
      fmt: (v) => (v ? 'TEMPO' : 'FREE') });
    def(`${u}.div`, { label: 'DIVISION', kind: 'detent', min: 0, max: 12, def: 5, labels: SYNC_LABELS, fx: true,
      available: () => !!FX_FIELDS[`${u}.sync`],
      get: (p) => { const s = dsyncGet(p, which); return s ? s - 1 : 5; },
      set: (p, v) => { dsyncSet(p, which, clamp(Math.round(v), 0, 12) + 1); }, fmt: (v) => SYNC_LABELS[v] });   // picking one turns SYNC on
  }
  for (const u of FX_UNITS) {
    for (const [key, label] of u.knobs) {
      const id = `${u.id}.${key}`;
      if (id === 'ensemble.mode' || id === 'reverb.type') continue;
      if (key === 'time' && id.split('.')[0] in DELAY_WHICH) {   // TIME: ms, or the division when SYNC is on
        const which = DELAY_WHICH[u.id];
        def(id, { label, min: 0, max: 1, def: 0, fx: true, available: () => fxAvailable(id),
          get: (p) => { const s = dsyncGet(p, which); if (s) return (s - 1) / 12; const v = fxRead(p, id); return v === null ? 0 : v; },
          set: (p, v) => { if (dsyncGet(p, which)) dsyncSet(p, which, clamp(Math.round(v * 12), 0, 12) + 1); else fxWrite(p, id, v); },
          fmt: (v, p) => (p ? fmtDelay(p, which) : `${Math.round(v * 1000)} ms`) });
        continue;
      }
      def(id, { label, min: 0, max: 1, def: 0, fx: true, available: () => fxAvailable(id),
        get: (p) => { const v = fxRead(p, id); return v === null ? 0 : v; },
        set: (p, v) => { const was = fxRead(p, id); if (fxWrite(p, id, v) && !was && v > 0) fxWake(p, id); },
        fmt: id.endsWith('.time') ? (v) => `${Math.round(v * 500)} ms` : fmtDial });
    }
    if (u.level) {
      def(`${u.id}.on`, { label: 'ON', kind: 'switch', min: 0, max: 1, def: 0, fx: true, available: () => !!FX_FIELDS[u.level],
        get: (p) => (fxIsOn(p, u.level) ? 1 : 0), set: (p, v) => { fxSetOn(p, u.level, !!v); }, fmt: (v) => (v ? 'ON' : 'OFF') });
    }
  }
  const REVERB_TYPES = ['TWIN TANK', 'SPRING'];
  def('reverb.type', { label: 'TYPE', kind: 'detent', min: 0, max: 1, def: 1, labels: REVERB_TYPES, fx: true,
    available: () => !!FX_FIELDS['reverb.type'],
    get: (p) => { const v = fxRead(p, 'reverb.type'); return v === null ? 1 : clamp(Math.round(v * 32767), 0, 1); },
    set: (p, v) => { fxWrite(p, 'reverb.type', clamp(Math.round(v), 0, 1) / 32767); }, fmt: (v) => REVERB_TYPES[v] });
  def('ensemble.mode', { label: 'MODE', kind: 'detent', min: 0, max: 4, def: 0, labels: ENSEMBLE_MODES, fx: true,
    available: () => fxAvailable('ensemble.mode'),
    get: (p) => {
      if (FX_FIELDS['ensemble.mode']) { const v = fxRead(p, 'ensemble.mode'); return v === null ? 0 : clamp(Math.round(v * 32767), 0, 4); }
      if (!fxRead(p, 'ensemble.mix')) return 0;
      const r = fxRead(p, 'ensemble.rate') || 0, d = fxRead(p, 'ensemble.depth') || 0;
      let best = 1, bd = 9;
      for (let m = 1; m < 5; m++) { const e = Math.abs(FX_MODE_V4[m][0] - r) + Math.abs(FX_MODE_V4[m][1] - d); if (e < bd) { bd = e; best = m; } }
      return best;
    },
    set: (p, v) => {
      const m = clamp(Math.round(v), 0, 4);
      if (FX_FIELDS['ensemble.mode']) { fxWrite(p, 'ensemble.mode', m / 32767); return; }
      if (m === 0) { fxSetOn(p, 'ensemble.mix', false); return; }
      if (!fxIsOn(p, 'ensemble.mix')) fxSetOn(p, 'ensemble.mix', true);
      fxWrite(p, 'ensemble.rate', FX_MODE_V4[m][0]); fxWrite(p, 'ensemble.depth', FX_MODE_V4[m][1]);
    }, fmt: (v) => ENSEMBLE_MODES[v] });

  // MOD overlay: modulator slots 1..4 (LFO / PERFORMER / STEPPER)
  const SLOT_KINDS = ['LFO', 'PERFORMER', 'STEPPER'];
  function slotKind(l) { return l.wave === W.PERFORMER ? 1 : l.wave === W.STEPPER ? 2 : 0; }
  for (let i = 0; i < 4; i++) {
    const n = i + 1, src = SRC.LFO1 + i;
    def(`lfo${n}.kind`, { label: 'TYPE', kind: 'detent', min: 0, max: 2, def: 0, labels: SLOT_KINDS,
      get: (p) => slotKind(p.lfo[i]),
      set: (p, v) => {
        const l = p.lfo[i], k = clamp(Math.round(v), 0, 2);
        if (k === slotKind(l)) return;
        l.wave = k === 0 ? W.SINE : k === 1 ? W.PERFORMER : W.STEPPER;
        if (k > 0 && l.step_level.every((x) => x === 127) && l.step_shape.every((x) => x === 0)) {
          // a fresh sequence that does something audible
          for (let s = 0; s < 16; s++) {
            l.step_shape[s] = k === 1 ? [10, 0, 11, 1][s % 4] : 0;
            l.step_level[s] = k === 1 ? 127 : [127, 32, 80, -40, 100, 0, 60, -90][s % 8];
          }
        }
      }, fmt: (v) => SLOT_KINDS[v] });
    def(`lfo${n}.wave`, { label: 'WAVE', kind: 'detent', min: 0, max: 5, def: 0, labels: LFO_WAVE_NAMES,
      get: (p) => (p.lfo[i].wave <= 5 ? p.lfo[i].wave : 0),
      set: (p, v) => { if (p.lfo[i].wave <= 5) p.lfo[i].wave = clamp(Math.round(v), 0, 5); }, fmt: (v) => LFO_WAVE_NAMES[v] });
    def(`lfo${n}.rate`, { label: 'RATE', min: 0, max: 1, def: 0.49,
      get: (p) => p.lfo[i].rate / 32767, set: (p, v) => { p.lfo[i].rate = q15(v); }, fmt: (v) => fmtHz(lfoHz(v)) });
    def(`lfo${n}.sync`, { label: 'SYNC', kind: 'switch', min: 0, max: 1, def: 0,
      get: (p) => (p.lfo[i].flags & LFO_SYNC ? 1 : 0),
      set: (p, v) => { p.lfo[i].flags = v ? p.lfo[i].flags | LFO_SYNC : p.lfo[i].flags & ~LFO_SYNC; }, fmt: (v) => (v ? 'TEMPO' : 'FREE') });
    def(`lfo${n}.div`, { label: 'DIVISION', kind: 'detent', min: 0, max: 12, def: 8, labels: SYNC_LABELS,
      get: (p) => clamp(p.lfo[i].sync_div, 0, 12), set: (p, v) => { p.lfo[i].sync_div = clamp(Math.round(v), 0, 12); }, fmt: (v) => SYNC_LABELS[v] });
    def(`lfo${n}.key`, { label: 'RETRIG', kind: 'switch', min: 0, max: 1, def: 0,
      get: (p) => (p.lfo[i].flags & LFO_KEYSYNC ? 1 : 0),
      set: (p, v) => { p.lfo[i].flags = v ? p.lfo[i].flags | LFO_KEYSYNC : p.lfo[i].flags & ~LFO_KEYSYNC; }, fmt: (v) => (v ? 'KEY' : 'FREE') });
    def(`lfo${n}.uni`, { label: 'UNIPOLAR', kind: 'switch', min: 0, max: 1, def: 0,
      get: (p) => (p.lfo[i].flags & LFO_UNIPOLAR ? 1 : 0),
      set: (p, v) => { p.lfo[i].flags = v ? p.lfo[i].flags | LFO_UNIPOLAR : p.lfo[i].flags & ~LFO_UNIPOLAR; }, fmt: (v) => (v ? 'UNI' : 'BI') });
    def(`lfo${n}.glide`, { label: 'GLIDE', min: 0, max: 1, def: 0,
      get: (p) => p.lfo[i].glide / 32767, set: (p, v) => { p.lfo[i].glide = q15(v); }, fmt: fmtPct });
    def(`lfo${n}.dest`, { label: 'SEND TO', kind: 'detent', min: 0, max: DST_NAMES.length - 1, def: 0, labels: DST_LABELS,
      get: (p) => { const s = sendSlot(p, src); return s < 0 ? 0 : p.mod[s].dst; },
      set: (p, v) => setSendDest(p, src, clamp(Math.round(v), 0, DST_NAMES.length - 1)), fmt: (v) => DST_LABELS[v] });
    def(`lfo${n}.amt`, { label: 'AMOUNT', min: -1, max: 1, def: 0.5, bipolar: true, scale: 'pm',
      get: (p) => { const s = sendSlot(p, src); return s < 0 ? 0 : p.mod[s].amount / 32767; },
      set: (p, v) => { const s = sendSlot(p, src); if (s >= 0) p.mod[s].amount = q15(v); }, fmt: fmtSigned });
  }
  // mod wheel send and velocity send (overlay)
  for (const [tag, src, label] of [['wheel', SRC.MODWHEEL, 'MOD WHEEL'], ['vel', SRC.VEL, 'VELOCITY'], ['env2', SRC.ENV2, 'ENV 2']]) {
    def(`${tag}.dest`, { label: `${label} TO`, kind: 'detent', min: 0, max: DST_NAMES.length - 1, def: 0, labels: DST_LABELS,
      get: (p) => { const s = sendSlot(p, src); return s < 0 ? 0 : p.mod[s].dst; },
      set: (p, v) => setSendDest(p, src, clamp(Math.round(v), 0, DST_NAMES.length - 1)), fmt: (v) => DST_LABELS[v] });
    def(`${tag}.amt`, { label: 'AMOUNT', min: -1, max: 1, def: 0.5, bipolar: true, scale: 'pm',
      get: (p) => { const s = sendSlot(p, src); return s < 0 ? 0 : p.mod[s].amount / 32767; },
      set: (p, v) => { const s = sendSlot(p, src); if (s >= 0) p.mod[s].amount = q15(v); }, fmt: fmtSigned });
  }
  // modulation oscillator: pitch, and ring / phase / position modulation of one oscillator each
  def('mo.semis', { label: 'PITCH', min: -48, max: 48, def: 0, bipolar: true, step: 1, scale: 'semis',
    get: (p) => p.modosc.semis, set: (p, v) => { p.modosc.semis = clamp(Math.round(v), -48, 48); },
    fmt: (v) => `${v >= 0 ? '+' : ''}${Math.round(v)} st` });
  const OSC_TARGETS = ['OFF', 'OSC 1', 'OSC 2', 'OSC 3'];
  for (const [k, label] of [['rm', 'RING MOD'], ['pm', 'PHASE MOD'], ['pos', 'WAVE MOD']]) {
    def(`mo.${k}`, { label, min: 0, max: 1, def: 0, get: (p) => p.modosc[k] / 32767, set: (p, v) => { p.modosc[k] = q15(v); }, fmt: fmtPct });
    def(`mo.${k}_osc`, { label: 'TARGET', kind: 'detent', min: 0, max: 3, def: 0, labels: OSC_TARGETS,
      get: (p) => p.modosc[`${k}_osc`], set: (p, v) => { p.modosc[`${k}_osc`] = clamp(Math.round(v), 0, 3); }, fmt: (v) => OSC_TARGETS[v] });
  }

  // the 16 matrix slots (MATRIX overlay)
  for (let i = 0; i < 16; i++) {
    def(`mod${i + 1}.src`, { label: 'SOURCE', kind: 'detent', min: 0, max: 24, def: 0, labels: SRC_LABELS,
      get: (p) => p.mod[i].src, set: (p, v) => { p.mod[i].src = clamp(Math.round(v), 0, 24); }, fmt: (v) => SRC_LABELS[v] });
    def(`mod${i + 1}.dst`, { label: 'DESTINATION', kind: 'detent', min: 0, max: DST_NAMES.length - 1, def: 0, labels: DST_LABELS,
      get: (p) => p.mod[i].dst, set: (p, v) => { p.mod[i].dst = clamp(Math.round(v), 0, DST_NAMES.length - 1); }, fmt: (v) => DST_LABELS[v] });
    def(`mod${i + 1}.amt`, { label: 'DEPTH', min: -1, max: 1, def: 0.5, bipolar: true, scale: 'pm',
      get: (p) => p.mod[i].amount / 32767, set: (p, v) => { p.mod[i].amount = q15(v); }, fmt: fmtSigned });
  }

  // --------------------------------------------------------------- utilities ---
  function get(p, id) { const d = P[id]; if (!d) throw new Error(`unknown parameter ${id}`); return d.get(p); }
  function set(p, id, v) {
    const d = P[id]; if (!d) throw new Error(`unknown parameter ${id}`);
    v = clamp(v, d.min, d.max);
    if (d.kind !== 'knob') v = Math.round(v);
    d.set(p, v);
    return d.get(p);
  }
  function format(id, v, p) { const d = P[id]; return d.fmt(d.kind === 'knob' ? v : Math.round(v), p); }   // p: the patch (delay TIME shows what plays)
  function clone(p) { return JSON.parse(JSON.stringify(p)); }
  function setName(p, name) {
    p.name = String(name || 'UNTITLED').replace(/[^\x20-\x7E]/g, '?').slice(0, 23);
    return p.name;
  }

  return {
    P, get, set, format, clone, setName, clamp, q15, uq,
    envSec, secToEnv, cutoffHz, hzToCutoff, lfoHz, hzToLfo,
    findSlot, routeAmount, setRoute, sendSlot, setSendDest, routesTo, addRoute, removeRoute, setRouteAmountAt,
    KNOB_SLOTS, KNOB_DST, SRC_SHORT, SRC_LABELS, FILTER2_SELECT: ['OFF'].concat(FILTER_SELECT),
    oscRangeIndex, setOscRange, oscFreq, setOscFreq, oscWave, setOscWave,
    FX_FIELDS, FX_UNITS, ENSEMBLE_MODES, fxAvailable, fxIsOn, fxSetOn, delayPlays, fmtDelay,
    perfShape, PERF_NAMES, SYNC_LABELS, RANGE_LABELS, RANGE_SEMIS, WAVE_NAMES, WAVETABLE_NAMES, FILTER_SELECT, FILTER_ENUM,
    VOICE_MODES, DST, DST_NAMES, DST_LABELS, SRC, W, LFO_WAVE_NAMES, SLOT_KINDS,
    WT_MOOG, OSC_ON, OSC_RESET, OSC_LO, LFO_KEYSYNC, LFO_UNIPOLAR, LFO_SYNC,
  };
});
