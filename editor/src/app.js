// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk editor page logic (DOM). Converter: MFCore (mf_core.js); FM1 link: MFProtocol (mf_protocol.js).
// Preset text is untrusted: it is only ever set with textContent, never as HTML.
'use strict';
(function () {
  const $ = (id) => document.getElementById(id);
  const PROFILES = JSON.parse($('mf-profiles').textContent);
  let current = null;      // { result, base }
  let readBytes = null;

  function el(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') e.className = v; else if (k === 'style') e.style.cssText = v; else e.setAttribute(k, v);
    }
    for (const k of kids) if (k !== null && k !== undefined) e.append(k instanceof Node ? k : String(k));
    return e;
  }
  function download(bytesOrText, name, type) {
    const blob = new Blob([bytesOrText], { type });
    const a = el('a', { href: URL.createObjectURL(blob), download: name });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }

  // ------------------------------------------------------------------ loading ---
  const drop = $('drop');
  $('file').addEventListener('change', (e) => { if (e.target.files[0]) loadFile(e.target.files[0]); });
  drop.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('file').click(); } });
  ['dragenter', 'dragover'].forEach((t) => drop.addEventListener(t, (e) => { e.preventDefault(); drop.classList.add('over'); }));
  ['dragleave', 'drop'].forEach((t) => drop.addEventListener(t, (e) => { e.preventDefault(); drop.classList.remove('over'); }));
  drop.addEventListener('drop', (e) => { const f = e.dataTransfer.files[0]; if (f) loadFile(f); });
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());

  async function loadFile(file) {
    const err = $('loaderr'); err.classList.add('hidden');
    try {
      const buf = new Uint8Array(await file.arrayBuffer());
      const result = MFCore.convertBytes(buf, file.name, PROFILES);
      current = { result, base: file.name.replace(/\.[^.]*$/, '') || 'preset' };
      show(result);
    } catch (e) {
      current = null;
      err.textContent = `Could not convert ${file.name}: ${e.message}`;
      err.classList.remove('hidden');
      $('result').classList.add('hidden'); $('settings').classList.add('hidden');
    }
    updateButtons();
  }

  // ------------------------------------------------------------------ display ---
  const pct = (x) => `${Math.round(x * 100)}%`;
  const hz = (f) => (f >= 1000 ? `${(f / 1000).toFixed(f >= 10000 ? 1 : 2)} kHz` : `${f.toFixed(f < 10 ? 2 : 0)} Hz`);
  const secs = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`);
  const envTime = (p) => 0.0005 * Math.pow(60000, p);           // MFE_TIME_RANGE
  const cutoffHz = (c) => 440 * Math.pow(2, (c * 136 - 69) / 12); // MFE cutoff = note / 136
  const lfoHz = (r) => 0.02 * Math.pow(2000, r);                  // MFE rate
  const WAVE_PATHS = [  // tiny pictures of the 8 classic (Model D-style) shapes, x 0..100, y 0..40
    'M0 40 L25 0 L75 40 L100 0', 'M0 40 Q30 0 50 0 L50 40 Q80 0 100 0', 'M0 0 L50 40 L50 0 L100 40', 'M0 40 L50 0 L50 40 L100 0 L100 40',
    'M0 40 L0 4 L50 4 L50 36 L100 36 L100 4', 'M0 36 L0 4 L30 4 L30 36 L50 36 L50 4 L80 4 L80 36 L100 36',
    'M0 36 L0 4 L12 4 L12 36 L50 36 L50 4 L62 4 L62 36 L100 36', 'M0 36 L0 4 L6 4 L6 36 L50 36 L50 4 L56 4 L56 36 L100 36'];
  function waveSvg(i) {
    const ns = 'http://www.w3.org/2000/svg';
    const s = document.createElementNS(ns, 'svg'); s.setAttribute('viewBox', '-4 -4 108 48'); s.setAttribute('class', 'wave');
    s.setAttribute('preserveAspectRatio', 'none'); s.setAttribute('aria-hidden', 'true');
    const p = document.createElementNS(ns, 'path'); p.setAttribute('d', WAVE_PATHS[i] || WAVE_PATHS[3]);
    p.setAttribute('fill', 'none'); p.setAttribute('stroke', '#F2C02E'); p.setAttribute('stroke-width', '3'); p.setAttribute('vector-effect', 'non-scaling-stroke');
    s.append(p); return s;
  }
  // Performer step shapes: names and curves as the engine plays them (dsp/mfe_tables.c perf_shape())
  const PERF_NAMES = ['RAMP UP', 'RAMP DOWN', 'EXP UP', 'EXP DOWN', 'LOG UP', 'HOLD FALL', 'S UP', 'S DOWN', 'HIGH', 'SQUARE',
    'TRIANGLE', 'BUMP', 'DECAY', 'DECAY X2', 'DECAY X3', 'DECAY X4', 'ZERO', 'FULL', 'HALF SINE', 'SINE', 'BUMP', 'STAIRS UP',
    'STAIRS DN', 'SHORT GATE', 'RAMP X2', 'RAMP DN X2', 'TRI X2', 'WOBBLE', 'BOUNCE', 'LATE RISE', 'GATE 3/4', 'SPIKE'];
  const SYNC_NUM = [1, 1, 1, 1, 3, 1, 2, 3, 1, 3, 2, 4, 8], SYNC_DEN = [8, 6, 4, 3, 8, 2, 3, 4, 1, 2, 1, 1, 1];
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
  // 16 steps: Performer = each step's shape scaled by its level; Stepper = a bar per step. Levels -1..1.
  function stepGraph(steps, performer) {
    const ns = 'http://www.w3.org/2000/svg', Wd = 160, H = 48, mid = H / 2, sw = Wd / 16;
    const s = document.createElementNS(ns, 'svg'); s.setAttribute('viewBox', `0 0 ${Wd} ${H}`); s.setAttribute('class', 'steps-graph');
    s.setAttribute('preserveAspectRatio', 'none'); s.setAttribute('role', 'img');
    s.setAttribute('aria-label', performer ? 'Performer steps' : 'Stepper steps');
    const add = (tag, attrs) => { const e = document.createElementNS(ns, tag); for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); s.append(e); return e; };
    for (let k = 1; k < 16; k++) add('line', { x1: k * sw, x2: k * sw, y1: 0, y2: H, stroke: '#704654', 'stroke-width': k % 4 ? 0.3 : 0.8 });
    add('line', { x1: 0, x2: Wd, y1: mid, y2: mid, stroke: '#704654', 'stroke-width': 0.6 });
    const y = (v) => (mid - Math.max(-1, Math.min(1, v)) * (mid - 2)).toFixed(2);
    if (performer) {
      let d = '';
      steps.forEach((st, k) => {
        for (let j = 0; j <= 12; j++) {
          const x = j / 12, v = perfShape(st.shape, Math.min(x, 0.9999)) * st.level;
          d += `${j === 0 ? (k === 0 ? 'M' : 'L') : 'L'}${(k * sw + x * sw).toFixed(2)} ${y(v)} `;
        }
      });
      add('path', { d, fill: 'none', stroke: '#A4C440', 'stroke-width': 1.4, 'vector-effect': 'non-scaling-stroke' });
    } else {
      steps.forEach((st, k) => {
        const v = Math.max(-1, Math.min(1, st.level)), top = v >= 0 ? y(v) : mid, h = Math.abs(v) * (mid - 2);
        add('rect', { x: (k * sw + 1).toFixed(2), width: (sw - 2).toFixed(2), y: top, height: Math.max(0.5, h).toFixed(2), fill: k % 4 ? '#A4C440' : '#F2C02E' });
      });
    }
    return s;
  }
  function kv(pairs) {
    const d = el('dl', { class: 'kv' });
    for (const [k, v] of pairs) d.append(el('dt', null, k), el('dd', null, v));
    return d;
  }
  function card(cls, title, chip, ...body) {
    return el('div', { class: `card ${cls}` }, el('h3', null, el('span', null, title), chip ? el('span', { class: 'chip' }, chip) : null), ...body);
  }

  function show(r) {
    const ir = r.ir, c = ir.compatibility, md = ir.metadata;
    $('pname').textContent = r.patch.name || md.name;
    $('pmeta').textContent = `${current.base}.nmsv  |  ${md.source_app || 'unknown app'} ${md.source_app_version || ''}  |  mapping profile: ${r.profileName || 'none (values kept, not applied)'}  |  .mfp ${r.mfp.length} bytes`;
    $('n-sup').textContent = c.supported.length; $('n-app').textContent = c.approximated.length;
    $('n-uns').textContent = c.unsupported.length; $('n-wt').textContent = c.missing_wavetables.length;
    const total = c.supported.length + c.approximated.length + c.unsupported.length || 1;
    const bar = $('cbar'); bar.replaceChildren();
    for (const [n, col] of [[c.supported.length, 'var(--avocado)'], [c.approximated.length, 'var(--mustard)'], [c.unsupported.length, 'var(--rust)']])
      bar.append(el('span', { style: `width:${(100 * n / total).toFixed(2)}%;background:${col}` }));
    const lists = $('lists'); lists.replaceChildren();
    for (const [title, key] of [['Supported', 'supported'], ['Approximated', 'approximated'], ['Unsupported (kept in the IR for later firmware)', 'unsupported'],
      ['Massive wavetables replaced by classic shapes', 'missing_wavetables'], ['Fidelity notes', 'notes']]) {
      const items = c[key];
      lists.append(el('details', null, el('summary', null, `${title} (${items.length})`),
        el('ul', null, ...(items.length ? items.map((x) => el('li', null, x)) : [el('li', null, '(none)')]))));
    }
    if (ir.preserved.decoder_warnings && ir.preserved.decoder_warnings.length)
      lists.append(el('p', { class: 'hint' }, 'Decoder warnings: ' + ir.preserved.decoder_warnings.join('; ')));
    renderSettings(ir);
    $('result').classList.remove('hidden'); $('settings').classList.remove('hidden');
  }

  function renderSettings(ir) {
    const cards = $('cards'); cards.replaceChildren();
    ir.oscillators.forEach((o, i) => {
      const moog = o.wavetable === 'MOOG';
      const f = moog ? Math.max(0, Math.min(7, Math.round(o.position * 7))) : -1;
      const name = moog ? MFCore.MOOG_WAVES[f] : `${o.wavetable} table, position ${pct(o.position)}`;
      const body = [moog ? waveSvg(f) : null, kv([
        ['waveform', name], ['from Massive', o.massive_wavetable || '-'], ['pitch', `${o.semis > 0 ? '+' : ''}${o.semis} st`],
        ['amp', pct(o.amp)], ['to F1 / F2', `${pct(1 - o.route)} / ${pct(o.route)}`], ['drift', pct(o.drift || 0)]])];
      const c = card('c-osc', `Osc ${i + 1}`, o.enabled ? 'on' : 'off', ...body);
      if (!o.enabled) c.classList.add('off');
      cards.append(c);
    });
    const mo = ir.mod_osc || {}, nz = ir.noise;
    cards.append(card('c-osc', 'Mod osc + noise', null, kv([
      ['mod osc', mo.enabled === false ? 'off' : `${mo.semis > 0 ? '+' : ''}${mo.semis || 0} st`],
      ['ring -> osc', mo.rm_osc || '-'], ['phase -> osc', mo.pm_osc || '-'], ['position -> osc', mo.pos_osc || '-'],
      ['noise amp', pct(nz.amp)], ['noise colour', pct(nz.color)]])));
    ir.filters.forEach((f, i) => {
      const c = card('c-filt', `Filter ${i + 1}`, f.type, kv([
        ['cutoff', f.type === 'OFF' ? '-' : hz(cutoffHz(f.cutoff))], ['resonance', pct(f.resonance)],
        ['drive', pct(f.drive)], ['key track', pct(f.keytrack)]]));
      if (f.type === 'OFF') c.classList.add('off');
      cards.append(c);
    });
    cards.append(card('c-filt', 'Routing + voice', null, kv([
      ['serial / parallel', `${pct(1 - ir.routing.serpar)} / ${pct(ir.routing.serpar)}`], ['F1 / F2 mix', `${pct(ir.routing.f1_mix)} / ${pct(ir.routing.f2_mix)}`],
      ['voice', `${ir.voice.mode}, ${ir.voice.max_voices} voices`], ['bend', `${ir.voice.bend_range} st`], ['glide', pct(ir.voice.glide)],
      ['master', `x${(ir.voice.master * 4).toFixed(2)}`]])));
    ir.envelopes.forEach((e, i) => {
      cards.append(card('c-env', `Env ${i + 1}`, i === 3 ? 'amp' : null, kv([
        ['delay', e.delay ? secs(envTime(e.delay)) : '0'], ['attack', secs(envTime(e.attack))], ['decay', secs(envTime(e.decay))],
        ['sustain', pct(e.sustain)], ['release', secs(envTime(e.release))], ['loop', e.loop ? 'yes' : 'no']])));
    });
    const slots = ir._slots || [];
    ir.lfos.forEach((l, i) => {
      const s = slots[i];
      const kind = s ? s.kind.toUpperCase() : (l.wave === 'PERFORMER' || l.wave === 'STEPPER' ? l.wave : 'LFO');
      const sd = Math.max(0, Math.min(12, l.sync_div));
      const rows = [['decoded as', s ? `Massive ${kind}` : 'default (not decoded)'],
        ['rate', l.sync ? `sync ${SYNC_NUM[sd]}/${SYNC_DEN[sd]} beat${kind !== 'LFO' ? ' a step' : ''}` : hz(lfoHz(l.rate)) + (kind !== 'LFO' ? ' a step' : '')],
        ['restart on key', l.keysync ? 'yes' : 'no']];
      if (kind === 'LFO') rows.push(['wave', l.wave], ['phase', pct(l.phase)], ['unipolar', l.unipolar ? 'yes' : 'no']);
      if (kind === 'STEPPER') rows.push(['glide', pct(l.glide)]);
      if (s) rows.push(['output (amp^2)', pct(s.output_gain)]);
      const body = [];
      if (kind !== 'LFO' && l.steps) {
        body.push(stepGraph(l.steps, kind === 'PERFORMER'));
        if (kind === 'PERFORMER') {
          const runs = [];   // consecutive steps with the same shape: "1-16 RAMP DOWN"
          l.steps.forEach((st, k) => {
            const name = st.level === 0 ? 'off' : (PERF_NAMES[st.shape] || String(st.shape));
            const r = runs[runs.length - 1];
            if (r && r.name === name) r.to = k + 1; else runs.push({ from: k + 1, to: k + 1, name });
          });
          body.push(el('p', { class: 'steps' }, runs.map((r) => `${r.from === r.to ? r.from : `${r.from}-${r.to}`} ${r.name}`).join(' . ')));
        }
      }
      body.push(kv(rows));
      if (s && s.notes.length) body.push(el('p', { class: 'hint' }, s.notes.slice(0, 3).join('; ') + (s.notes.length > 3 ? ` (+${s.notes.length - 3} more)` : '')));
      cards.append(card('c-lfo', `Mod ${i + 5}`, kind, ...body));
    });
    const mods = ir.modulation;
    cards.append(card('c-mod', 'Modulation', `${Math.min(mods.length, 16)} / 16`,
      mods.length ? el('ul', { class: 'modlist' }, ...mods.slice(0, 16).map((m) =>
        el('li', null, el('span', null, `${m.source} -> ${m.destination}`), el('span', { class: 'amt' }, `${m.amount > 0 ? '+' : ''}${Math.round(m.amount * 100)}%`))))
        : el('p', { class: 'hint' }, 'No modulation routings.')));
    const fx = ir.effects;
    cards.append(card('c-mac', 'Effects', null, kv([['oct fuzz', pct(fx.fuzz.amount)], ['tape drive', pct(fx.tape.drive)],
      ['ensemble', `${fx.ensemble.mode} ${pct(fx.ensemble.mix)}`], ['bbd echo mix', pct(fx.bbd.mix)], ['oil can mix', pct(fx.oilcan.mix)],
      [`reverb (${fx.reverb.type.toLowerCase()})`, pct(fx.reverb.amount)]])));
    const mac = el('div', { class: 'macros' }, ...ir.macros.map((m, i) => el('div', { class: 'mac' }, `${m.name || i + 1}`,
      el('div', { class: 'meter' }, el('i', { style: `width:${(Math.max(0, Math.min(1, m.value)) * 100).toFixed(1)}%` })))));
    const mc = card('c-mac', 'Macros', '8', mac); mc.style.gridColumn = '1 / -1';
    cards.append(mc);
  }

  $('dl-mfp').addEventListener('click', () => current && download(current.result.mfp, `${current.base}.mfp`, 'application/octet-stream'));
  $('dl-ir').addEventListener('click', () => current && download(JSON.stringify(current.result.ir, null, 1), `${current.base}.ir.json`, 'application/json'));
  $('dl-rep').addEventListener('click', () => current && download(current.result.report, `${current.base}.report.md`, 'text/markdown'));
  $('dl-rb').addEventListener('click', () => readBytes && download(readBytes.bytes, readBytes.name, 'application/octet-stream'));

  // --------------------------------------------------------------------- MIDI ---
  let access = null, out = null, inp = null, client = null;
  const logBox = $('log');
  function log(cls, text) {
    logBox.append(el('div', { class: cls }, text));
    while (logBox.childNodes.length > 200) logBox.firstChild.remove();
    logBox.scrollTop = logBox.scrollHeight;
  }
  const hexs = (b) => Array.from(b.length > 24 ? b.subarray(0, 24) : b, (x) => x.toString(16).padStart(2, '0')).join(' ') + (b.length > 24 ? ` ... (${b.length} bytes)` : '');
  function status(ok, text) { const s = $('midi-status'); s.textContent = text; s.className = `status ${ok ? 'ok' : 'bad'}`; }

  function updateButtons() {
    const ready = !!(out && inp && client);
    $('b-hello').disabled = !ready; $('b-read').disabled = !ready; $('b-edit').disabled = !ready; $('b-load').disabled = !ready;
    $('b-send').disabled = !(ready && current); $('b-store').disabled = !(ready && current);
  }
  function fillPorts() {
    for (const [sel, list, chosen] of [[$('midi-out'), access.outputs, out], [$('midi-in'), access.inputs, inp]]) {
      sel.replaceChildren(el('option', { value: '' }, 'choose a port'));
      list.forEach((p) => { const o = el('option', { value: p.id }, p.name || p.id); if (chosen && chosen.id === p.id) o.selected = true; sel.append(o); });
      sel.disabled = false;
    }
  }
  function bind() {
    const oid = $('midi-out').value, iid = $('midi-in').value;
    if (inp) inp.onmidimessage = null;
    out = oid ? access.outputs.get(oid) : null;
    inp = iid ? access.inputs.get(iid) : null;
    client = out && inp ? new MFProtocol.MFClient({ send: (f) => out.send(f) }, {
      check: MFCore.mfpCheck, log: (dir, b) => log(dir, `${dir === 'out' ? '>>' : '<<'} ${hexs(b)}`) }) : null;
    if (inp && client) inp.onmidimessage = (ev) => client.receive(ev.data);
    updateButtons();
  }
  $('midi-out').addEventListener('change', bind);
  $('midi-in').addEventListener('change', bind);
  $('midi-on').addEventListener('click', async () => {
    if (!navigator.requestMIDIAccess) { $('midi-state').textContent = 'This browser has no Web MIDI. Use Chrome or Edge.'; return; }
    try {
      access = await navigator.requestMIDIAccess({ sysex: true });
      $('midi-state').textContent = 'MIDI ready (SysEx allowed). Choose the FM1 ports.';
      fillPorts();
      for (const [sel, list] of [[$('midi-out'), access.outputs], [$('midi-in'), access.inputs]]) {
        if (!sel.value) list.forEach((p) => { if (!sel.value && /fm-?1|m-?vave/i.test(p.name || '')) sel.value = p.id; });
      }
      bind();
      access.onstatechange = () => { fillPorts(); bind(); };
    } catch (e) {
      $('midi-state').textContent = `MIDI not available: ${e.message || e}. SysEx permission is needed.`;
    }
  });
  function slot() {
    const n = parseInt($('slot').value, 10);
    if (!(n >= 1 && n <= 64)) throw new Error('slot must be 1 to 64');
    return n - 1;
  }
  async function act(label, fn) {
    try { log('note', `-- ${label}`); const msg = await fn(); status(true, msg); log('note', msg); } catch (e) { status(false, `${label}: ${e.message}`); log('err', e.message); }
  }
  $('b-hello').addEventListener('click', () => act('hello', async () => {
    const h = await client.hello();
    return `FM1 answered: firmware "${h.firmware}", protocol ${h.protocol}, preset v${h.presetVersion} (${h.presetSize} B), ${h.slots} slots, current slot ${h.currentSlot + 1}`
      + (h.protocol !== 2 ? '  (this page speaks protocol 2)' : '') + (h.presetSize !== MFCore.MFP.SIZE ? `  (this page writes ${MFCore.MFP.SIZE}-byte presets)` : '');
  }));
  $('b-send').addEventListener('click', () => act('send', async () => {
    await client.putEdit(current.result.mfp);
    return `"${current.result.patch.name}" is playing on the FM1 (edit buffer).`;
  }));
  $('b-store').addEventListener('click', () => act('store', async () => {
    const s = slot();
    if (!window.confirm(`Overwrite FM1 slot ${s + 1} with "${current.result.patch.name}"?`)) return 'Store cancelled.';
    await client.putEdit(current.result.mfp);
    await client.store(s);
    return `"${current.result.patch.name}" stored in slot ${s + 1}.`;
  }));
  $('b-load').addEventListener('click', () => act('load', async () => { const s = slot(); await client.load(s); return `Slot ${s + 1} is playing.`; }));
  async function showRead(bytes, what) {
    const p = MFCore.mfpRead(bytes);
    readBytes = { bytes, name: `${(p.name || 'fm1').replace(/[^A-Za-z0-9 _-]/g, '_')}.mfp` };
    $('rb-text').textContent = `${what}: "${p.name}", filter 1 ${MFCore.MFP.FILTERS[p.filt[0].type] || p.filt[0].type}, ${p.mod.filter((m) => m.src).length} modulation routings.`;
    $('readback').classList.remove('hidden');
    return `${what} read: "${p.name}" (${bytes.length} bytes, CRC ok).`;
  }
  $('b-read').addEventListener('click', () => act('read slot', async () => { const s = slot(); return showRead(await client.dump(s), `Slot ${s + 1}`); }));
  $('b-edit').addEventListener('click', () => act('read edit buffer', async () => showRead(await client.getEdit(), 'Edit buffer')));
  updateButtons();
})();
