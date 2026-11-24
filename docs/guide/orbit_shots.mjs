// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk user guide: screenshots of the ORBIT panel (plugin/web/orbit, the page the plugin shows) for
// the ORBIT chapters. The page runs in headless Chrome with a fake JUCE backend that answers 'o.hello' with
// the state the plugin itself pushes (the host test writes it from the real model and runner:
// plugin/build*/test_out/orbit_panel_state.json, the factory preset RUSTED SATELLITE). Real mouse clicks open
// the inspectors and lists. Called by docs/guide/build_guide.py; writes PNGs to the folder given.
//   node docs/guide/orbit_shots.mjs <out dir>
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.resolve(process.argv[2] || path.join(ROOT, 'build/guide/orbit_raw'));
const CHROME = ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find((c) => fs.existsSync(c));
if (!CHROME) { console.log('no Chrome / Edge'); process.exit(1); }
const cands = ['plugin/build/test_out/orbit_panel_state.json', 'plugin/build-dev/test_out/orbit_panel_state.json', 'plugin/test_out_saved/build/test_out/orbit_panel_state.json', 'plugin/test_out_saved/build-dev/test_out/orbit_panel_state.json']
  .map((p) => path.join(ROOT, p)).filter((p) => fs.existsSync(p)).sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);
if (!cands.length) { console.log('no orbit_panel_state.json: run the host test first (plugin\\build.ps1 -Test)'); process.exit(1); }
const fixture = JSON.parse(fs.readFileSync(cands[0], 'utf8'));
fs.mkdirSync(OUT, { recursive: true });

const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vc-guide-orbit-'));
const proc = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${prof}`, '--no-first-run',
  '--disable-gpu', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
let code = 0;
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
  // the panel's own design size, drawn at 2x for print
  await send('Emulation.setDeviceMetricsOverride', { width: 1600, height: 1000, deviceScaleFactor: 2, mobile: false });
  const fakeJuce = `window.__JUCE__ = { backend: { emitEvent(id, o) { if (o.op === 'o.hello') setTimeout(() => window.ORB && ORB.state(window.__FIX), 30); } } };
    window.__FIX = ${JSON.stringify(fixture)};`;
  const shim = fs.readFileSync(path.join(ROOT, 'plugin/web/desktop_shim.js'), 'utf8');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `${fakeJuce}\nwindow.MFHostInit = {};\n${shim}` });
  const load = async () => {
    await send('Page.navigate', { url: pathToFileURL(path.join(ROOT, 'plugin/web/orbit/orbit.html')).href });
    for (let i = 0; i < 50 && !(await ev('document.documentElement.dataset.ready === "1"')); i++) await sleep(100);
    // meters as while a chord plays (the puck travels its drawn loop)
    for (let k = 0; k < 40; k++) {
      const t = 4.2 + k * 0.012;
      await ev(`ORB.meter({ cpu: 0.11, voices: 6, rate: 48000, latencyMs: 7.23, puck: [${(0.05 + 0.55 * Math.cos(t)).toFixed(3)}, ${(-0.05 + 0.5 * Math.sin(t)).toFixed(3)}], w: [0.42, 0.18, 0.31, 0.09], vu: [0.12, 0.12], pk: [0.4, 0.4], midi: 3 }); document.getElementById('toast').classList.add('hidden')`);
    }
    await sleep(200);
  };
  const mouse = (type, x, y) => send('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
  const click = async (expr) => {
    const [x, y] = JSON.parse(await ev(`(() => { const el = (${expr}); if (!el) return '[-1,-1]'; const r = el.getBoundingClientRect(); return JSON.stringify([r.left + r.width / 2, r.top + r.height / 2]); })()`));
    if (x < 0) throw new Error('not found: ' + expr);
    await mouse('mousePressed', x, y); await mouse('mouseReleased', x, y); await sleep(250);
  };
  const shot = async (name, clip) => {
    const r = await send('Page.captureScreenshot', { format: 'png', clip: clip ? { ...clip, scale: 1 } : undefined, captureBeyondViewport: false });
    fs.writeFileSync(path.join(OUT, name + '.png'), Buffer.from(r.result.data, 'base64'));
    console.log('  ' + name);
  };
  const rectOf = async (expr, pad = 8) => JSON.parse(await ev(`(() => { const r = (${expr}).getBoundingClientRect(); return JSON.stringify({ x: Math.max(0, r.left - ${pad}), y: Math.max(0, r.top - ${pad}), width: r.width + 2 * ${pad}, height: r.height + 2 * ${pad} }); })()`));
  // the cards: 0-2 the layers, 3 the orbit blender, 4 the bottom row
  const card = (k, pad = 8) => rectOf(`document.querySelectorAll('.card')[${k}]`, pad);
  await load();
  await shot('orbit_panel');
  await shot('orbit_layer1', await card(0));
  await shot('orbit_layer2', await card(1));
  const c0 = await card(0, 0);
  await shot('orbit_matrix', { x: c0.x + 556, y: c0.y, width: 306, height: c0.height });   // CONVOLVE MATRIX of layer 1
  await shot('orbit_blender', await card(3));
  await shot('orbit_bottom', await card(4));
  await shot('orbit_header', { x: 0, y: 0, width: 1600, height: 84 });
  // inspectors: the layer 2 source (a Chonk print), the layer 1 impulse (a generated one)
  await click(`document.querySelectorAll('.card')[1].querySelectorAll('.lbl.clk')[0]`);
  await shot('orbit_insp_source', await rectOf(`document.querySelector('.ovl:not(.hidden)')`));
  await load();
  await click(`document.querySelectorAll('.card')[0].querySelectorAll('.lbl.clk')[1]`);
  await shot('orbit_insp_impulse', await rectOf(`document.querySelector('.ovl:not(.hidden)')`));
  // modulation list (ASSIGN), the preset browser
  await load();
  await click(`[...document.querySelectorAll('.ttl.clk')].find((e) => e.textContent.startsWith('ASSIGN'))`);
  await shot('orbit_modlist', await rectOf(`document.querySelector('.ovl:not(.hidden)')`));
  await load();
  await click(`document.querySelector('.vfd.clk')`);
  await shot('orbit_browse', await rectOf(`document.querySelector('.ovl:not(.hidden)')`));
  // assign mode: macro 1 picked, the bar asks for a control
  await load();
  await click(`document.querySelectorAll('.card')[4] && [...document.querySelectorAll('.nix')].find((n) => n.closest('.card') === document.querySelectorAll('.card')[4] && n.textContent.trim() === '1')`);
  await shot('orbit_assign_mode');
  // the CIRCLE path
  await load();
  await click(`[...document.querySelectorAll('.card')[3].querySelectorAll('*')].find((e) => e.children.length === 0 && e.textContent.trim() === 'CIRCLE')`);
  await shot('orbit_blender_circle', await card(3));
  if (errors.length) { console.log('page errors: ' + errors.slice(0, 3).join(' / ')); code = 1; }
  ws.close();
} catch (e) {
  console.log('orbit shots: ' + e.message);
  code = 1;
} finally { proc.kill(); }
process.exit(code);
