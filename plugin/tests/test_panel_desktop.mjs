// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge (formerly Vintage Chonk) VST3: the tablet panel with the desktop bridge, in headless Chrome (the plugin's WebView2
// is Chromium too). Real browser mouse input (CDP Input.dispatchMouseEvent), no touch:
//   * plugin/web/desktop_shim.js: window.AndroidSynth over a fake JUCE backend; boot shows the host's
//     sound (MFHostInit) instead of F01; edits go out as 'patch' events with the host serial; the preset
//     entry goes out as 'ui'; host pushes (MFHost.patch / restore) land on the panel without an echo;
//     computer-keyboard notes; save / delete keep the local list.
//   * mouse: drag, ctrl-drag (fine), wheel, shift-wheel (fine), double-click = default, the keyboard.
//   * the ORBIT panel (plugin/web/orbit): state from the plugin, sliders, knobs, the orbit field (puck, nodes,
//     paths, drawing), effects, macro assign, inspectors, presets, host pushes, a dropped file, a screenshot;
//     1.2.0: LIVE INPUT as a source / impulse (gate, window), the input meter and trim, the MOTION switch,
//     park-on-drag, double-click restart, PUCK DRIFT, RATE STOP, the REGOLITH | ORBIT switch, the ATOMIC LOUNGE name.
//   node plugin/tests/test_panel_desktop.mjs
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WWW = fs.existsSync(path.join(ROOT, 'panel/index.html')) ? path.join(ROOT, 'panel') : path.join(ROOT, 'panel');
const Core = require(path.join(WWW, 'js/mf_core.js'));
const M = require(path.join(WWW, 'js/panel_model.js'));
const F = require(path.join(WWW, 'js/factory.js'));
let pass = 0, fail = 0;
function check(ok, what) { if (ok) pass++; else fail++; console.log(`${ok ? '  ok  ' : '  FAIL'} ${what}`); }

const CHROME = ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find((c) => fs.existsSync(c));
if (!CHROME) { console.log('no Chrome / Edge: skipped'); process.exit(0); }

// the host's state the plugin would inject: factory 6 with an edited cutoff, marked edited
const bank = F.build(Core, M);
const hostPatch = M.clone(bank[6]);
hostPatch.filt[0].cutoff = 12345;
const init = {
  state: { b64: Core.b64encode(Core.mfpWrite(hostPatch)), ui: { kind: 'F', name: bank[6].name, dirty: true }, serial: 7 },
  presets: [{ name: 'MY PAD', b64: Core.b64encode(Core.mfpWrite(bank[3])) }],
  status: { voices: 3, midi: 5, sampleRate: 48000, latencyMs: 2.17, midiDevices: ['HOST MIDI'] },
};
const fakeJuce = `window.__JUCE__ = { backend: { emitEvent(id, o) { (window.__sent = window.__sent || []).push({ id, o: JSON.parse(JSON.stringify(o)) }); } } };`;
const shim = fs.readFileSync(path.join(ROOT, 'plugin/web/desktop_shim.js'), 'utf8');
const userScript = `${fakeJuce}\nwindow.MFHostInit = ${JSON.stringify(init)};\n${shim}`;

const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'mfm1-desk-'));
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
  };
  const send = (method, params) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
  const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); return r.result.result.value; };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  await send('Runtime.enable'); await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: userScript });
  await send('Page.navigate', { url: pathToFileURL(path.join(WWW, 'index.html')).href });
  await sleep(1200);

  // ---- boot from the host's state
  const boot = JSON.parse(await ev(`JSON.stringify({ name: MFUI.patch.name, cut: MFUI.patch.filt[0].cutoff, led: document.getElementById('led-name').textContent,
    flag: document.getElementById('led-flag').textContent, num: document.getElementById('led-num').textContent, info: document.querySelector('#strip .status .row:nth-child(2) span').textContent,
    users: MFUI.entries().filter((e) => e.kind === 'U').map((e) => e.name), sent: (window.__sent || []).map((s) => s.o.op) })`));
  check(boot.name === bank[6].name && boot.cut === 12345, `boots with the host's sound, not F01 (${boot.name}, cutoff ${boot.cut})`);
  check(boot.num === 'F07' && boot.flag === 'EDITED', `shows the host's preset entry and edited flag (${boot.num} ${boot.flag})`);
  check(!boot.sent.includes('patch'), `boot sends no patch back (would overwrite the host's sound): ${boot.sent.join(',')}`);
  check(boot.sent.includes('hello') && boot.sent.includes('ui'), `boot says hello and reports its preset entry (${boot.sent.join(',')})`);
  check(boot.users.join() === 'MY PAD', `user presets from the plugin's folder (${boot.users.join()})`);
  check(/48\.0 kHz/.test(boot.info) && /HOST MIDI/.test(boot.info), `status line from the plugin (${boot.info})`);
  const nm = JSON.parse(await ev(`JSON.stringify({ sw: [...document.querySelectorAll('#mode-switch b')].map((b) => b.textContent + ':' + b.dataset.m),
    title: (document.querySelector('#panel-foot .title') || {}).textContent, doc: document.title })`));
  check(nm.sw.join() === 'REGOLITH:chonk,ORBIT:orbit' && nm.title === 'ATOMIC LOUNGE' && nm.doc === 'Atomic Lounge',
    `plugin names on the tablet panel: switch ${nm.sw.join(' | ')}, wordmark ${nm.title}, title ${nm.doc}`);

  // ---- mouse, through real browser input
  const box = async (sel) => JSON.parse(await ev(`(() => { const r = document.querySelector('${sel}').getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height / 2]); })()`));
  const mouse = (type, x, y, o = {}) => send('Input.dispatchMouseEvent', { type, x, y, button: o.button || (type === 'mouseMoved' ? 'none' : 'left'), buttons: o.buttons ?? (type === 'mousePressed' || (type === 'mouseMoved' && o.down) ? 1 : 0), clickCount: o.clickCount || 1, modifiers: o.modifiers || 0, deltaX: o.deltaX || 0, deltaY: o.deltaY || 0 });
  const res = () => ev('MFUI.patch.filt[0].res');
  const RES = '.ctl[data-dst="17"]';
  const [rx, ry] = await box(RES);
  await ev('window.__sent = []');
  const r0 = await res();
  await mouse('mousePressed', rx, ry); for (let i = 1; i <= 6; i++) await mouse('mouseMoved', rx, ry - i * 10, { down: true }); await mouse('mouseReleased', rx, ry - 60);
  const r1 = await res();
  await mouse('mousePressed', rx, ry, { modifiers: 2 }); for (let i = 1; i <= 6; i++) await mouse('mouseMoved', rx, ry - i * 10, { down: true, modifiers: 2 }); await mouse('mouseReleased', rx, ry - 60, { modifiers: 2 });
  const r2 = await res();
  check(r1 > r0 + 1000, `mouse drag up turns the knob (${r0} -> ${r1})`);
  check(r2 > r1 && (r2 - r1) < 0.25 * (r1 - r0), `ctrl-drag is fine (${r1 - r0} vs ${r2 - r1})`);
  await sleep(400);
  const w0 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: -100 });
  await sleep(50);
  const w1 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: -100, modifiers: 8 });
  await sleep(50);
  const w2 = await res();
  await mouse('mouseWheel', rx, ry, { deltaY: 100 });
  await sleep(50);
  const w3 = await res();
  check(w1 > w0, `wheel up turns the knob up (${w0} -> ${w1})`);
  check(w2 > w1 && (w2 - w1) < 0.5 * (w1 - w0), `shift-wheel is fine (${w1 - w0} vs ${w2 - w1})`);
  check(w3 < w2, `wheel down turns it down (${w2} -> ${w3})`);
  await sleep(400);
  await mouse('mousePressed', rx, ry, { clickCount: 1 }); await mouse('mouseReleased', rx, ry, { clickCount: 1 });
  await mouse('mousePressed', rx, ry, { clickCount: 2 }); await mouse('mouseReleased', rx, ry, { clickCount: 2 });
  const rd = await res();
  check(rd === 0, `double-click sets the default (emphasis ${rd})`);
  const [kx, ky] = JSON.parse(await ev(`(() => { const r = document.querySelectorAll('.key')[12].getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height * 0.8]); })()`));
  await mouse('mousePressed', kx, ky);
  const down = await ev(`document.querySelectorAll('.key')[12].classList.contains('down')`);
  await mouse('mouseReleased', kx, ky);
  const zoom = await ev(`document.getElementById('stage').style.transform`);
  await mouse('mouseWheel', rx, ry, { deltaY: -100, modifiers: 2 });   // ctrl+wheel: fine on the knob, never page zoom
  const zoom2 = await ev(`document.getElementById('stage').style.transform`);
  const sent = JSON.parse(await ev('JSON.stringify(window.__sent)'));
  const midi = sent.filter((s) => s.o.op === 'midi').map((s) => s.o.a.join(' '));
  check(down === true && midi.includes('144 60 ' + midi.find((m) => m.startsWith('144 60'))?.split(' ')[2]) && midi.includes('128 60 0'), `the mouse plays the keyboard: ${midi.join(' / ')}`);
  check(zoom === zoom2, 'ctrl+wheel does not zoom the panel');
  const patches = sent.filter((s) => s.o.op === 'patch');
  check(patches.length > 3 && patches.every((s) => s.o.a[1] === 7), `edits go out as patches with the host serial (${patches.length}, serial ${patches[0]?.o.a[1]})`);
  const last = Core.mfpRead(Core.b64decode(patches[patches.length - 1].o.a[0]));
  check(last.filt[0].res === (await res()), 'the last patch sent carries the knob value');

  // ---- host pushes
  const auto = M.clone(last); auto.filt[0].cutoff = 777; auto.macro[2] = 30000;
  await ev(`window.__sent = []; MFHost.patch('${Core.b64encode(Core.mfpWrite(auto))}', 9)`);
  await sleep(100);
  const ap = JSON.parse(await ev(`JSON.stringify({ cut: MFUI.patch.filt[0].cutoff, m3: MFUI.patch.macro[2], sent: window.__sent.map((s) => s.o.op) })`));
  check(ap.cut === 777 && ap.m3 === 30000 && !ap.sent.includes('patch'), `host automation lands on the panel, no echo (${JSON.stringify(ap)})`);
  const [cx, cy] = await box('.ctl[data-dst="15"]');
  await mouse('mouseWheel', cx, cy, { deltaY: -100 });
  await sleep(50);
  const after = JSON.parse(await ev('JSON.stringify(window.__sent.filter((s) => s.o.op === "patch").map((s) => s.o.a[1]))'));
  check(after.length >= 1 && after.every((s) => s === 9), `edits after a push carry the new serial (${after})`);
  await ev(`window.__sent = []; MFHost.restore({ b64: '${Core.b64encode(Core.mfpWrite(bank[2]))}', ui: { kind: 'F', name: '${bank[2].name}', dirty: false }, serial: 11 })`);
  await sleep(100);
  const rs = JSON.parse(await ev(`JSON.stringify({ name: MFUI.patch.name, num: document.getElementById('led-num').textContent, flag: document.getElementById('led-flag').textContent, sent: window.__sent.map((s) => s.o.op + ':' + (s.o.a[0] || '')) })`));
  check(rs.name === bank[2].name && rs.num === 'F03' && rs.flag === '' && !rs.sent.some((s) => s.startsWith('patch')), `host restore (program change / project load): ${JSON.stringify(rs)}`);
  check(rs.sent.some((s) => s.startsWith('ui:') && s.includes(bank[2].name)), 'the panel confirms the restored entry');

  // ---- bridge calls the panel makes synchronously
  const br = JSON.parse(await ev(`(() => {
    window.__sent = [];
    const a = AndroidSynth.savePreset('Fat/Bass*1', 'AAAA'), l1 = JSON.parse(AndroidSynth.listPresets()).map((p) => p.name);
    const d = AndroidSynth.deletePreset('MY PAD'), l2 = JSON.parse(AndroidSynth.listPresets()).map((p) => p.name);
    const st = JSON.parse(AndroidSynth.getStatus());
    AndroidSynth.pickNmsv(); AndroidSynth.openSettings();
    return JSON.stringify({ a, l1, d, l2, st: st.sampleRate, ops: window.__sent.map((s) => s.o.op + ':' + (s.o.a[0] || '')) });
  })()`));
  check(br.a === 'ok' && br.l1.includes('Fat_Bass_1') && br.d === 'ok' && !br.l2.includes('MY PAD'), `save / delete with the tablet's file-name rule: ${JSON.stringify(br)}`);
  check(br.ops.includes('save:Fat_Bass_1') && br.ops.includes('delete:MY PAD') && br.ops.includes('pick:'), `save / delete / pick reach the plugin (${br.ops})`);

  // ---- computer keyboard
  await ev('window.__sent = []');
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'a', code: 'KeyA', text: 'a' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'a', code: 'KeyA' });
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'x', code: 'KeyX', text: 'x' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'x', code: 'KeyX' });
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'j', code: 'KeyJ', text: 'j' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'j', code: 'KeyJ' });
  const kb = JSON.parse(await ev('JSON.stringify(window.__sent.filter((s) => s.o.op === "midi").map((s) => s.o.a.join(" ")))'));
  check(kb.join('|') === '144 60 100|128 60 0|144 83 100|128 83 0', `computer keyboard: A = C4, X = octave up, J = B5 (${kb.join(' | ')})`);

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  const out = path.join(ROOT, 'plugin/build/test_out');
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'panel_desktop_chrome.png'), Buffer.from(shot.result.data, 'base64'));
  check(errors.length === 0, `no page errors (${errors.slice(0, 3).join(' / ')})`);
  ws.close();
} catch (e) {
  check(false, `desktop panel test: ${e.message}`);
} finally { proc.kill(); }
// ================================================================================ the ORBIT panel ===
// plugin/web/orbit/orbit.html in headless Chrome with a fake JUCE backend (as the plugin's WebView2 sees it):
// the state the plugin pushes (a fixture the host test writes from the real model: build*/test_out/
// orbit_panel_state.json, else plugin/tests/orbit_panel_state.json), then real mouse / keyboard input, and the
// events that go back to the plugin.
async function orbitPanel() {
  const cands = ['plugin/build-dev/test_out/orbit_panel_state.json', 'plugin/build/test_out/orbit_panel_state.json', 'plugin/tests/orbit_panel_state.json',
    'plugin/test_out_saved/build-dev/test_out/orbit_panel_state.json', 'plugin/test_out_saved/build/test_out/orbit_panel_state.json']
    .map((p) => path.join(ROOT, p)).filter((p) => fs.existsSync(p))
    .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);   // the newest host test run's fixture
  if (!cands.length) { check(false, 'ORBIT panel: no state fixture (run the host test first)'); return; }
  const fixture = JSON.parse(fs.readFileSync(cands[0], 'utf8'));
  // a fixture from before 1.2.0: the 1.2.0 host parameters and settings as the plugin adds them
  const add12 = [['orbit_motion', 'ORBIT Motion', fixture.settings.path ? 1 : 0], ['orbit_puck_drift', 'ORBIT Puck Drift', 0], ['orbit_input_trim', 'ORBIT Input Trim', 0.5]];
  for (let l = 1; l <= 3; l++) add12.push([`orbit_l${l}_src_input`, `ORBIT L${l} Source Input`, 0]);
  for (let l = 1; l <= 3; l++) add12.push([`orbit_l${l}_imp_input`, `ORBIT L${l} Impulse Input`, 0]);
  for (let l = 1; l <= 3; l++) add12.push([`orbit_l${l}_input_gate`, `ORBIT L${l} Input Gate`, 0]);
  const had12 = fixture.host.some((h) => h.id === 'orbit_motion');
  add12.forEach(([id, name, v], k) => { if (!fixture.host.some((h) => h.id === id)) { fixture.host.push({ id, name, target: 2000 + k, map: 'special' }); fixture.hostValues.push(v); } });
  if (!fixture.settings.input) fixture.settings.input = { trim: 0, layers: [0, 1, 2].map(() => ({ src: false, imp: false, gate: 0, window: 0.25 })) };
  if (!fixture.settings.shape) fixture.settings.shape = fixture.settings.path || 1;
  console.log(`  ORBIT fixture: ${path.relative(ROOT, cands[0])}${had12 ? '' : ' (1.2.0 parameters added by the test)'}`);
  const prof2 = fs.mkdtempSync(path.join(os.tmpdir(), 'mfm1-orbit-'));
  const proc2 = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${prof2}`, '--no-first-run',
    '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1', 'about:blank'], { stdio: 'ignore' });
  try {
    let port = null;
    for (let i = 0; i < 100 && !port; i++) {
      await new Promise((r) => setTimeout(r, 100));
      try { port = fs.readFileSync(path.join(prof2, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim(); } catch (e) { /* not yet */ }
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
    };
    const send = (method, params) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
    const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.result.exceptionDetails) errors.push(r.result.exceptionDetails.exception?.description); return r.result.result.value; };
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    await send('Runtime.enable'); await send('Page.enable');
    await send('Emulation.setDeviceMetricsOverride', { width: 1600, height: 1000, deviceScaleFactor: 1, mobile: false });
    // the plugin answers 'o.hello' with ORB.state (PluginEditor::pushOrbitState)
    const fakeJuce = `window.__JUCE__ = { backend: { emitEvent(id, o) { (window.__sent = window.__sent || []).push({ id, o: JSON.parse(JSON.stringify(o)) });
      if (o.op === 'o.hello') setTimeout(() => window.ORB && ORB.state(window.__FIX), 30); } } }; window.__FIX = ${JSON.stringify(fixture)};`;
    const shim = fs.readFileSync(path.join(ROOT, 'plugin/web/desktop_shim.js'), 'utf8');
    await send('Page.addScriptToEvaluateOnNewDocument', { source: `${fakeJuce}\nwindow.MFHostInit = {};\n${shim}` });
    await send('Page.navigate', { url: pathToFileURL(path.join(ROOT, 'plugin/web/orbit/orbit.html')).href });
    for (let i = 0; i < 50 && !(await ev('document.documentElement.dataset.ready === "1"')); i++) await sleep(100);

    const ops = async () => JSON.parse(await ev('JSON.stringify((window.__sent || []).map((s) => s.o))'));
    const clear = () => ev('window.__sent = []');
    const S = fixture.settings, H = fixture.host;
    const hidx = (id) => H.findIndex((h) => h.id === id);
    const LP = (l, p) => 24 + l * 42 + p;
    const boot = JSON.parse(await ev(`JSON.stringify({ sent: window.__sent.map((s) => s.o.op), name: document.querySelectorAll('.vfd')[0].textContent,
      src: [...document.querySelectorAll('.card .lbl.clk')].map((e) => e.textContent).slice(0, 2), paths: [...document.querySelectorAll('.win.wave svg path')].filter((p) => (p.getAttribute('d') || '').length > 50).length,
      nix: document.querySelectorAll('.nix').length })`));
    check(boot.sent[0] === 'o.hello', `the ORBIT page says hello to the plugin (${boot.sent.join(',')})`);
    check(boot.name === S.preset.name.toUpperCase(), `shows the plugin's preset (${boot.name})`);
    check(boot.paths >= 5, `waveform windows draw the loaded sources / impulses (${boot.paths} of 6 drawn)`);
    check(!boot.sent.includes('o.host') && !boot.sent.includes('o.set'), 'boot sends no edits back');

    const mouse = (type, x, y, o = {}) => send('Input.dispatchMouseEvent', { type, x, y, button: o.button || (type === 'mouseMoved' ? 'none' : 'left'), buttons: o.buttons ?? (type === 'mousePressed' || (type === 'mouseMoved' && o.down) ? 1 : 0), clickCount: o.clickCount || 1, modifiers: o.modifiers || 0, deltaX: o.deltaX || 0, deltaY: o.deltaY || 0 });
    const center = async (expr) => JSON.parse(await ev(`(() => { const el = (${expr}); if (!el) return JSON.stringify([-1, -1]); el.scrollIntoView({ block: 'nearest' }); const r = el.getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height / 2]); })()`));
    const drag = async (x, y, dx, dy, steps = 8, mods = 0) => { await mouse('mousePressed', x, y, { modifiers: mods }); for (let i = 1; i <= steps; i++) await mouse('mouseMoved', x + dx * i / steps, y + dy * i / steps, { down: true, modifiers: mods }); await mouse('mouseReleased', x + dx, y + dy, { modifiers: mods }); };
    const click = async (x, y) => { await mouse('mousePressed', x, y); await mouse('mouseReleased', x, y); };

    // matrix slider: layer 1 TONE up
    await clear();
    const [sx, sy] = await center(`document.querySelectorAll('.card')[0].querySelectorAll('.slot')[1]`);
    await drag(sx, sy, 0, -30);
    let o = await ops();
    const tone = o.filter((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_l1_tone'));
    check(tone.length > 2 && tone[tone.length - 1].a[1] > fixture.hostValues[hidx('orbit_l1_tone')], `matrix slider drag sends L1 TONE up (${tone.length} sends, ${fixture.hostValues[hidx('orbit_l1_tone')].toFixed(2)} -> ${tone.length ? tone[tone.length - 1].a[1].toFixed(2) : '-'})`);
    const nixT = await ev(`document.querySelectorAll('.card')[0].querySelectorAll('.nix.sm')[1].textContent`);
    check(nixT === String(Math.round(tone[tone.length - 1].a[1] * 100)).padStart(2, '0').slice(-2), `its nixie reads the value (${nixT})`);
    // cutoff knob: drag, ctrl-drag (fine), wheel, double-click (default)
    await clear();
    const [kx, ky] = await center(`document.querySelectorAll('.card')[1].querySelectorAll('svg.k')[1]`);
    await drag(kx, ky, 0, -40);
    const c1 = (await ops()).filter((x) => x.op === 'o.host').map((x) => x.a[1]);
    await clear();
    await drag(kx, ky, 0, -40, 8, 2);
    const c2 = (await ops()).filter((x) => x.op === 'o.host').map((x) => x.a[1]);
    await clear();
    await mouse('mouseWheel', kx, ky, { deltaY: 100 });
    const c3 = (await ops()).filter((x) => x.op === 'o.host').map((x) => x.a[1]);
    const d1 = c1[c1.length - 1] - fixture.hostValues[hidx('orbit_l2_cutoff')], d2 = c2[c2.length - 1] - c1[c1.length - 1];
    check(c1.length && d1 > 0.1 && d2 > 0 && d2 < 0.3 * d1 && c3.length === 1 && c3[0] < c2[c2.length - 1],
      `L2 cutoff knob: drag ${d1.toFixed(3)}, ctrl-drag fine ${d2.toFixed(3)}, wheel down ${c3[0] && c3[0].toFixed(3)}`);
    const cutTxt = await ev(`document.querySelectorAll('.card')[1].querySelectorAll('.sub.c')[1].textContent`);
    check(/Hz/.test(cutTxt), `cutoff readout ${cutTxt}`);
    // orbit field: HOLD, click the field -> the puck goes there
    await clear();
    const [hx, hy] = await center(`[...document.querySelectorAll('.pill.or b')][0]`);
    await click(hx, hy);
    const [fx, fy] = await center(`document.getElementById('field')`);
    await drag(fx - 60, fy - 40, 20, 10, 4);
    o = await ops();
    const px = o.filter((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_puck_x')), py = o.filter((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_puck_y'));
    check(o.some((x) => x.op === 'o.path' && x.a[0] === 0) && px.length && py.length && px[px.length - 1].a[1] < 0.5 && py[py.length - 1].a[1] > 0.5,
      `HOLD + drag on the field moves the puck (x ${px.length && px[px.length - 1].a[1].toFixed(2)}, y ${py.length && py[py.length - 1].a[1].toFixed(2)})`);
    // drag node 2
    await clear();
    const [nx, ny] = await center(`document.querySelectorAll('#field .node')[1]`);
    await drag(nx, ny, -25, 20, 5);
    o = await ops();
    check(o.some((x) => x.op === 'o.set' && x.a[0] === 4) && o.some((x) => x.op === 'o.set' && x.a[0] === 5), 'dragging node 2 moves NODE2 X / Y');
    // CIRCLE, then DRAWN + a hand-drawn loop
    await clear();
    const pills = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('.pill.or b')].map((b) => { const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; }))`));
    await click(...pills[1]);
    const circ = await ev(`document.querySelector('#field path').getAttribute('d').length`);
    await click(...pills[3]);
    await ev(`window.ORB.state(Object.assign({}, window.__FIX, { settings: Object.assign({}, window.ORB._debug().S, { drawn: [] , path: 3 }) }))`);
    await clear();
    await mouse('mousePressed', fx + 60, fy);
    for (let k = 1; k <= 36; k++) { const t = k / 36 * 2 * Math.PI; await mouse('mouseMoved', fx + 60 * Math.cos(t), fy + 40 * Math.sin(t), { down: true }); }
    await mouse('mouseReleased', fx + 60, fy);
    o = await ops();
    const drawn = o.find((x) => x.op === 'o.drawn');
    check(circ > 200 && drawn && drawn.a[0].length >= 8 && drawn.a[0].length <= 64, `CIRCLE draws its path; DRAWN + a hand-drawn loop sends o.drawn (${drawn ? drawn.a[0].length : 0} points)`);
    // sync, direction, rate text
    await clear();
    const syncB = await center(`[...document.querySelectorAll('.pill')].find((p) => p.textContent.includes('FREE')).children[1]`);
    await click(...syncB);
    const rateTxt = await ev(`[...document.querySelectorAll('.card')][3].querySelectorAll('.sub.c')[0].textContent`);
    o = await ops();
    check(o.some((x) => x.op === 'o.sync' && x.a[0] === false) && /Hz|s$/.test(rateTxt), `SYNC FREE sends o.sync, the rate reads in time (${rateTxt})`);
    // effects: jewel on/off, ensemble mode
    await clear();
    const j0 = await center(`document.querySelectorAll('.cup .jewel')[0]`);
    await click(...j0);
    const e2 = await center(`document.querySelectorAll('.cup .lbl')[2]`);
    await click(...e2);
    o = await ops();
    check(o.some((x) => x.op === 'o.fx' && x.a[0] === 0 && x.a[1] === !S.fx.on[0]) && o.some((x) => x.op === 'o.fxopt' && x.a[0].ens),
      `effects: OCT FUZZ on/off, ENSEMBLE mode (${o.map((x) => x.op).join(',')})`);
    // macro assign: click macro 2's number, then the L3 GRAIN slider
    await clear();
    const m2 = await center(`document.querySelectorAll('.nix.sm.clk')[1]`);
    await click(...m2);
    const bar = await ev(`!document.getElementById('assignBar').classList.contains('hidden')`);
    const [gx, gy] = await center(`document.querySelectorAll('.card')[2].querySelectorAll('.slot')[2]`);
    await click(gx, gy);
    o = await ops();
    const asg = o.find((x) => x.op === 'o.assign');
    check(bar && asg && asg.a[0] === 2 && asg.a[1] === LP(2, 4), `macro assign: macro 2 then L3 GRAIN -> o.assign ${asg ? JSON.stringify(asg.a) : '-'}`);
    check(!o.some((x) => x.op === 'o.host'), 'the click that assigns does not move the slider');
    // source: SYNTH inspector, pick a Chonk preset
    await clear();
    const srcS = await center(`document.querySelectorAll('.card')[0].querySelectorAll('.pill')[0].children[1]`);
    await click(...srcS);
    const insp = await ev(`!!document.getElementById('insp') && !document.getElementById('insp').classList.contains('hidden')`);
    const item = await center(`[...document.querySelectorAll('#insp .list div')].find((d) => d.textContent.includes('SILK STRINGS'))`);
    await click(...item);
    o = await ops();
    check(insp && o.some((x) => x.op === 'o.src' && x.a[0] === 0 && x.a[1].patch && x.a[1].patch.name === 'Silk Strings'),
      `source inspector: a Chonk preset as the synth source (${o.map((x) => x.op + (x.a[1] && x.a[1].patch ? ':' + x.a[1].patch.name : '')).join(',')})`);
    // impulse: factory impulse list
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape' });
    await clear();
    const impW = await center(`document.querySelectorAll('.card')[1].querySelectorAll('.win.wave')[1]`);
    await click(...impW);
    const fac = await center(`[...document.querySelectorAll('#insp .btn')].find((b) => b.textContent === 'FACTORY')`);
    await click(...fac);
    const gl = await center(`[...document.querySelectorAll('#insp .list div')].find((d) => d.textContent.includes('PLATE'))`);
    await click(...gl);
    o = await ops();
    check(o.some((x) => x.op === 'o.imp' && x.a[0] === 1 && x.a[1].gen === 1), `impulse inspector: factory impulse PLATE for layer 2 (${o.map((x) => x.op).join(',')})`);
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape' });
    // filter type menu
    await clear();
    const ft = await center(`document.querySelectorAll('.card')[0].querySelector('.abs.clk')`);
    await click(...ft);
    const band = await center(`[...document.querySelectorAll('#fmenu .list div')].find((d) => d.textContent === 'SVF BAND')`);
    await click(...band);
    o = await ops();
    check(o.some((x) => x.op === 'o.set' && x.a[0] === LP(0, 14) && x.a[1] === 3), 'filter type menu: SVF BAND on layer 1');
    // presets: browse + load factory 3, save
    await clear();
    const br = await center(`[...document.querySelectorAll('.hbtn')].find((b) => b.textContent === 'BROWSE')`);
    await click(...br);
    const f3 = await center(`document.querySelector('#browse .list div:nth-child(3)')`);
    await click(...f3);
    const sv = await center(`[...document.querySelectorAll('.hbtn')].find((b) => b.textContent === 'SAVE')`);
    await click(...sv);
    await sleep(50);
    await ev(`document.querySelector('#save input').value = 'MY ORBIT'`);
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter' });
    o = await ops();
    check(o.some((x) => x.op === 'o.preset' && x.a[0] === 'F' && x.a[1] === 2) && o.some((x) => x.op === 'o.save' && x.a[0] === 'MY ORBIT'), `browse loads factory 3, save sends the name (${o.map((x) => x.op).join(',')})`);
    // the plugin pushes automation and meters: the panel follows, no echo
    await clear();
    const ti = hidx('orbit_l1_body');
    await ev(`ORB.values({ ${ti}: 0.13 }); ORB.meter({ cpu: 0.11, voices: 6, rate: 48000, latencyMs: 7.23, puck: [0.38, 0.61], w: [0.42, 0.18, 0.31, 0.09], vu: [0.2, 0.2], pk: [0.5, 0.5], midi: 3 })`);
    await sleep(50);
    const st = JSON.parse(await ev(`JSON.stringify({ nix: document.querySelectorAll('.card')[0].querySelectorAll('.nix.sm')[0].textContent,
      puck: document.querySelector('#field .puck').getAttribute('transform'), stat: document.querySelector('.abs[style*="1288px"]').textContent,
      blend: [...document.querySelectorAll('.card')[3].querySelectorAll('.nix:not(.sm)')].map((n) => n.textContent).join(' ') })`));
    o = await ops();
    check(st.nix === '13' && o.length === 0, `host automation lands on the panel, no echo (L1 BODY nixie ${st.nix}, ${o.length} sends)`);
    check(/translate/.test(st.puck) && /VOICES 06\/48/.test(st.stat) && /CPU 11/.test(st.stat) && st.blend === '42 18 31 _9'.replace('_', '0'),
      `meters: puck ${st.puck}, status "${st.stat}", blend ${st.blend}`);
    // ---- 1.2.0: names
    const nm2 = JSON.parse(await ev(`JSON.stringify({ word: [...document.querySelectorAll('.band ~ .abs')].map((e) => e.textContent).find((t) => /LOUNGE/.test(t)) || '',
      modes: [...document.querySelectorAll('#modesw .modeb')].map((e) => e.textContent),
      text: [...document.querySelectorAll('#body *')].filter((e) => !e.closest('.card .lbl.clk, .card .sub') && e.children.length === 0).map((e) => e.textContent).join(' ') })`));
    check(/ATOMIC\s*LOUNGE/.test(nm2.word.replace(/\u2009/g, ' ')) && nm2.modes.join('|') === 'REGOLITH|ORBIT' && !/CHONK/i.test(nm2.text),
      `ORBIT panel names: wordmark ${nm2.word}, switch ${nm2.modes.join(' | ')}, no CHONK on the panel`);
    // ---- LIVE INPUT as layer 1's source: the Source Input host parameter, the inspector's gate
    const esc = () => send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape' });
    await esc(); await clear();
    await click(...await center(`document.querySelectorAll('.card')[0].querySelectorAll('.pill')[0].children[2]`));
    o = await ops();
    const si1 = hidx('orbit_l1_src_input');
    const lab1 = JSON.parse(await ev(`JSON.stringify({ name: document.querySelectorAll('.card')[0].querySelector('.lbl.clk').textContent,
      on: [...document.querySelectorAll('.card')[0].querySelectorAll('.pill')[0].children].findIndex((b) => b.classList.contains('on')),
      insp: [...document.querySelectorAll('#insp .btn.on')].map((b) => b.textContent) })`));
    check(o.some((x) => x.op === 'o.host' && x.a[0] === si1 && x.a[1] === 1) && !o.some((x) => x.op === 'o.src'),
      `SOURCE INPUT sends ORBIT L1 Source Input = 1 (${o.map((x) => x.op + JSON.stringify(x.a)).join(' ')})`);
    check(lab1.name === 'LIVE INPUT' && lab1.on === 2 && lab1.insp.includes('LIVE INPUT') && lab1.insp.includes('THRU'),
      `layer 1 shows LIVE INPUT, its pill and the inspector's LIVE INPUT + THRU (${JSON.stringify(lab1)})`);
    await clear();
    await click(...await center(`[...document.querySelectorAll('#insp .btn')].find((b) => b.textContent === 'NOTE')`));
    o = await ops();
    check(o.some((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_l1_input_gate') && x.a[1] === 1)
      && /NOTE GATE/.test(await ev(`document.querySelectorAll('.card')[0].querySelector('.sub').textContent`)), 'GATE NOTE sends ORBIT L1 Input Gate = 1 and the layer reads NOTE GATE');
    // back to SYNTH: the input flag goes off first, then the source as before
    await esc(); await clear();
    await click(...await center(`document.querySelectorAll('.card')[0].querySelectorAll('.pill')[0].children[1]`));
    o = await ops();
    const offAt = o.findIndex((x) => x.op === 'o.host' && x.a[0] === si1 && x.a[1] === 0), srcAt = o.findIndex((x) => x.op === 'o.src');
    check(offAt >= 0 && srcAt > offAt, `SYNTH after INPUT: Source Input = 0, then o.src (${o.map((x) => x.op).join(',')})`);
    // LIVE INPUT as layer 2's impulse, a 500 ms window
    await esc(); await clear();
    await click(...await center(`document.querySelectorAll('.card')[1].querySelectorAll('.pill')[1].children[3]`));
    await click(...await center(`[...document.querySelectorAll('#insp .btn')].find((b) => b.textContent === '500 MS')`));
    o = await ops();
    const impSub = await ev(`document.querySelectorAll('.card')[1].querySelector('.sub.c').textContent`);
    check(o.some((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_l2_imp_input') && x.a[1] === 1) && o.some((x) => x.op === 'o.window' && x.a[0] === 1 && x.a[1] === 0.5)
      && /500 ms WINDOW/.test(impSub), `IMPULSE INPUT: ORBIT L2 Impulse Input = 1, o.window [1, 0.5], reads "${impSub}"`);
    await esc();
    // the input meter: no bus, then a stereo signal with a peak
    const inm = async (m) => { await ev(`ORB.meter(${JSON.stringify(Object.assign({ cpu: 0.1, voices: 0, rate: 48000, latencyMs: 7.2, puck: [0, 0], w: [0.25, 0.25, 0.25, 0.25], vu: [0, 0], pk: [0, 0], midi: 3 }, m))})`); await ev(`ORB.meter(${JSON.stringify(Object.assign({ cpu: 0.1, voices: 0, rate: 48000, latencyMs: 7.2, puck: [0, 0], w: [0.25, 0.25, 0.25, 0.25], vu: [0, 0], pk: [0, 0], midi: 3 }, m))})`); };
    const inState = () => ev(`JSON.stringify({ none: getComputedStyle(document.querySelector('#inmeter .abs')).display !== 'none', bars: [...document.querySelectorAll('#inmeter .inbar i')].map((i) => i.style.transform),
      shown: [...document.querySelectorAll('#inmeter .inbar')].map((b) => b.style.display !== 'none'), peak: document.querySelector('#inmeter .jewel').classList.contains('on'),
      hint: document.querySelectorAll('.card')[1].querySelectorAll('.win.wave')[1].querySelector('.hint').textContent })`);
    for (let k = 0; k < 10; k++) await inm({ in: [0, 0, 0], inCh: 0 });
    const n0 = JSON.parse(await inState());
    for (let k = 0; k < 10; k++) await inm({ in: [0.5, 0.25, 0.95], inCh: 2 });
    const n2 = JSON.parse(await inState());
    const sub2 = await ev(`[...document.querySelectorAll('.card')[4].querySelectorAll('.sub.c')].map((e) => e.textContent).find((t) => /STEREO|MONO|NO INPUT/.test(t)) || ''`);
    check(n0.none && n0.shown.every((x) => !x) && n0.hint === 'NO INPUT BUS', `input meter without a bus: NO INPUT BUS (${JSON.stringify(n0)})`);
    check(!n2.none && n2.shown.every((x) => x) && n2.peak && n2.bars.every((b) => /scaleY\(0\.[5-9]/.test(b) || /scaleY\(1/.test(b)) && /STEREO/.test(sub2) && n2.hint === '',
      `input meter, stereo at -6 / -12 dBFS with a peak: ${n2.bars.join(' ')}, "${sub2}"`);
    // TRIM knob and PUCK DRIFT knob: their host parameters
    await clear();
    const [tx, ty] = await center(`document.getElementById('intrim')`);
    await mouse('mouseWheel', tx, ty, { deltaY: -100 });
    const [dx, dy] = await center(`document.querySelector('svg.puckdrift')`);
    await mouse('mouseWheel', dx, dy, { deltaY: -100 });
    o = await ops();
    const tr = o.find((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_input_trim')), pd = o.find((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_puck_drift'));
    check(tr && tr.a[1] > 0.5 && /\+1\.2 dB/.test(await ev(`document.getElementById('intrim').parentElement.querySelector('.sub.c[style*="180px"]').textContent`)),
      `TRIM wheel up: ORBIT Input Trim ${tr && tr.a[1].toFixed(3)} (+1.2 dB)`);
    check(pd && pd.a[1] > 0, `PUCK DRIFT wheel up: ORBIT Puck Drift ${pd && pd.a[1].toFixed(3)}`);
    // MOTION: the switch, park on drag, double-click restarts
    const mot = () => ev(`document.querySelector('#motionsw b').textContent`);
    await clear();
    await click(...pills[0]);                 // HOLD: motion off
    const m0 = await mot();
    await click(...await center(`document.getElementById('motionsw')`));
    const m1 = await mot();
    o = await ops();
    check(m0 === 'OFF' && m1 === 'ON' && o.some((x) => x.op === 'o.path' && x.a[0] === 0) && o.some((x) => x.op === 'o.motion' && x.a[0] === true),
      `MOTION switch: HOLD = OFF, the switch turns it ON (o.motion true) (${m0} -> ${m1})`);
    await click(...pills[1]);                 // CIRCLE (the last shape was DRAWN, where a drag on the field draws)
    await clear();
    await drag(fx + 50, fy - 30, 10, 5, 3);
    o = await ops();
    const pk = o.find((x) => x.op === 'o.park');
    check(pk && o[0].op === 'o.park' && pk.a[0] > 0 && pk.a[1] > 0 && (await mot()) === 'OFF' && o.some((x) => x.op === 'o.host' && x.a[0] === hidx('orbit_puck_x')),
      `dragging the moving puck parks it (o.park ${pk ? JSON.stringify(pk.a) : '-'}), motion OFF, then the drag moves it`);
    await clear();
    await mouse('mousePressed', fx - 40, fy + 20, { clickCount: 1 }); await mouse('mouseReleased', fx - 40, fy + 20, { clickCount: 1 });
    await mouse('mousePressed', fx - 40, fy + 20, { clickCount: 2 }); await mouse('mouseReleased', fx - 40, fy + 20, { clickCount: 2 });
    o = await ops();
    check(o.some((x) => x.op === 'o.motion' && x.a[0] === true) && (await mot()) === 'ON', `double-click on the field restarts motion (${o.map((x) => x.op).join(',')})`);
    // the host leads: DAW automation of ORBIT Motion and of Layer 3's Source Input lands on the panel, no echo
    await clear();
    await ev(`ORB.values({ ${hidx('orbit_motion')}: 0, ${hidx('orbit_l3_src_input')}: 1, ${hidx(H.find((h) => h.target === 1000) ? H.find((h) => h.target === 1000).id : 'orbit_rate')}: 0 })`);
    const au = JSON.parse(await ev(`JSON.stringify({ mot: document.querySelector('#motionsw b').textContent, l3: document.querySelectorAll('.card')[2].querySelector('.lbl.clk').textContent,
      rate: document.querySelectorAll('.card')[3].querySelectorAll('.sub.c')[0].textContent })`));
    o = await ops();
    check(au.mot === 'OFF' && au.l3 === 'LIVE INPUT' && o.length === 0, `automation: Motion OFF, L3 Source Input ON reach the panel, no echo (${JSON.stringify(au)})`);
    check(au.rate === 'STOP', `RATE at its minimum reads STOP (sync free): ${au.rate}`);
    // drop a file on layer 3's source window: read in the page, sent in chunks
    await clear();
    await ev(`(() => { const w = document.querySelectorAll('.card')[2].querySelectorAll('.win.wave')[0]; const dt = new DataTransfer();
      dt.items.add(new File([new Uint8Array(700000).fill(7)], 'Rain Stick.wav', { type: 'audio/wav' }));
      w.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); })()`);
    await sleep(400);
    o = await ops();
    const chunks = o.filter((x) => x.op === 'o.dropChunk');
    const bytes = chunks.reduce((a, x) => a + Buffer.from(x.a[0], 'base64').length, 0);
    check(o[0] && o[0].op === 'o.dropBegin' && o[0].a[0] === 2 && o[0].a[1] === 'src' && bytes === 700000 && o[o.length - 1].op === 'o.dropEnd',
      `a dropped WAV goes to the plugin (${chunks.length} chunks, ${bytes} bytes)`);
    // the computer keyboard plays ORBIT too (desktop shim), and CHONK mode is one click
    await clear();
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'a', code: 'KeyA', text: 'a' });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'a', code: 'KeyA' });
    const ch = await center(`document.querySelector('.modeb.off')`);
    await click(...ch);
    o = await ops();
    check(o.some((x) => x.op === 'midi' && x.a[0] === 0x90 && x.a[1] === 60) && o.some((x) => x.op === 'o.mode' && x.a[0] === 'chonk'), `computer keyboard notes and the REGOLITH switch (${o.map((x) => x.op).join(',')})`);
    // a clean screenshot of the panel as the plugin shows it (state as pushed, a chord's meters)
    await send('Page.navigate', { url: pathToFileURL(path.join(ROOT, 'plugin/web/orbit/orbit.html')).href });
    for (let i = 0; i < 50 && !(await ev('document.documentElement.dataset.ready === "1"')); i++) await sleep(100);
    await ev(`ORB.meter({ cpu: 0.11, voices: 6, rate: 48000, latencyMs: 7.23, puck: [0.38, 0.61], w: [0.42, 0.18, 0.31, 0.09], vu: [0.12, 0.12], pk: [0.4, 0.4], midi: 3 }); document.getElementById('toast').classList.add('hidden')`);
    for (let k = 0; k < 40; k++) { const t = 4.2 + k * 0.012; await ev(`ORB.meter({ cpu: 0.11, voices: 6, rate: 48000, latencyMs: 7.23, puck: [${(0.05 + 0.55 * Math.cos(t)).toFixed(3)}, ${(-0.05 + 0.5 * Math.sin(t)).toFixed(3)}], w: [0.42, 0.18, 0.31, 0.09], vu: [0.12, 0.12], pk: [0.4, 0.4], midi: 3, in: [${(0.1 + 0.06 * Math.sin(k)).toFixed(3)}, ${(0.08 + 0.05 * Math.cos(k)).toFixed(3)}, 0.3], inCh: 2 })`); }
    await sleep(150);
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    // next to a live host-test fixture; never into plugin/test_out_saved (kept screenshots for the guide)
    const out = cands[0].includes('test_out') && !cands[0].includes('test_out_saved') ? path.dirname(cands[0]) : path.join(ROOT, 'plugin/build-dev/test_out');
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, 'orbit_panel_chrome.png'), Buffer.from(shot.result.data, 'base64'));
    check(errors.length === 0, `ORBIT panel: no page errors (${errors.slice(0, 3).join(' / ')})`);
    ws.close();
  } catch (e) {
    check(false, `ORBIT panel test: ${e.message}`);
  } finally { proc2.kill(); }
}
await orbitPanel();
console.log(`desktop panel: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
