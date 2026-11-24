// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk Android UI tests (node, no dependencies):
//   node tests/test_panel.mjs [--no-shots]
// 1. The page's converter core copy is current (tools/sync_ui_core.py --check).
// 2. The factory bank (panel/js/factory.js): every preset is a valid .mfp v4 image (480 B) that
//    round-trips through mf_core write/read unchanged, and has a unique original name.
// 3. The panel's parameter mapping (panel_model.js) writes the intended patch fields.
// 4. If Chrome is installed: the page loads with no script errors, and screenshots at 1920x1200 (and one
//    at 1280x800) go to build/android_ui_shots/.
import { createRequire } from 'node:module';
import { execFileSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WWW = path.join(ROOT, 'panel');
const Core = require(path.join(WWW, 'js/mf_core.js'));
const M = require(path.join(WWW, 'js/panel_model.js'));
const F = require(path.join(WWW, 'js/factory.js'));

let pass = 0, fail = 0;
function check(ok, what) { if (ok) pass++; else { fail++; console.log(`FAIL: ${what}`); } }
const eqBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// ---------------------------------------------------------------- 1. sync ---
try {
  execFileSync('python', ['-I', path.join(ROOT, 'tools/sync_ui_core.py'), '--check'], { stdio: 'pipe' });
  check(true, 'sync');
} catch (e) { check(false, `panel/js copies of mf_core.js / mf_profiles.js are stale: ${String(e.stdout || e.message).trim()}`); }
const editorCore = fs.readFileSync(path.join(ROOT, 'editor/src/mf_core.js'), 'utf8');
check(fs.readFileSync(path.join(WWW, 'js/mf_core.js'), 'utf8') === editorCore, 'www/js/mf_core.js equals editor/src/mf_core.js');

// ------------------------------------------------------------ 2. factory ---
const bank = F.build(Core, M);
check(bank.length >= 12, `factory bank has >= 12 presets (${bank.length})`);
const names = new Set();
for (const p of bank) {
  const img = Core.mfpWrite(p);
  check(img.length === Core.MFP.SIZE && Core.MFP.SIZE === 508, `${p.name}: .mfp is 508 bytes (${img.length})`);
  check(Core.mfpCheck(img) === null, `${p.name}: mfpCheck ok`);
  const back = Core.mfpRead(img);
  check(eqBytes(Core.mfpWrite(back), img), `${p.name}: write(read(write(p))) == write(p)`);
  check(JSON.stringify(back) === JSON.stringify(Core.mfpRead(Core.mfpWrite(back))), `${p.name}: read is stable`);
  check(back.name === p.name && p.name.length > 0 && p.name.length <= 23, `${p.name}: name kept`);
  check(!names.has(p.name), `${p.name}: unique name`); names.add(p.name);
  check(!/moog|massive|minimoog|native instruments|roland|juno|korg/i.test(p.name), `${p.name}: original name, no brand`);
  // engine ranges (dsp/mfe_preset.c read checks)
  check(back.osc.every((o) => o.table < 7) && back.filt.every((f) => f.type < 8) && back.voice_mode <= 2, `${p.name}: enums in range`);
  check(back.lfo.every((l) => l.wave < 8 && l.sync_div < 13 && l.step_shape.every((s) => s < 32)), `${p.name}: LFO data in range`);
  check(back.mod.every((m) => m.src < 25 && m.dst < 25), `${p.name}: matrix in range`);
  check(back.osc.some((o) => (o.flags & 1) && o.amp > 0) || back.noise.amp > 0, `${p.name}: makes sound`);
}
const kinds = bank.map((p) => p.lfo.map((l) => l.wave));
check(kinds.some((w) => w.includes(6)), 'a factory preset uses a Performer');
check(kinds.some((w) => w.includes(7)), 'a factory preset uses a Stepper');
check(bank.some((p) => p.filt[0].type === 7), 'a factory preset uses the ACID filter');
check(bank.some((p) => p.filt[0].type === 6), 'a factory preset uses the LADDER filter');

// ------------------------------------------------------------ 3. mapping ---
const P0 = () => Core.defaultPatch();
let p = P0();
M.set(p, 'osc1.range', M.RANGE_LABELS.indexOf("16'"));
check(p.osc[0].semis === -12 && !(p.osc[0].flags & M.OSC_LO), "RANGE 16' -> semis -12, LO off");
M.set(p, 'osc1.range', M.RANGE_LABELS.indexOf("32'")); check(p.osc[0].semis === -24, "RANGE 32' -> -24");
M.set(p, 'osc1.range', M.RANGE_LABELS.indexOf("4'")); check(p.osc[0].semis === 12, "RANGE 4' -> +12");
M.set(p, 'osc1.range', M.RANGE_LABELS.indexOf("2'")); check(p.osc[0].semis === 24, "RANGE 2' -> +24");
M.set(p, 'osc1.range', M.RANGE_LABELS.indexOf("8'")); check(p.osc[0].semis === 0, "RANGE 8' -> 0");
M.set(p, 'osc1.range', 0); check((p.osc[0].flags & M.OSC_LO) !== 0 && p.osc[0].semis === 0, 'RANGE LO sets the LO flag');
check(M.get(p, 'osc1.range') === 0, 'LO reads back as LO');
M.set(p, 'osc1.range', 2); check(!(p.osc[0].flags & M.OSC_LO) && p.osc[0].semis === -12, 'leaving LO clears the flag');
p = P0();
M.set(p, 'osc2.range', 2); M.set(p, 'osc2.freq', 7);
check(p.osc[1].semis === -5 && p.osc[1].fine === 0, "16' + FREQUENCY +7 -> semis -5");
M.set(p, 'osc2.freq', -3.25); check(p.osc[1].semis === -15 && p.osc[1].fine === -25, 'FREQUENCY -3.25 -> semis -15 (16\' - 3), fine -25 cents');
M.set(p, 'osc2.range', 4); check(p.osc[1].semis === 9 && p.osc[1].fine === -25, "RANGE change keeps the FREQUENCY offset (4' - 3 = +9)");
check(Math.abs(M.get(p, 'osc2.freq') + 3.25) < 1e-9, 'FREQUENCY reads back');
for (let f = 0; f < 8; f++) {
  M.set(p, 'osc3.wave', f);
  check(p.osc[2].table === 6 && p.osc[2].position === Math.round(f / 7 * 32767), `WAVEFORM ${f}: CLASSIC table, position = frame/7 (${p.osc[2].position})`);
  check(M.get(p, 'osc3.wave') === f, `WAVEFORM ${f} reads back`);
}
check(P0().osc[0].position === Math.round(3 / 7 * 32767), 'INIT saw = frame 3');
const FT = { LADDER: 6, ACID: 7, LP4: 2, LP2: 1, HP: 4, BP: 3, NOTCH: 5 };
for (const [name, en] of Object.entries(FT)) {
  M.set(p, 'f1.type', M.FILTER_SELECT.indexOf(name));
  check(p.filt[0].type === en && Core.MFP.FILTERS[en] === name, `filter type ${name} -> enum ${en}`);
  check(M.format('f1.type', M.get(p, 'f1.type')) === name, `filter type ${name} reads back`);
}
M.set(p, 'f2.type', 0); check(p.filt[1].type === 0, 'filter 2 OFF -> enum 0');
M.set(p, 'f2.type', M.FILTER2_SELECT.indexOf('ACID')); check(p.filt[1].type === 7, 'filter 2 ACID -> 7');
p = P0();
M.set(p, 'f1.cutoff', 0.5); check(p.filt[0].cutoff === 16384, 'cutoff 0.5 -> Q15 16384');
check(Math.abs(M.cutoffHz(69 / 136) - 440) < 1e-9, 'cutoff note 69 = 440 Hz');
M.set(p, 'f1.res', 1); check(p.filt[0].res === 32767, 'emphasis full -> 32767');
M.set(p, 'f1.contour', 0.4);
let k = M.findSlot(p, M.SRC.ENV1, M.DST.F1_CUTOFF);
check(k >= 0 && p.mod[k].amount === Math.round(0.4 * 32767), 'amount of contour -> matrix slot ENV1 -> F1_CUTOFF');
M.set(p, 'f1.contour', 0); check(M.findSlot(p, M.SRC.ENV1, M.DST.F1_CUTOFF) < 0 && p.mod.every((m) => !m.src), 'contour 0 frees the slot');
M.set(p, 'env1.attack', M.secToEnv(0.01)); check(Math.abs(M.envSec(p.env[0].attack / 32767) - 0.01) < 0.0005, 'ENV 1 attack 10 ms');
M.set(p, 'env4.release', 0.5); check(p.env[3].release === 16384, 'ENV 4 (loudness) release');
M.set(p, 'env4.sustain', 0.25); check(p.env[3].sustain === 8192, 'ENV 4 sustain level');
M.set(p, 'env2.dec_level', 0.5); check(p.env[1].dec_level === 16384, 'ENV 2 D-LEVEL');
M.set(p, 'env3.loop', 1); check((p.env[2].flags & 1) === 1, 'ENV 3 loop flag');
M.set(p, 'osc2.on', 1); check((p.osc[1].flags & M.OSC_ON) !== 0, 'OSC 2 rocker on');
M.set(p, 'osc1.on', 0); check((p.osc[0].flags & M.OSC_ON) === 0, 'OSC 1 rocker off');
M.set(p, 'osc1.vol', 0.5); check(p.osc[0].amp === 16384, 'OSC 1 volume');
M.set(p, 'master', 0.5); check(p.master === 8192, 'master at 12 o\'clock = unity (Q13 8192)');
M.set(p, 'serpar', 0); check(p.serpar === 32767, 'SER.PAR fully serial -> serpar 1');
M.set(p, 'tune', 10); check(p.osc.every((o) => o.fine === 10), 'TUNE shifts all oscillators');
M.set(p, 'glide', 0.3); M.set(p, 'glide.on', 0); check(p.glide === 0, 'glide rocker off -> glide 0');
M.set(p, 'glide.on', 1); check(p.glide === Math.round(0.3 * 32767), 'glide rocker on restores the time');
M.set(p, 'voice', 2); check(p.voice_mode === 2, 'voice LEGATO');
M.set(p, 'macro3', 0.75); check(p.macro[2] === Math.round(0.75 * 32767), 'macro 3 value');
// modulator slots 5-8 (lfo[0..3])
p = P0();
M.set(p, 'lfo1.kind', 1); check(p.lfo[0].wave === M.W.PERFORMER, 'slot 5 tap-cycle: PERFORMER');
M.set(p, 'lfo1.kind', 2); check(p.lfo[0].wave === M.W.STEPPER, 'slot 5: STEPPER');
M.set(p, 'lfo1.kind', 0); check(p.lfo[0].wave === M.W.SINE, 'slot 5: back to LFO');
M.set(p, 'lfo2.sync', 1); M.set(p, 'lfo2.div', 5); check((p.lfo[1].flags & M.LFO_SYNC) && p.lfo[1].sync_div === 5 && M.SYNC_LABELS[5] === '1/8', 'slot 6 sync 1/8');
check(M.PERF_NAMES.length === 32 && M.PERF_NAMES[3] === 'EXP DOWN' && M.PERF_NAMES[31] === 'SPIKE', 'Performer shape names as dsp/mfe_tables.c');
check(Math.abs(M.perfShape(10, 0.5) - 1) < 1e-12 && M.perfShape(9, 0.75) === 0, 'Performer curves as perf_shape()');
// drag-to-assign
p = P0();
const r1 = M.addRoute(p, M.SRC.LFO1, M.KNOB_DST['f1.cutoff'], 0.3);
check(r1 >= 0 && p.mod[r1].src === 5 && p.mod[r1].dst === M.DST.F1_CUTOFF && p.mod[r1].amount === Math.round(0.3 * 32767), 'drop LFO 5 on CUTOFF -> matrix slot');
check(M.addRoute(p, M.SRC.LFO1, M.DST.F1_CUTOFF) === r1, 'dropping the same source again reuses its slot');
M.addRoute(p, 14, M.DST.F1_CUTOFF); M.addRoute(p, 1, M.DST.F1_CUTOFF);
check(M.addRoute(p, 9, M.DST.F1_CUTOFF) === -2, 'a knob takes three sources');
check(M.routesTo(p, M.DST.F1_CUTOFF).length === 3, 'three routes on CUTOFF');
M.setRouteAmountAt(p, r1, -0.5); check(p.mod[r1].amount === Math.round(-0.5 * 32767), 'slot depth');
M.removeRoute(p, r1); check(M.routesTo(p, M.DST.F1_CUTOFF).length === 2, 'double-tap removes the route');
check(Core.mfpCheck(Core.mfpWrite(p)) === null, 'routed patch writes');
// effects: isolated field table
check(Object.keys(M.FX_FIELDS).length > 0 && M.FX_UNITS.map((u) => u.title).join(',') === 'OCT FUZZ,TAPE,ENSEMBLE,BBD ECHO,OIL CAN,REVERB', 'six effect units in order');
p = P0();
M.set(p, 'echo.on', 1); check(p.fx.bbd_mix > 0 && p.fx.bbd_time > 0, 'BBD ECHO on');
M.set(p, 'echo.on', 0); check(p.fx.bbd_mix === 0, 'BBD ECHO off');
M.set(p, 'echo.on', 1); check(p.fx.bbd_mix === Math.round(0.5 * 32767), 'BBD ECHO back on at its level');
M.set(p, 'ensemble.mode', 2); check(p.fx.ens_mode === 2 && M.get(p, 'ensemble.mode') === 2, 'ENSEMBLE II');
M.set(p, 'ensemble.mode', 0); check(p.fx.ens_mode === 0 && M.get(p, 'ensemble.mode') === 0, 'ENSEMBLE OFF');
M.set(p, 'reverb.mix', 0.4); check(p.fx.rev_amount === Math.round(0.4 * 32767), 'REVERB amount');
M.set(p, 'reverb.type', 1); check(p.fx.rev_type === 1 && M.get(p, 'reverb.type') === 1, 'REVERB type SPRING');
M.set(p, 'tape.drive', 0.3); check(p.fx.tape_drive === Math.round(0.3 * 32767), 'TAPE drive');
M.set(p, 'oilcan.mix', 0.7); check(p.fx.oil_mix === Math.round(0.7 * 32767) && M.P['oilcan.mix'].available(), 'OIL CAN mix');
M.set(p, 'fuzz.amount', 0.6); check(p.fx.fuzz_amount === Math.round(0.6 * 32767), 'OCT FUZZ amount');
// delay tempo sync (17 Nov 2026): fx_sync nibbles, TIME shows ms or the division, longest that fits
p = P0();
check(M.get(p, 'echo.sync') === 0 && M.get(p, 'oilcan.sync') === 0 && (p.fx.fx_sync || 0) === 0, 'delays free by default');
check(M.format('oilcan.time', M.get(p, 'oilcan.time'), p) === '130 ms', `OIL CAN free time readout (${M.format('oilcan.time', M.get(p, 'oilcan.time'), p)})`);
M.set(p, 'echo.time', 0.3); check(M.delayPlays(p, 0).ms === 300, `BBD free 0.3 = 300 ms (${M.delayPlays(p, 0).ms})`);
M.set(p, 'echo.sync', 1); check(p.fx.fx_sync >> 4 === 7 && M.format('echo.time', M.get(p, 'echo.time'), p) === '1/4T · 333 ms',
  `BBD SYNC on at 300 ms: nearest division 1/4T (${p.fx.fx_sync >> 4}, ${M.format('echo.time', M.get(p, 'echo.time'), p)})`);
M.set(p, 'oilcan.div', 7); check((p.fx.fx_sync & 15) === 8 && p.fx.fx_sync >> 4 === 7 && M.get(p, 'oilcan.sync') === 1, 'OIL CAN division 1/8. turns SYNC on, BBD kept');
check(M.fmtDelay(p, 1) === '1/8. · 375 ms', `OIL CAN 1/8. at 120 BPM = 375 ms (${M.fmtDelay(p, 1)})`);
M.set(p, 'oilcan.time', 1); check(M.fmtDelay(p, 1) === '1/4* · 500 ms', `OIL CAN 2 BAR does not fit: longest that fits (${M.fmtDelay(p, 1)})`);
check(Core.mfpRead(Core.mfpWrite(p)).fx.fx_sync === p.fx.fx_sync, 'fx_sync survives the .mfp image');
M.set(p, 'oilcan.sync', 0); check((p.fx.fx_sync & 15) === 0 && M.format('oilcan.time', M.get(p, 'oilcan.time'), p).endsWith(' ms'), 'OIL CAN SYNC off: free time');
// every parameter: set default, read, format, image still valid
p = P0();
let allOk = true;
for (const id of Object.keys(M.P)) {
  try { M.set(p, id, M.P[id].def); M.format(id, M.get(p, id)); } catch (e) { allOk = false; console.log(`  ${id}: ${e.message}`); }
}
check(allOk && Core.mfpCheck(Core.mfpWrite(p)) === null, `all ${Object.keys(M.P).length} panel parameters set/get/format`);
// on-screen text: no brand names
const ui = ['index.html', 'js/ui.js', 'js/panel_model.js', 'js/factory.js'].map((f) => fs.readFileSync(path.join(WWW, f), 'utf8')).join('\n');
const strings = (ui.match(/'[^'\n]*'|"[^"\n]*"|`[^`\n]*`|>[^<\n]+</g) || []).join('\n');
check(!/\b(moog|minimoog|model d|massive|native instruments|m-vave|juno|deco|tel-ray|chroma)\b/i.test(strings.replace(/mfe_wt_moog/gi, '')), 'no brand names in UI strings');
check(/VINTAGE CHONK/.test(fs.readFileSync(path.join(WWW, 'index.html'), 'utf8')), 'panel title VINTAGE CHONK');
check(!/https?:\/\//.test(ui.replace(/http:\/\/www\.w3\.org\/2000\/svg/g, '')), 'no network URLs in the page');

// -------------------------------------------------------------- 4. shots ---
const CHROME = ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  '/usr/bin/google-chrome', '/usr/bin/chromium'].find((c) => fs.existsSync(c));
async function shots() {
  const out = path.join(ROOT, 'build/android_ui_shots');
  fs.mkdirSync(out, { recursive: true });
  const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'mfm1-ui-'));
  const proc = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${prof}`, '--no-first-run',
    '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1', 'about:blank'], { stdio: 'ignore' });
  try {
    let port = null;
    for (let i = 0; i < 100 && !port; i++) {
      await new Promise((r) => setTimeout(r, 100));
      try { port = fs.readFileSync(path.join(prof, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim(); } catch (e) { /* not yet */ }
    }
    if (!port) throw new Error('Chrome did not start');
    const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    let id = 0; const pending = new Map(); const errors = [];
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
      if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text);
      if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') errors.push(msg.params.args.map((a) => a.value).join(' '));
      if (msg.method === 'Network.requestWillBeSent' && !/^(file|data|blob|about):/.test(msg.params.request.url)) errors.push(`network request: ${msg.params.request.url}`);
    };
    const send = (method, params) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
    await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable');
    const url = pathToFileURL(path.join(WWW, 'index.html')).href;
    const list = [['panel', 1920, 1200, ''], ['panel_perf', 1920, 1200, '#p7,t4'], ['panel_stepper', 1920, 1200, '#p8,t5'], ['panel_env', 1920, 1200, '#p0,t0'],
      ['effects', 1920, 1200, '#fx'], ['matrix', 1920, 1200, '#p8,matrix'], ['panel_1280x800', 1280, 800, '#p5,t4']];
    for (const [name, w, h, hash] of list) {
      await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
      await send('Page.navigate', { url: `${url}?shot=${name}${hash}` });   // a query makes every shot a fresh load
      await new Promise((r) => setTimeout(r, 900));
      if (name === 'panel') {   // a held chord, a dragged knob tooltip
        await send('Runtime.evaluate', { expression: "document.querySelectorAll('.key')[12].classList.add('down');document.querySelectorAll('.key')[16].classList.add('down');document.querySelectorAll('.key')[19].classList.add('down');" });
      }
      const shot = await send('Page.captureScreenshot', { format: 'png' });
      fs.writeFileSync(path.join(out, `${name}.png`), Buffer.from(shot.result.data, 'base64'));
    }
    // scripted touch: drag MACRO 1's handle onto CUTOFF, drag the EMPHASIS knob, play a key
    await send('Page.navigate', { url: `${url}?shot=interact#p0` });
    await new Promise((r) => setTimeout(r, 900));
    const js = `(() => {
      const out = {}, calls = [];
      for (const k of ['noteOn', 'noteOff', 'loadPreset']) { const f = MFBridge[k]; MFBridge[k] = (...a) => { calls.push(k); return f(...a); }; }
      const ev = (el, type, x, y, id) => el.dispatchEvent(new PointerEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, pointerId: id || 7, pointerType: 'touch', isPrimary: true }));
      const mid = (el) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
      const h = [...document.querySelectorAll('.src-handle.macro')][0], cut = document.querySelector('.ctl[data-dst="15"]');
      const [hx, hy] = mid(h), [cx, cy] = mid(cut);
      ev(h, 'pointerdown', hx, hy); ev(h, 'pointermove', hx + 30, hy - 30); ev(h, 'pointermove', cx, cy); ev(h, 'pointerup', cx, cy);
      out.route = MFUI.patch.mod.some((m) => m.src === 14 && m.dst === 15);
      out.slotShown = [...cut.querySelectorAll('.mslot.full')].map((s) => s.textContent).join(',');
      const res = document.querySelector('.ctl[data-dst="17"]'), [rx, ry] = mid(res), before = MFUI.patch.filt[0].res;
      ev(res, 'pointerdown', rx, ry, 9); ev(res, 'pointermove', rx, ry - 20, 9); ev(res, 'pointermove', rx, ry - 60, 9); ev(res, 'pointerup', rx, ry - 60, 9);
      out.res = MFUI.patch.filt[0].res > before;
      const key = document.querySelectorAll('.key')[12], [kx, ky] = mid(key);
      ev(key, 'pointerdown', kx, ky + 80, 11); out.down = key.classList.contains('down'); ev(key, 'pointerup', kx, ky + 80, 11);
      out.calls = calls.join(',');
      return JSON.stringify(out);
    })()`;
    const ir = await send('Runtime.evaluate', { expression: js, returnByValue: true });
    const o = JSON.parse(ir.result.result.value || '{}');
    check(o.route === true, 'drag-to-assign: MACRO 1 dropped on CUTOFF makes a matrix slot');
    check(/M1/.test(o.slotShown || ''), `the slot under CUTOFF shows M1 (${o.slotShown})`);
    check(o.res === true, 'dragging EMPHASIS up raises the resonance');
    check(o.down === true && /noteOn/.test(o.calls) && /noteOff/.test(o.calls), `a key sends noteOn / noteOff (${o.calls})`);
    check(/loadPreset/.test(o.calls), 'edits send the patch with loadPreset');
    // zoom gestures (17 Nov 2026): double-tap empty panel, pinch on knobs, keys never zoom, FIT chip
    const zjs = `(async () => {
      const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
      const ev = (el, type, x, y, id) => el.dispatchEvent(new PointerEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, pointerId: id, pointerType: 'touch', isPrimary: id === 31 }));
      const scale = () => Number((/scale\\(([0-9.]+)\\)/.exec(document.getElementById('stage').style.transform) || [0, 0])[1]);
      const mid = (el) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
      const out = { s0: scale() };
      const foot = document.getElementById('panel-foot'), fr = foot.getBoundingClientRect();
      const fx = fr.right - 40, fy = fr.top + fr.height / 2, ft = document.elementFromPoint(fx, fy);
      ev(ft, 'pointerdown', fx, fy, 31); ev(ft, 'pointerup', fx, fy, 31); await sleep(60);
      ev(ft, 'pointerdown', fx, fy, 32); ev(ft, 'pointerup', fx, fy, 32); await sleep(400);
      out.s1 = scale();
      const chip = document.getElementById('zoom-chip'); out.chip = !chip.classList.contains('hidden');
      ev(chip, 'pointerdown', 10, 10, 33); await sleep(400); out.s2 = scale();
      // pinch with both fingers starting on knobs: zoom grows, the knob value does not move
      const k1 = document.querySelector('.ctl[data-dst="15"]'), k2 = document.querySelector('.ctl[data-dst="17"]');
      const before = JSON.stringify([MFUI.patch.filt[0].cutoff, MFUI.patch.filt[0].res]);
      const [ax, ay] = mid(k1), [bx, by] = mid(k2);
      ev(k1, 'pointerdown', ax, ay, 41); ev(k2, 'pointerdown', bx, by, 42);
      for (let i = 1; i <= 6; i++) { ev(k1, 'pointermove', ax - 20 * i, ay, 41); ev(k2, 'pointermove', bx + 20 * i, by, 42); }
      ev(k1, 'pointerup', ax - 120, ay, 41); ev(k2, 'pointerup', bx + 120, by, 42); await sleep(300);
      out.s3 = scale(); out.knobsKept = JSON.stringify([MFUI.patch.filt[0].cutoff, MFUI.patch.filt[0].res]) === before;
      ev(chip, 'pointerdown', 10, 10, 43); await sleep(400);
      // two fingers on the keyboard: a chord, no zoom
      const keys = document.querySelectorAll('.key'), [qx, qy] = mid(keys[12]), [wx, wy] = mid(keys[16]);
      const s4 = scale();
      ev(keys[12], 'pointerdown', qx, qy + 80, 51); ev(keys[16], 'pointerdown', wx, wy + 80, 52);
      ev(keys[12], 'pointermove', qx - 60, qy + 80, 51); ev(keys[16], 'pointermove', wx + 60, wy + 80, 52);
      out.chord = document.querySelectorAll('.key.down').length >= 2;
      ev(keys[12], 'pointerup', qx - 60, qy + 80, 51); ev(keys[16], 'pointerup', wx + 60, wy + 80, 52);
      out.keysNoZoom = scale() === s4;
      return JSON.stringify(out);
    })()`;
    const zr = await send('Runtime.evaluate', { expression: zjs, returnByValue: true, awaitPromise: true });
    const z = JSON.parse(zr.result.result.value || '{}');
    check(z.s1 > z.s0 * 2, `double-tap on empty panel zooms in (${z.s0} -> ${z.s1})`);
    check(z.chip === true && Math.abs(z.s2 - z.s0) < 1e-3, `FIT chip shows while zoomed and returns to fit (${z.s2})`);
    check(z.s3 > z.s0 * 1.2 && z.knobsKept === true, `pinch on knobs zooms (${z.s3}) without turning them (${z.knobsKept})`);
    check(z.chord === true && z.keysNoZoom === true, `two fingers on the keys play a chord and never zoom (${z.chord}, ${z.keysNoZoom})`);
    // .nmsv import as Java hands it back (onNmsvPicked) with a repository fixture
    const fx = fs.readFileSync(path.join(ROOT, 'tests/fixtures/fixture_saw_pluck.nmsv')).toString('base64');
    const im = await send('Runtime.evaluate', { expression: `onNmsvPicked('fixture_saw_pluck.nmsv', '${fx}'); JSON.stringify({ dlg: document.querySelector('#dialog h2') && document.querySelector('#dialog h2').textContent, n: document.querySelectorAll('.compat-k b').length, ok: MFCore.mfpCheck(MFCore.mfpWrite(MFUI.patch)) })`, returnByValue: true });
    const io = JSON.parse(im.result.result.value || '{}');
    check(io.dlg === 'IMPORTED' && io.n === 3 && io.ok === null, `.nmsv import converts, summarises and loads (${im.result.result.value})`);
    // a scripted edit and a send through the mock bridge
    const r = await send('Runtime.evaluate', { expression: "MFUI.loadEntry(0); MFUI.patch.name + '|' + MFUI.entries().length", returnByValue: true });
    check(/\|\d+$/.test(r.result.result.value), `page boots with the factory bank (${r.result.result.value})`);
    check(errors.length === 0, `no page errors (${errors.slice(0, 3).join(' / ')})`);
    ws.close();
    console.log(`screenshots: ${list.length} in ${path.relative(ROOT, out)}`);
  } finally { proc.kill(); }
}
if (CHROME && !process.argv.includes('--no-shots')) {
  try { await shots(); } catch (e) { check(false, `screenshots: ${e.message}`); }
} else console.log('screenshots skipped (no Chrome or --no-shots)');

console.log(`android UI: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
