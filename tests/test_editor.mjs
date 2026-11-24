// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk browser editor tests (node, no dependencies):
//   node tests/test_editor.mjs
// 1. The JS converter core (editor/src/mf_core.js) makes .mfp files byte-identical to the Python
//    converter (tools/preset_converter/convert.py, run here into a temp folder) for the fixtures,
//    the ni-file init preset (read in place) and the developer's captures (skipped when absent).
// 2. The MF protocol (editor/src/mf_protocol.js) against a fake FM1 port: request frames match the
//    documented bytes and chunked replies reassemble.
// 3. The built page (editor/converter.html) embeds the current mapping profiles and the current
//    JS sources, and makes no network requests.
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const core = require(path.join(ROOT, 'editor/src/mf_core.js'));
const proto = require(path.join(ROOT, 'editor/src/mf_protocol.js'));

let pass = 0, fail = 0, skipped = 0;
function check(ok, what) {
  if (ok) pass++; else { fail++; console.log(`FAIL: ${what}`); }
}
const eqBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// ------------------------------------------------------------ converter parity ---
const mapsDir = path.join(ROOT, 'tools/preset_converter/maps');
const profiles = fs.readdirSync(mapsDir).filter((f) => f.endsWith('.json')).sort()
  .map((name) => ({ name, profile: JSON.parse(fs.readFileSync(path.join(mapsDir, name), 'utf8')) }));

execFileSync('python', ['-I', path.join(ROOT, 'tests/fixtures/make_fixtures.py')], { cwd: ROOT, stdio: 'ignore' });
const inputs = [];
const addDir = (d, label) => {
  const full = path.join(ROOT, d);
  if (!fs.existsSync(full)) { console.log(`skip: ${d} (absent)`); skipped++; return; }
  for (const f of fs.readdirSync(full).sort()) if (f.toLowerCase().endsWith('.nmsv')) inputs.push([path.join(full, f), `${label}/${f}`]);
};
addDir('tests/fixtures', 'fixtures');
const nifile = path.join(ROOT, 'third_party/ni-file/tests/data/Containers/NIS/files/massive/1.0.0.0/000-new.nmsv');
if (fs.existsSync(nifile)) inputs.push([nifile, 'ni-file/000-new.nmsv']); else { console.log('skip: ni-file init preset (absent)'); skipped++; }
addDir('captures', 'captures');
addDir('captures/reference', 'captures/reference');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mf-editor-test-'));
let parity = 0;
try {
  for (const [file, label] of inputs) {
    const outdir = path.join(tmp, String(parity));
    let expected;
    try {
      execFileSync('python', ['-I', path.join(ROOT, 'tools/preset_converter/convert.py'), file, '-o', outdir], { cwd: ROOT, stdio: 'pipe' });
      expected = fs.readFileSync(path.join(outdir, path.basename(file).replace(/\.nmsv$/i, '') + '.mfp'));
    } catch (e) {
      console.log(`FAIL: python converter failed on ${label}: ${e.stderr || e.message}`); fail++; continue;
    }
    let got;
    try { got = core.convertBytes(new Uint8Array(fs.readFileSync(file)), path.basename(file), profiles); } catch (e) {
      console.log(`FAIL: JS converter threw on ${label}: ${e.stack}`); fail++; continue;
    }
    const same = eqBytes(got.mfp, new Uint8Array(expected));
    if (!same) {
      const i = got.mfp.findIndex((x, k) => x !== expected[k]);
      console.log(`FAIL: ${label}: .mfp differs (JS ${got.mfp.length} B, Python ${expected.length} B, first difference at byte ${i})`);
      fail++;
    } else pass++;
    // the compatibility report and IR agree with Python too (not required byte-exact, checked anyway)
    const pyIr = JSON.parse(fs.readFileSync(path.join(outdir, path.basename(file).replace(/\.nmsv$/i, '') + '.ir.json'), 'utf8'));
    for (const k of ['supported', 'approximated', 'unsupported', 'missing_wavetables']) {
      check(pyIr.compatibility[k].length === got.ir.compatibility[k].length, `${label}: ${k} count ${got.ir.compatibility[k].length} vs Python ${pyIr.compatibility[k].length}`);
    }
    check(JSON.stringify(pyIr.modulation) === JSON.stringify(got.ir.modulation), `${label}: modulation list equals Python's`);
    check(pyIr.metadata.source_sha256 === got.ir.metadata.source_sha256, `${label}: sha256`);
    console.log(`${same ? 'ok  ' : 'BAD '} ${label}: "${got.patch.name}" ${got.mfp.length} B, profile ${got.profileName || 'none'}`);
    parity++;
  }
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
// the 2 synthetic fixtures always; one more when the ni-file init preset or captures are on this machine
const wantParity = inputs.length > 2 ? 3 : 2;
check(parity >= wantParity, `at least ${wantParity} presets compared (the 2 fixtures${wantParity > 2 ? ' and one more' : ''}; got ${parity})`);

// master FX rules (17 Nov 2026): Python massive_rules and the JS twin map Massive's FX the same way
if (fs.existsSync(path.join(ROOT, 'captures/reference/msv_default.nmsv'))) {
  const py = [
    'import sys, os, json, base64',
    "sys.path[:0] = [os.path.join(os.getcwd(), 'tools', 'preset_converter'), os.path.join(os.getcwd(), 'tools', 'preset_decoder')]",
    'import massive, convert, massive_rules',
    "d = massive.decode_bytes(open('captures/reference/msv_default.nmsv', 'rb').read())",
    "prof = convert.load_profile('tools/preset_converter/maps/massive_state_v24.json')",
    "raw = bytearray(base64.b64decode(d['state']['raw_b64']))",
    "def setc(n, v):",
    "    e = prof['enum_params'][n]; raw[e['offset']] = int({b: a for a, b in e['values'].items()}[v])",
    "for n, v in [('master_fx1_type', 'C Tube'), ('master_fx1_on_off', 'on'), ('master_fx2_type', 'Phaser'),",
    "             ('master_fx2_on_off', 'on'), ('insert1_type', 'Clip'), ('insert1_on_off', 'on')]: setc(n, v)",
    "names = {e['index']: e['massive'] for e in prof.get('float_names', [])}",
    "vals = d['state']['float_block']['values']",
    "by_name = {names[i]: vals[i] for i in names if i < len(vals)}",
    "by_name['master_fx1_dry_wet'] = 0.5; by_name['master_fx2_dry_wet'] = 0.625",
    "ir = convert.default_ir(); massive_rules.apply(ir, bytes(raw), prof, by_name)",
    "print(json.dumps({'raw': base64.b64encode(bytes(raw)).decode(), 'by_name': by_name, 'effects': ir['effects'],",
    "                  'approx': ir['compatibility']['approximated']}))",
  ].join('\n');
  const out = JSON.parse(execFileSync('python', ['-I', '-c', py], { cwd: ROOT }).toString());
  const v24 = profiles.find((p) => p.name === 'massive_state_v24.json').profile;
  const ir = core.defaultIr();
  core.rulesApply(ir, core.b64decode(out.raw), v24, out.by_name);
  check(JSON.stringify(ir.effects) === JSON.stringify(out.effects), `FX rules: JS effects ${JSON.stringify(ir.effects)} equal Python's`);
  { const js = ir.compatibility.approximated, pyA = out.approx, bad = js.map((x, i) => x === pyA[i] ? null : `JS '${x}' vs Python '${pyA[i]}'`).filter((x) => x);
    check(js.length === pyA.length && !bad.length, `FX rules: compatibility notes equal Python's (${js.length} vs ${pyA.length}) ${bad.slice(0, 2).join('; ')}`); }
  check(ir.effects.ensemble.mode === 'II' && ir.effects.ensemble.mix === 0.625 && ir.effects.tape.drive > 0,
    'Phaser -> ENSEMBLE II with its dry/wet; C Tube / Clip -> TAPE drive');
}

// delay tempo sync (17 Nov 2026): Massive's sync delay note value -> synced BBD ECHO, Python and JS the same
if (fs.existsSync(path.join(ROOT, 'captures/reference/msv_default.nmsv'))) {
  const py = [
    'import sys, os, json, base64',
    "sys.path[:0] = [os.path.join(os.getcwd(), 'tools', 'preset_converter'), os.path.join(os.getcwd(), 'tools', 'preset_decoder')]",
    'import massive, convert, massive_rules, mfp',
    "d = massive.decode_bytes(open('captures/reference/msv_default.nmsv', 'rb').read())",
    "prof = convert.load_profile('tools/preset_converter/maps/massive_state_v24.json')",
    "raw = bytearray(base64.b64decode(d['state']['raw_b64']))",
    "def setc(n, v):",
    "    e = prof['enum_params'][n]; raw[e['offset']] = int({b: a for a, b in e['values'].items()}[v])",
    "for n, v in [('master_fx1_type', 'Delay'), ('master_fx1_on_off', 'on'), ('master_fx1_sd_ln', '3'), ('master_fx1_sd_ld', '16')]: setc(n, v)",
    "by_name = {'master_fx1_dry_wet': 0.4}",
    "ir = convert.default_ir(); massive_rules.apply(ir, bytes(raw), prof, by_name)",
    "near = [mfp.nearest_delay_div(4 * n, dd) for n in range(1, 33) for dd in (1, 2, 4, 8, 12, 16, 24, 32)]",
    "print(json.dumps({'raw': base64.b64encode(bytes(raw)).decode(), 'by_name': by_name, 'effects': ir['effects'],",
    "                  'fx_sync': convert.ir_to_patch(ir)['fx']['fx_sync'] if hasattr(convert, 'ir_to_patch') else None, 'near': near}))",
  ].join('\n');
  const out = JSON.parse(execFileSync('python', ['-I', '-c', py], { cwd: ROOT }).toString());
  const v24 = profiles.find((p) => p.name === 'massive_state_v24.json').profile;
  const ir = core.defaultIr();
  core.rulesApply(ir, core.b64decode(out.raw), v24, out.by_name);
  check(JSON.stringify(ir.effects) === JSON.stringify(out.effects), `delay sync: JS effects equal Python's (${JSON.stringify(ir.effects.bbd)})`);
  check(ir.effects.bbd.sync_div === 7, `Massive delay 3/16 -> BBD ECHO 1/8 dotted (division ${ir.effects.bbd.sync_div})`);
  const near = []; for (let n = 1; n <= 32; n++) for (const dd of [1, 2, 4, 8, 12, 16, 24, 32]) near.push(core.nearestDelayDiv(4 * n, dd));
  check(JSON.stringify(near) === JSON.stringify(out.near), 'nearestDelayDiv equals Python for every Massive note value');
  if (out.fx_sync !== null) check(core.irToPatch(ir).fx.fx_sync === out.fx_sync && out.fx_sync === 8 << 4, `fx_sync byte ${out.fx_sync}`);
}

// mfp read round trip
{
  const r = core.convertBytes(new Uint8Array(fs.readFileSync(inputs[0][0])), 'x', profiles);
  check(eqBytes(core.mfpWrite(core.mfpRead(r.mfp)), r.mfp), 'mfpRead/mfpWrite round trip');
  const bad = r.mfp.slice(); bad[20] ^= 1;
  check(core.mfpCheck(bad) === 'CRC mismatch', 'CRC damage detected');
}
// Python helpers
check(core.pyRound(0.5) === 0 && core.pyRound(1.5) === 2 && core.pyRound(-0.5) === 0 && core.pyRound(2.5) === 2, 'round half even');
check(core.fmtG(0.000012345, 5) === '1.2345e-05' && core.fmtG(0.5, 5) === '0.5' && core.fmtG(123456, 5) === '1.2346e+05', 'format .5g');

// ------------------------------------------------------------ protocol ---
const hx = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join(' ');
check(hx(proto.frames.hello()) === 'f0 7d 4d 46 01 f7', 'HELLO frame = F0 7D 4D 46 01 F7');
check(hx(proto.frames.store(5)) === 'f0 7d 4d 46 04 05 f7', 'STORE 5 frame = F0 7D 4D 46 04 05 F7');
check(hx(proto.frames.load(63)) === 'f0 7d 4d 46 05 3f f7', 'LOAD 63 frame');
check(hx(proto.frames.dump(0)) === 'f0 7d 4d 46 06 00 f7', 'DUMP 0 frame');
check(hx(proto.frames.getEdit()) === 'f0 7d 4d 46 02 f7', 'GET_EDIT frame');
// documented 7-bit packing: groups of 7 bytes become 8, first byte holds bit 7 of byte k at bit k
check(hx(proto.pack7([0x80, 0x01, 0xFF, 0x7F, 0x00, 0x81, 0x02, 0x83])) === '25 00 01 7f 7f 00 01 02 01 03', 'pack7 layout');
const sample = core.convertBytes(new Uint8Array(fs.readFileSync(inputs[0][0])), 'x', profiles).mfp;
{
  const f = proto.frames.putEdit(sample);
  const n = sample.length;
  const expectLen = 6 + Math.floor(n / 7) * 8 + (n % 7 ? n % 7 + 1 : 0);
  check(f.length === expectLen && hx(f.subarray(0, 5)) === 'f0 7d 4d 46 03' && f[f.length - 1] === 0xF7, 'PUT_EDIT frame shape');
  check(eqBytes(proto.unpack7(f.subarray(5, f.length - 1)), sample), 'PUT_EDIT payload unpacks to the .mfp');
  let manual = [];   // independent packer written from the doc text
  for (let i = 0; i < n; i += 7) { const g = sample.slice(i, i + 7); manual.push(g.reduce((h, b, k) => h | ((b >> 7) << k), 0), ...g.map((b) => b & 127)); }
  check(eqBytes(f.subarray(5, f.length - 1), Uint8Array.from(manual)), 'PUT_EDIT payload equals a packer written from the doc');
  check(f.length <= 1024, 'PUT_EDIT fits the 1,024-byte incoming SysEx limit');
}
// refusals
for (const [what, fn] of [['SET_PARAM', () => proto.buildFrame(7, [0])], ['unknown cmd', () => proto.buildFrame(0x40)],
  ['byte >= 0x80', () => proto.buildFrame(1, [0x80])], ['slot 64', () => proto.frames.store(64)]]) {
  let threw = false; try { fn(); } catch (e) { threw = true; }
  check(threw, `refuses ${what}`);
}
check(!proto.isSafeFrame(Uint8Array.from([0xF0, 0x00, 0x20, 0x29, 0x01, 0xF7])), 'vendor SysEx is not a safe frame');

// fake FM1: answers like firmware/src/mfm1_app.c (chunks of 98 raw bytes, status frames)
function chunkFrames(cmd, slot, raw) {
  const count = Math.ceil(raw.length / 98), out = [];
  for (let i = 0; i < count; i++) {
    const part = raw.subarray(i * 98, i * 98 + 98);
    const args = [slot, i, count, raw.length & 127, raw.length >> 7, ...proto.pack7(part)];
    out.push(Uint8Array.from([0xF0, 0x7D, 0x4D, 0x46, cmd, ...args, 0xF7]));
  }
  return out;
}
class FakeFM1 {
  constructor() { this.sent = []; this.slots = new Map(); this.edit = sample; this.mode = 'normal'; }
  send(f) {
    this.sent.push(Uint8Array.from(f));
    const cmd = f[4], a = f.subarray(5, f.length - 1);
    const reply = (args) => setTimeout(() => this.client.receive(Uint8Array.from([0xF0, 0x7D, 0x4D, 0x46, cmd, ...args, 0xF7])), 1);
    if (cmd === 1) reply([2, 4, 480 & 127, 480 >> 7, 64, 3, ...Buffer.from('MFM1 0.2'), 0]);
    else if (cmd === 3) { const raw = proto.unpack7(a); const ok = !core.mfpCheck(raw); if (ok) this.edit = raw; reply([ok ? 0 : 2]); }
    else if (cmd === 4) { if (a[0] >= 64) reply([3]); else { this.slots.set(a[0], this.edit); reply([0]); } }
    else if (cmd === 5) reply([this.slots.has(a[0]) ? 0 : 3]);
    else if (cmd === 6 || cmd === 2) {
      const slot = cmd === 2 ? 0x7F : a[0];
      const raw = cmd === 2 ? this.edit : this.slots.get(slot);
      if (!raw) { reply([3]); return; }
      let fr = chunkFrames(cmd, slot, raw);
      if (this.mode === 'shuffled') {
        // a stale, longer reply first (replaced), chunks out of order, a status frame of another cmd in between
        const stale = chunkFrames(cmd, slot, new Uint8Array(300)).slice(0, 2);
        fr = [...stale, fr[2], fr[0], Uint8Array.from([0xF0, 0x7D, 0x4D, 0x46, 4, 0, 0xF7]), ...fr.slice(3), fr[1], fr[1]];
      }
      fr.forEach((x, i) => setTimeout(() => this.client.receive(x), 1 + i));
    }
  }
}
{
  const fake = new FakeFM1();
  const client = new proto.MFClient(fake, { check: core.mfpCheck, timeoutMs: 500 });
  fake.client = client;
  const h = await client.hello();
  check(h.protocol === 2 && h.firmware === 'MFM1 0.2' && h.presetSize === 480 && h.currentSlot === 3, 'HELLO reply parsed');
  const other = core.convertBytes(new Uint8Array(fs.readFileSync(inputs[1][0])), 'y', profiles).mfp;
  await client.putEdit(other);
  check(eqBytes(fake.edit, other), 'PUT_EDIT delivered the preset byte-exact');
  await client.store(9);
  check(hx(fake.sent[fake.sent.length - 1]) === 'f0 7d 4d 46 04 09 f7', 'STORE 9 sent as documented');
  const d = await client.dump(9);
  check(eqBytes(d, other), 'DUMP 9 reassembled byte-exact (5 chunks)');
  fake.mode = 'shuffled';
  const d2 = await client.dump(9);
  check(eqBytes(d2, other), 'DUMP reassembles out of order, with a replaced stale reply and an interleaved status');
  const ge = await client.getEdit();
  check(eqBytes(ge, other), 'GET_EDIT reassembled (slot 0x7F)');
  let err = null; try { await client.dump(40); } catch (e) { err = e; }
  check(err && /bad slot/.test(err.message), 'DUMP of an empty slot reports the status frame');
  await client.load(9);
  err = null; try { await client.putEdit(new Uint8Array(480)); } catch (e) { err = e; }
  check(err && /not sending/.test(err.message), 'an invalid .mfp is never sent');
  check(fake.sent.every((f) => proto.isSafeFrame(f)), 'every frame the client sent is a documented MF frame');
  const noPort = new proto.MFClient(null, {});
  err = null; try { await noPort.hello(); } catch (e) { err = e; }
  check(err && /no MIDI output/.test(err.message), 'no port chosen: nothing is sent');
}

// ------------------------------------------------------------ built page ---
const page = path.join(ROOT, 'editor/converter.html');
if (!fs.existsSync(page)) { console.log('FAIL: editor/converter.html missing (run python tools/build_editor.py)'); fail++; }
else {
  const html = fs.readFileSync(page, 'utf8');
  const m = html.match(/<script id="mf-profiles" type="application\/json">([\s\S]*?)<\/script>/);
  check(!!m, 'page embeds the profiles');
  if (m) {
    const emb = JSON.parse(m[1]);
    check(JSON.stringify(emb) === JSON.stringify(profiles), 'embedded profiles equal tools/preset_converter/maps (rebuild the page if not)');
  }
  for (const f of ['mf_core.js', 'mf_protocol.js', 'app.js']) {
    const src = fs.readFileSync(path.join(ROOT, 'editor/src', f), 'utf8').replace(/<\/script/gi, '<\\/script');
    check(html.includes(src), `page contains the current editor/src/${f} (rebuild the page if not)`);
  }
  check(!/\b(src|href)\s*=\s*["']?(https?:)?\/\//i.test(html) && !/@import|fetch\(|XMLHttpRequest|WebSocket/.test(html), 'no external resources or network calls in the page');
}

console.log(`\n${pass} passed, ${fail} failed, ${skipped} skipped`);
process.exit(fail ? 1 : 0);
