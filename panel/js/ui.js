// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk Android UI: the panel (DOM). Layout and workflow follow the classic wavetable soft-synth
// (oscillators, mod osc / noise / feedback, two filters with serial-parallel and mix, master; envelopes
// 1-4 and modulators 5-8 as tabs with their editor, 8 macros, effects; drag a source onto a knob to
// modulate it) in the dress of a 1970s analog monosynth.
// Parameters: MFPanel (panel_model.js); presets: MFCore (mf_core.js, .mfp v4 writer/reader and the .nmsv
// converter); native side: MFBridge (bridge.js). Preset and file names are untrusted text: only ever
// set with textContent.
'use strict';
(function () {
  const Core = window.MFCore, M = window.MFPanel, Bridge = window.MFBridge;
  const PROFILES = window.MF_PROFILES || [];
  const NS = 'http://www.w3.org/2000/svg';
  const $ = (id) => document.getElementById(id);
  const stage = $('stage');
  const clamp = M.clamp;
  const NA_TEXT = 'Not in this engine version yet';

  // ------------------------------------------------------------------ helpers ---
  function el(tag, cls, parent, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    if (parent) parent.appendChild(e);
    return e;
  }
  function sv(tag, attrs, parent) {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }
  function place(e, x, y, w, h) {
    if (x !== undefined) e.style.left = `${x}px`;
    if (y !== undefined) e.style.top = `${y}px`;
    if (w !== undefined) e.style.width = `${w}px`;
    if (h !== undefined) e.style.height = `${h}px`;
    return e;
  }
  let SCALE = 1, OX = 0, OY = 0;
  const toStage = (cx, cy) => [(cx - OX) / SCALE, (cy - OY) / SCALE];
  function stageRect(e) {
    const r = e.getBoundingClientRect();
    const [x, y] = toStage(r.left, r.top);
    return { x, y, w: r.width / SCALE, h: r.height / SCALE };
  }
  // ---- view: fit to the screen, plus zoom (17 Nov 2026): pinch, two-finger pan, double-tap
  let BASE = 1, BX = 0, BY = 0, ZOOM = 1, PANX = 0, PANY = 0;
  const ZMAX = 3.5;
  let zoomChip = null;
  function applyView() {
    const w = window.innerWidth, h = window.innerHeight;
    ZOOM = clamp(ZOOM, 1, ZMAX);
    SCALE = BASE * ZOOM;
    const sw = 1920 * SCALE, sh = 1200 * SCALE;
    if (ZOOM === 1) { PANX = 0; PANY = 0; }
    // keep the zoomed stage covering the screen
    OX = ZOOM === 1 ? BX : clamp(BX + PANX, Math.min(BX, w - sw), Math.max(BX, 0));
    OY = ZOOM === 1 ? BY : clamp(BY + PANY, Math.min(BY, h - sh), Math.max(BY, 0));
    PANX = OX - BX; PANY = OY - BY;
    stage.style.transform = `translate(${OX}px, ${OY}px) scale(${SCALE})`;
    gestureLayer();
    if (zoomChip) zoomChip.classList.toggle('hidden', ZOOM < 1.02);
  }
  let layerTimer = 0;
  function gestureLayer() {   // will-change only while the view moves; dropped 250 ms later so text re-rasters sharp
    stage.style.willChange = 'transform';
    clearTimeout(layerTimer);
    layerTimer = setTimeout(() => { stage.style.willChange = ''; }, 250);
  }
  function fit() {
    const w = window.innerWidth, h = window.innerHeight;
    BASE = Math.min(w / 1920, h / 1200);
    BX = Math.round((w - 1920 * BASE) / 2); BY = Math.round((h - 1200 * BASE) / 2);
    applyView();
  }
  // zoom to z keeping stage point (sx, sy) under screen point (cx, cy)
  function zoomKeep(z, sx, sy, cx, cy) {
    ZOOM = clamp(z, 1, ZMAX);
    const ns = BASE * ZOOM;
    PANX = cx - sx * ns - BX; PANY = cy - sy * ns - BY;
    applyView();
  }
  let zoomAnim = 0;
  function animateZoom(z, cx, cy) {
    cancelAnimationFrame(zoomAnim);
    const z0 = ZOOM, [sx, sy] = toStage(cx, cy), t0 = performance.now();
    const step = (t) => {
      const k = Math.min(1, (t - t0) / 220), e = 1 - Math.pow(1 - k, 3);
      zoomKeep(z0 + (z - z0) * e, sx, sy, cx, cy);
      if (k < 1) zoomAnim = requestAnimationFrame(step);
    };
    zoomAnim = requestAnimationFrame(step);
  }
  zoomChip = document.createElement('div');
  zoomChip.id = 'zoom-chip'; zoomChip.className = 'hidden'; zoomChip.textContent = 'FIT';
  document.body.appendChild(zoomChip);
  zoomChip.addEventListener('pointerdown', (e) => { e.preventDefault(); e.stopPropagation(); animateZoom(1, window.innerWidth / 2, window.innerHeight / 2); });

  // gestures; the keyboard and wheels never zoom, so chords and bends stay musical
  const NOZOOM = '#keys, #wheelbox, #zoom-chip, #dialog, #browser, #picker';
  const EMPTY_SKIP = '.ctl, .btn, .rocker, .mslot, .tab, svg, input, select, #keys, #wheelbox, #strip, #title-hold';
  const touches = new Map();      // pointerId -> {x, y, target}
  let pinch = null;               // {d0, z0, sx, sy}: stage point under the starting midpoint
  let pan1 = null;                // one finger on an empty part of the panel while zoomed: {id, x, y}
  let lastEmptyTap = { t: 0, x: 0, y: 0 };
  const dist = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
  let ownCancel = false;
  function cancelControl(id, t) {   // ends the control the first finger was on; the finger itself stays in the pinch
    ownCancel = true;
    try { t.dispatchEvent(new PointerEvent('pointercancel', { pointerId: id, bubbles: true })); } catch (x) { /* */ }
    ownCancel = false;
  }
  window.addEventListener('pointerdown', (e) => {
    if (e.pointerType === 'mouse') return;
    const t = e.target;
    if (t.closest && t.closest(NOZOOM)) return;
    touches.set(e.pointerId, { x: e.clientX, y: e.clientY, target: t });
    if (touches.size === 2) {
      const [a, b] = [...touches.values()];
      for (const [id, v] of touches) if (id !== e.pointerId) cancelControl(id, v.target);
      const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2, [sx, sy] = toStage(mx, my);
      pinch = { d0: Math.max(20, dist(a, b)), z0: ZOOM, sx, sy };
      pan1 = null; cancelAnimationFrame(zoomAnim); hideTip();
      e.stopImmediatePropagation(); e.preventDefault(); return;
    }
    if (pinch) { e.stopImmediatePropagation(); e.preventDefault(); return; }
    const empty = !(t.closest && t.closest(EMPTY_SKIP));
    if (empty && touches.size === 1) {
      const now = Date.now();
      if (now - lastEmptyTap.t < 330 && Math.hypot(e.clientX - lastEmptyTap.x, e.clientY - lastEmptyTap.y) < 40) {
        lastEmptyTap.t = 0;
        animateZoom(ZOOM > 1.2 ? 1 : 2.2, e.clientX, e.clientY);
        e.stopImmediatePropagation(); e.preventDefault(); return;
      }
      lastEmptyTap = { t: now, x: e.clientX, y: e.clientY };
      if (ZOOM > 1.02) pan1 = { id: e.pointerId, x: e.clientX, y: e.clientY };
    }
  }, true);
  window.addEventListener('pointermove', (e) => {
    const p = touches.get(e.pointerId); if (!p) return;
    p.x = e.clientX; p.y = e.clientY;
    if (pinch && touches.size >= 2) {
      const [a, b] = [...touches.values()];
      zoomKeep(pinch.z0 * dist(a, b) / pinch.d0, pinch.sx, pinch.sy, (a.x + b.x) / 2, (a.y + b.y) / 2);
      e.stopImmediatePropagation(); return;
    }
    if (pinch) { e.stopImmediatePropagation(); return; }
    if (pan1 && pan1.id === e.pointerId) {
      PANX += e.clientX - pan1.x; PANY += e.clientY - pan1.y; pan1.x = e.clientX; pan1.y = e.clientY; applyView();
    }
  }, true);
  const lift = (e) => {
    if (ownCancel || !touches.has(e.pointerId)) return;
    touches.delete(e.pointerId);
    if (pan1 && pan1.id === e.pointerId) pan1 = null;
    if (pinch) {
      e.stopImmediatePropagation();
      if (touches.size === 0) {
        pinch = null;
        if (ZOOM < 1.08) animateZoom(1, window.innerWidth / 2, window.innerHeight / 2);   // snap back to fit
      }
    }
  };
  window.addEventListener('pointerup', lift, true);
  window.addEventListener('pointercancel', lift, true);
  window.addEventListener('resize', fit);
  for (const t of ['gesturestart', 'contextmenu', 'selectstart', 'dragstart']) document.addEventListener(t, (e) => e.preventDefault());
  document.addEventListener('touchmove', (e) => { if (!e.target.closest || !e.target.closest('.scroll')) e.preventDefault(); }, { passive: false });
  document.addEventListener('dblclick', (e) => e.preventDefault());

  let toastTimer = 0;
  function toast(text) {
    let t = document.querySelector('.toast');
    if (!t) t = el('div', 'toast', stage);
    t.textContent = text; t.style.opacity = '1';
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.style.opacity = '0'; }, 2200);
  }

  // ---------------------------------------------------------------- textures ---
  function rng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; }; }
  function noiseTexture(size, base, amount, seed) {
    const c = document.createElement('canvas'); c.width = c.height = size;
    const g = c.getContext('2d'), img = g.createImageData(size, size), rnd = rng(seed);
    for (let i = 0; i < size * size; i++) {
      const n = (rnd() - 0.5) * amount;
      img.data[i * 4] = clamp(base[0] + n, 0, 255); img.data[i * 4 + 1] = clamp(base[1] + n, 0, 255);
      img.data[i * 4 + 2] = clamp(base[2] + n, 0, 255); img.data[i * 4 + 3] = 255;
    }
    g.putImageData(img, 0, 0);
    return c.toDataURL();
  }
  function drawWood(canvas, seed) {
    const W = 128, H = 2400; canvas.width = W; canvas.height = H;
    const g = canvas.getContext('2d'), rnd = rng(seed);
    const bg = g.createLinearGradient(0, 0, W, 0);
    bg.addColorStop(0, '#2a160b'); bg.addColorStop(0.3, '#4a2a16'); bg.addColorStop(0.55, '#5b3520'); bg.addColorStop(0.8, '#43261a'); bg.addColorStop(1, '#25130a');
    g.fillStyle = bg; g.fillRect(0, 0, W, H);
    for (let k = 0; k < 70; k++) {
      const x0 = rnd() * W, amp = 2 + rnd() * 9, f = 0.0015 + rnd() * 0.004, ph = rnd() * 6.28, dark = rnd() < 0.62;
      g.strokeStyle = dark ? `rgba(20,8,3,${0.18 + rnd() * 0.35})` : `rgba(160,100,60,${0.07 + rnd() * 0.14})`;
      g.lineWidth = 0.6 + rnd() * (dark ? 2.6 : 1.6);
      g.beginPath();
      for (let y = 0; y <= H; y += 12) {
        const x = x0 + Math.sin(y * f + ph) * amp + Math.sin(y * f * 3.7 + ph * 2) * amp * 0.25;
        if (y === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
    }
    for (let k = 0; k < 5000; k++) { g.fillStyle = `rgba(15,6,2,${0.15 + rnd() * 0.3})`; g.fillRect(rnd() * W, rnd() * H, 0.8, 2 + rnd() * 5); }
    for (let k = 0; k < 2; k++) {
      const cx = 30 + rnd() * 68, cy = 200 + rnd() * 2000;
      for (let r = 3; r < 26; r += 2.5) { g.strokeStyle = `rgba(25,10,4,${0.35 - r * 0.01})`; g.lineWidth = 1.2; g.beginPath(); g.ellipse(cx, cy, r * 0.55, r * 1.8, 0, 0, 6.283); g.stroke(); }
    }
    const sh = g.createLinearGradient(0, 0, W, 0);
    sh.addColorStop(0, 'rgba(255,220,180,0)'); sh.addColorStop(0.42, 'rgba(255,220,180,0.10)'); sh.addColorStop(0.5, 'rgba(255,220,180,0.03)'); sh.addColorStop(1, 'rgba(0,0,0,0.2)');
    g.fillStyle = sh; g.fillRect(0, 0, W, H);
  }

  // ------------------------------------------------------------------- state ---
  let patch = Core.defaultPatch();
  const FACTORY = window.MFFactory.build(Core, M);
  let entries = [];          // [{kind:'F'|'U', name, patch?|b64?}]
  let current = 0;           // index into entries, -1 = an unsaved import / init
  let dirty = false;
  const controls = [];       // { update(force) }
  const views = [];          // functions redrawn after an edit

  function refresh(force) { for (const c of controls) c.update(force); for (const v of views) v(force); updateDisplay(); }
  function edit(id, v) { M.set(patch, id, v); touched(); }
  function touched() { dirty = true; scheduleSend(); refresh(false); }

  // patch -> engine, throttled to <= 60 per second
  let lastSend = 0, sendTimer = 0, lastErr = 0;
  function sendNow() {
    clearTimeout(sendTimer); sendTimer = 0; lastSend = Date.now();
    let res;
    try { res = Bridge.loadPreset(Core.b64encode(Core.mfpWrite(patch))); } catch (e) { res = String(e.message || e); }
    if (res !== undefined && res !== 'ok' && Date.now() - lastErr > 3000) { lastErr = Date.now(); toast(`Engine: ${res}`); }
  }
  function scheduleSend() {
    if (sendTimer) return;
    const dt = Date.now() - lastSend;
    if (dt >= 17) sendNow(); else sendTimer = setTimeout(sendNow, 17 - dt);
  }

  // ------------------------------------------------------------------ tooltip ---
  const tip = $('tip'), tipL = tip.querySelector('.tl'), tipV = tip.querySelector('.tv');
  function showTip(ctlEl, label, value) {
    const r = stageRect(ctlEl);
    tipL.textContent = label; tipV.textContent = value;
    tip.classList.remove('hidden');
    place(tip, clamp(r.x + r.w / 2, 110, 1810), Math.max(76, r.y + 6));
  }
  const hideTip = () => tip.classList.add('hidden');

  // ------------------------------------------------------------------- knobs ---
  const WAVE_PATHS = ['M0 40 L25 0 L75 40 L100 0', 'M0 40 Q30 0 50 0 L50 40 Q80 0 100 0', 'M0 0 L50 40 L50 0 L100 40', 'M0 40 L50 0 L50 40 L100 0 L100 40',
    'M0 40 L0 4 L50 4 L50 36 L100 36 L100 4', 'M0 36 L0 4 L30 4 L30 36 L50 36 L50 4 L80 4 L80 36 L100 36',
    'M0 36 L0 4 L12 4 L12 36 L50 36 L50 4 L62 4 L62 36 L100 36', 'M0 36 L0 4 L6 4 L6 36 L50 36 L50 4 L56 4 L56 36 L100 36'];
  const ROUTE_COLORS = ['#ffb02e', '#58a6ff', '#9ccc4a'];
  function detentAngles(n) {
    const step = n <= 6 ? 50 : 270 / (n - 1), span = step * (n - 1);
    return Array.from({ length: n }, (_, i) => -span / 2 + i * step);
  }
  function angleOf(d, v) {
    if (d.kind === 'detent') return detentAngles(d.max - d.min + 1)[Math.round(v) - d.min] || 0;
    return -150 + 300 * (clamp(v, d.min, d.max) - d.min) / (d.max - d.min);
  }
  const polar = (a, r) => [Math.sin(a * Math.PI / 180) * r, -Math.cos(a * Math.PI / 180) * r];
  function arcPath(r, a0, a1) {
    const [x0, y0] = polar(a0, r), [x1, y1] = polar(a1, r);
    return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${Math.abs(a1 - a0) > 180 ? 1 : 0} ${a1 > a0 ? 1 : 0} ${x1.toFixed(2)} ${y1.toFixed(2)}`;
  }
  function text(g, x, y, str, size, cls) {
    const t = sv('text', { x: x.toFixed(1), y: y.toFixed(1), 'text-anchor': 'middle', class: cls || 'scale', 'font-size': size }, g);
    t.textContent = str; return t;
  }

  function drawScale(g, d, R, size, slots) {
    const tiny = size <= 46, small = size < 58;
    if (d.kind === 'detent') {
      detentAngles(d.max - d.min + 1).forEach((a, i) => {
        const v = d.min + i;
        const [x1, y1] = polar(a, R + 3), [x2, y2] = polar(a, R + 8);
        sv('line', { x1, y1, x2, y2, class: 'tick' }, g);
        if (d.id.endsWith('.wave')) {
          const [cx, cy] = polar(a, R + 25);
          const ic = sv('g', { transform: `translate(${(cx - 9).toFixed(1)} ${(cy - 5).toFixed(1)}) scale(0.18 0.25)` }, g);
          sv('path', { d: WAVE_PATHS[i], class: 'wave-icon', 'vector-effect': 'non-scaling-stroke' }, ic);
          sv('rect', { x: (cx - 14).toFixed(1), y: (cy - 10).toFixed(1), width: 28, height: 20, fill: 'transparent', class: 'pick-hit', 'data-v': v }, g);
        } else {
          const [cx, cy] = polar(a, R + 18);
          text(g, cx, cy + 4.5, d.labels[i], 12.5, 'scale pick').setAttribute('data-v', v);
        }
      });
      return;
    }
    const n = d.scale === 'freq' ? 15 : 11;
    for (let i = 0; i < n; i++) {
      const a = -150 + 300 * i / (n - 1);
      const [x1, y1] = polar(a, R + 3), [x2, y2] = polar(a, R + (i === 0 || i === n - 1 || i === (n - 1) / 2 ? 9 : 7));
      sv('line', { x1, y1, x2, y2, class: 'tick' }, g);
    }
    // end / centre legends
    const ends = { route: ['F1', 'F2'], serpar: ['SER', 'PAR'], freq: ['-7', '+7'], pm: ['-', '+'], semis: ['-48', '+48'], color: ['W', 'D'] }[d.scale];
    if (tiny && !ends) return;
    if (slots) {   // the modulation slots sit under the knob: legend only at the top
      const [cx, cy] = polar(0, R + 15);
      if (!tiny) text(g, cx, cy + 3, d.scale === 'freq' || d.scale === 'pm' ? '0' : '5', 10.5);
      return;
    }
    if (ends) {
      const [ax, ay] = polar(-150, R + 15), [bx, by] = polar(150, R + 15);
      text(g, ax - 2, ay + 7, ends[0], 10.5); text(g, bx + 2, by + 7, ends[1], 10.5);
      if (!tiny && (d.scale === 'freq' || d.scale === 'pm')) { const [cx, cy] = polar(0, R + 15); text(g, cx, cy + 3, '0', 10.5); }
      return;
    }
    for (let i = 0; i <= 10; i += small ? 5 : 1) {
      if (!small && i % 2 === 1) continue;
      const [cx, cy] = polar(-150 + 30 * i, R + 15);
      text(g, cx, cy + 4, String(i), small ? 10.5 : 11.5);
    }
  }

  // knob = a fixed base (shadow, skirt, scale, mod arcs) + the turning cap on its own GPU layer, rotated with a
  // CSS transform, + a fixed highlight on top. Turning a knob then redraws nothing (17 Nov: the Fire HD 10 took
  // 120-150 ms a frame repainting knob tiles while one knob turned).
  function knobBody(svg, R, box, W) {
    sv('circle', { cx: 1.5, cy: R * 0.12, r: R * 1.02 + 4, fill: 'url(#gShadow)', opacity: 0.75 }, svg);
    sv('circle', { r: R, fill: 'url(#gSkirt)', stroke: '#5d5e5c', 'stroke-width': 1 }, svg);
    sv('circle', { r: R * 0.9, fill: 'none', stroke: 'rgba(0,0,0,0.12)', 'stroke-width': 0.6 }, svg);
    sv('circle', { r: R * 0.82, fill: 'none', stroke: 'rgba(255,255,255,0.35)', 'stroke-width': 0.6 }, svg);
    const arcs = sv('g', { class: 'arcs' }, svg);
    const S = R + 2;   // the cap layer covers the pointer line out to 0.985 R
    const rot = sv('svg', { class: 'knob-rot', width: 2 * S, height: 2 * S, viewBox: `${-S} ${-S} ${2 * S} ${2 * S}` }, box);
    place(rot, W / 2 - S, W / 2 - S);
    sv('line', { x1: 0, y1: -R * 0.76, x2: 0, y2: -R * 0.985, stroke: '#121212', 'stroke-width': Math.max(2, R * 0.07) }, rot);
    sv('circle', { r: R * 0.73, fill: '#060606' }, rot);
    const rr = R * 0.685, n = 36, c = 2 * Math.PI * rr / n;
    sv('circle', { r: rr, fill: 'none', stroke: '#303030', 'stroke-width': R * 0.085, 'stroke-dasharray': `${(c * 0.42).toFixed(2)} ${(c * 0.58).toFixed(2)}` }, rot);
    sv('circle', { r: R * 0.6, fill: 'url(#gCap)', stroke: '#000', 'stroke-width': 0.8 }, rot);
    sv('line', { x1: 0, y1: -R * 0.16, x2: 0, y2: -R * 0.6, stroke: '#efe8d6', 'stroke-width': Math.max(2.2, R * 0.075), 'stroke-linecap': 'round' }, rot);
    const hi = sv('svg', { class: 'knob-hi', width: 2 * S, height: 2 * S, viewBox: `${-S} ${-S} ${2 * S} ${2 * S}` }, box);
    place(hi, W / 2 - S, W / 2 - S);
    sv('ellipse', { cx: 0, cy: -R * 0.28, rx: R * 0.46, ry: R * 0.26, fill: 'url(#gHi)' }, hi);
    return [rot, arcs];
  }

  // a knob. o: {size, label, sub, labelRight, labelGap, dim(), noSlots}
  function knob(parent, id, cx, cy, o) {
    o = o || {};
    const d = M.P[id], size = o.size || 60, R = size / 2;
    const pad = d.id.endsWith('.wave') ? 40 : 30, W = size + pad * 2;
    const box = place(el('div', 'ctl', parent), cx - W / 2, cy - W / 2, W, W);
    const svg = sv('svg', { class: 'knob', width: W, height: W, viewBox: `${-W / 2} ${-W / 2} ${W} ${W}` }, box);
    const dst = o.noSlots ? undefined : M.KNOB_DST[id];
    drawScale(sv('g', {}, svg), d, R, size, dst !== undefined);
    const [rot, arcs] = knobBody(svg, R, box, W);
    const gap = o.labelGap || (dst ? 26 : 22);
    const lbl = el('div', 'lbl', box);
    lbl.textContent = o.label || d.label;
    if (o.sub) el('small', null, lbl, o.sub);
    if (o.labelRight) { lbl.style.left = `${W / 2 + R + 10}px`; lbl.style.right = 'auto'; lbl.style.textAlign = 'left'; lbl.style.top = `${W / 2 - 7}px`; }
    else lbl.style.top = `${W / 2 + R + gap}px`;
    let slotEls = null;
    if (dst !== undefined) {
      box.dataset.dst = dst;
      const row = place(el('div', 'mslots', box), W / 2 - 30, W / 2 + R + 5);
      slotEls = [0, 1, 2].map((k) => { const s = el('div', 'mslot', row); attachSlot(s, box, id, dst, k); return s; });
    }
    let shown = null, shownRoutes = '';
    const c = {
      id, el: box,
      update(force) {
        const avail = !d.available || d.available();
        box.classList.toggle('na', !avail || !!(o.dim && o.dim()));
        const v = M.get(patch, id);
        let rs = '';
        const routes = dst !== undefined ? M.routesTo(patch, dst).slice(0, 3) : [];
        if (dst !== undefined) rs = routes.map((i) => `${patch.mod[i].src}:${patch.mod[i].amount}`).join(',');
        if (!force && v === shown && rs === shownRoutes) return;
        shown = v; shownRoutes = rs;
        const a = angleOf(d, v);
        rot.style.transform = `rotate(${a.toFixed(2)}deg)`;
        if (slotEls) {
          arcs.replaceChildren();
          slotEls.forEach((s, k) => {
            const i = routes[k];
            if (i === undefined) { s.className = 'mslot'; s.textContent = ''; s.style.background = ''; return; }
            const m = patch.mod[i];
            s.className = 'mslot full'; s.textContent = M.SRC_SHORT[m.src] || '?'; s.style.background = ROUTE_COLORS[k];
            const amt = m.amount / 32767, a1 = clamp(a + amt * 300, -150, 150);
            if (Math.abs(a1 - a) > 0.5) sv('path', { d: arcPath(R - 3 - k * 3.6, a, a1), stroke: ROUTE_COLORS[k], 'stroke-width': 3, fill: 'none', 'stroke-linecap': 'round' }, arcs);
          });
        }
      },
    };
    attachKnobDrag(box, d, c, o);
    controls.push(c);
    return c;
  }

  // the 3 modulation slots under a knob: drag vertically = depth, double-tap = remove
  function attachSlot(s, box, id, dst, k) {
    let pid = null, lastY = 0, lastTap = 0, slot = -1;
    const desc = () => { const m = patch.mod[slot]; return [`${M.SRC_LABELS[m.src]} → ${M.DST_LABELS[dst]}`, `${m.amount >= 0 ? '+' : ''}${Math.round(m.amount / 327.67)}%`]; };
    s.addEventListener('pointerdown', (e) => {
      e.preventDefault(); e.stopPropagation();
      slot = M.routesTo(patch, dst)[k]; if (slot === undefined) { toast('Drag a modulation source (✛) onto the knob'); return; }
      const now = Date.now();
      if (now - lastTap < 320) { M.removeRoute(patch, slot); lastTap = 0; touched(); toast('Modulation removed'); return; }
      lastTap = now;
      pid = e.pointerId; lastY = e.clientY; try { s.setPointerCapture(pid); } catch (x) { /* */ }
      showTip(box, ...desc());
    });
    s.addEventListener('pointermove', (e) => {
      if (e.pointerId !== pid) return;
      const dy = (lastY - e.clientY) / SCALE; lastY = e.clientY;
      if (Math.abs(dy) > 0) { lastTap = 0; M.setRouteAmountAt(patch, slot, patch.mod[slot].amount / 32767 + dy / 200); touched(); showTip(box, ...desc()); }
    });
    const end = (e) => { if (e.pointerId !== pid) return; pid = null; setTimeout(hideTip, 500); };
    s.addEventListener('pointerup', end); s.addEventListener('pointercancel', end);
  }

  function attachKnobDrag(box, d, c, o) {
    let pid = null, lastY = 0, startY = 0, startX = 0, acc = 0, fine = false, moved = false, lpTimer = 0, lastTap = 0;
    const span = d.max - d.min, label = (o && o.label) || d.label;
    const show = () => showTip(box, fine ? `${label} · FINE` : label, M.format(d.id, M.get(patch, d.id), patch));
    box.addEventListener('pointerdown', (e) => {
      if (d.available && !d.available()) { toast(NA_TEXT); return; }
      e.preventDefault();
      if (pid !== null) return;   // a second finger belongs to pinch-zoom (17 Nov); long-press still gives fine
      const pick = e.target && e.target.getAttribute && e.target.getAttribute('data-v');
      if (pick !== null && pick !== undefined) { edit(d.id, Number(pick)); showTip(box, label, M.format(d.id, M.get(patch, d.id), patch)); setTimeout(hideTip, 700); return; }
      const now = Date.now();
      if (now - lastTap < 320) { lastTap = 0; edit(d.id, d.def); showTip(box, `${label} · DEFAULT`, M.format(d.id, M.get(patch, d.id), patch)); setTimeout(hideTip, 700); return; }
      lastTap = now;
      pid = e.pointerId; try { box.setPointerCapture(pid); } catch (x) { /* */ }
      lastY = startY = e.clientY; startX = e.clientX; moved = false; fine = !!(e.shiftKey || e.ctrlKey);   // desktop: shift/ctrl-drag = fine
      acc = M.get(patch, d.id);
      box.classList.add('active');
      lpTimer = setTimeout(() => { if (!moved) { fine = true; show(); } }, 450);   // long-press: fine
      if (e.pointerType === 'mouse') {   // desktop: keep following the mouse even if the capture is lost outside the knob
        window.addEventListener('pointermove', winMove, true);
        window.addEventListener('pointerup', winEnd, true); window.addEventListener('pointercancel', winEnd, true);
      }
      show();
    });
    const move = (e) => {
      if (e.pointerId !== pid) return;
      const dy = (lastY - e.clientY) / SCALE; lastY = e.clientY;
      if (!moved && (Math.abs(e.clientY - startY) + Math.abs(e.clientX - startX)) / SCALE > 6) { moved = true; lastTap = 0; }
      if (!moved) return;
      if (e.shiftKey || e.ctrlKey) fine = true;
      const px = d.kind === 'detent' ? 30 * (d.max - d.min + 1) : 260;
      acc = clamp(acc + dy * span / px * (fine ? 0.12 : 1), d.min, d.max);
      const v = d.kind === 'detent' ? Math.round(acc) : acc;
      if (v !== M.get(patch, d.id)) edit(d.id, v);
      show();
    };
    box.addEventListener('pointermove', move);
    const winMove = (e) => { if (e.pointerId === pid && !box.contains(e.target)) move(e); };
    const winEnd = (e) => { if (e.pointerId === pid && !box.contains(e.target)) end(e); };
    const end = (e) => {
      if (e.pointerId !== pid) return;
      window.removeEventListener('pointermove', winMove, true);
      window.removeEventListener('pointerup', winEnd, true); window.removeEventListener('pointercancel', winEnd, true);
      pid = null; clearTimeout(lpTimer); box.classList.remove('active');
      setTimeout(() => { if (pid === null) hideTip(); }, moved ? 350 : 600);
    };
    box.addEventListener('pointerup', end);
    box.addEventListener('pointercancel', end);
    // desktop (VST3 plugin, browsers): the mouse wheel turns the knob, shift/ctrl = fine. Touch never sends wheel events.
    let wheelTimer = 0;
    box.addEventListener('wheel', (e) => {
      if (d.available && !d.available()) return;
      e.preventDefault();
      const delta = e.deltaY || e.deltaX;   // shift+wheel scrolls sideways in Chromium
      if (!delta) return;
      const dir = delta < 0 ? 1 : -1, wfine = e.shiftKey || e.ctrlKey;
      const cur = M.get(patch, d.id);
      let v;
      if (d.kind === 'detent') v = clamp(Math.round(cur) + dir, d.min, d.max);
      else if (d.step) v = clamp(cur + dir * d.step, d.min, d.max);
      else v = clamp(cur + dir * span * (wfine ? 0.004 : 0.025), d.min, d.max);
      if (v !== cur) edit(d.id, v);
      showTip(box, wfine ? `${label} · FINE` : label, M.format(d.id, M.get(patch, d.id), patch));
      clearTimeout(wheelTimer); wheelTimer = setTimeout(() => { if (pid === null) hideTip(); }, 700);
    }, { passive: false });
  }

  // rocker switch (0/1). id = a model parameter, or o.get/o.set for UI-only switches
  function rocker(parent, id, x, y, color, label, o) {
    o = o || {};
    const d = id ? M.P[id] : null;
    const r = place(el('div', `rocker ${color}`, parent), x, y);
    el('div', 'face', r); el('div', 'dot', r);
    if (label) place(el('div', 'rk-lbl', parent, label), x + 19 - 40, y - 18);
    const get = o.get || (() => M.get(patch, id));
    let shown = null;
    const c = {
      el: r,
      update(force) {
        if (d && d.available) r.classList.toggle('na', !d.available());
        const v = get(); if (!force && v === shown) return; shown = v; r.classList.toggle('on', !!v);
      },
    };
    r.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      if (d && d.available && !d.available()) { toast(NA_TEXT); return; }
      const v = get() ? 0 : 1;
      if (o.set) { o.set(v); c.update(true); } else edit(id, v);
    });
    controls.push(c);
    return c;
  }

  // a vertical bank of lit selections
  function bank(parent, id, x, y, w, rowH, title, fs) {
    const d = M.P[id];
    const b = place(el('div', 'bank', parent), x, y, w);
    if (title) el('div', 'bank-title', b, title);
    const opts = d.labels.map((lab, i) => {
      const o = el('div', 'opt', b); o.style.height = `${rowH}px`; if (fs) o.style.fontSize = `${fs}px`;
      const led = el('span', 'led', o); el('span', null, o, lab);
      o.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        if (d.available && !d.available()) { toast(NA_TEXT); return; }
        edit(id, d.min + i);
      });
      return [o, led];
    });
    let shown = null;
    controls.push({ update(force) {
      if (d.available) b.classList.toggle('na', !d.available());
      const v = M.get(patch, id); if (!force && v === shown) return; shown = v;
      opts.forEach(([o, led], i) => { const on = d.min + i === v; o.classList.toggle('on', on); led.classList.toggle('on', on); });
    } });
    return b;
  }

  // chips: an amber readout that opens a picker (or cycles with o.cycle)
  function chip(parent, id, x, y, w, title, o) {
    o = o || {};
    const d = M.P[id];
    const c = place(el('div', o.cycle ? 'chip cycle' : 'chip', parent), x, y, w);
    if (o.h) { c.style.height = `${o.h}px`; c.style.fontSize = '11.5px'; }
    let t = null;
    if (title) t = place(el('div', 'chip-lbl', parent, title), x + 2, y - 15);
    let shown = null;
    controls.push({ update(force) {
      const na = (d.available && !d.available()) || (o.dim && o.dim());
      c.classList.toggle('na', !!na);
      const v = M.get(patch, id); if (!force && v === shown) return; shown = v; c.textContent = M.format(id, v, patch);
    } });
    c.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      if (d.available && !d.available()) { toast(NA_TEXT); return; }
      const v = M.get(patch, id);
      if (o.cycle) { edit(id, v >= d.max ? d.min : v + 1); return; }
      showPicker(title || d.label, d.labels.map((l) => ({ label: l })), v - d.min, (i) => edit(id, d.min + i), c, o.cols);
    });
    c.titleEl = t;
    return c;
  }

  // ------------------------------------------------------------------- picker ---
  const pickerEl = $('picker');
  function showPicker(title, options, cur, onPick, anchor, cols) {
    pickerEl.replaceChildren(); pickerEl.classList.remove('hidden');
    const pk = el('div', 'pk', pickerEl);
    cols = cols || (options.length > 16 ? 5 : options.length > 8 ? 4 : Math.min(options.length, 3));
    pk.style.gridTemplateColumns = `repeat(${cols}, ${options.length > 16 ? 176 : 190}px)`;
    el('div', 'pk-title', pk, title);
    options.forEach((op, i) => {
      const b = el('div', i === cur ? 'pk-opt on' : 'pk-opt', pk);
      if (op.icon) b.appendChild(op.icon);
      el('span', null, b, op.label);
      b.addEventListener('pointerup', (e) => { e.preventDefault(); e.stopPropagation(); pickerEl.classList.add('hidden'); onPick(i); });
    });
    const a = anchor ? stageRect(anchor) : { x: 960, y: 300, w: 0, h: 0 };
    const pw = pk.offsetWidth, ph = pk.offsetHeight;
    let y = a.y + a.h + 10;
    if (y + ph > 1180) y = a.y - ph - 10;
    place(pk, clamp(a.x + a.w / 2 - pw / 2, 20, 1900 - pw), clamp(y, 20, 1180 - ph));
  }
  pickerEl.addEventListener('pointerdown', (e) => { if (e.target === pickerEl) { e.preventDefault(); pickerEl.classList.add('hidden'); } });

  // ------------------------------------------------------- drag-to-assign ---
  // a source handle (✛): drag it onto a knob with modulation slots to add a route
  function sourceHandle(parent, src, x, y, o) {
    o = o || {};
    const h = place(el('div', o.cls ? `src-handle ${o.cls}` : 'src-handle', parent, o.text || '✛'), x, y);
    let pid = null, ghost = null, sx = 0, sy = 0, moved = false;
    h.addEventListener('pointerdown', (e) => {
      e.preventDefault(); e.stopPropagation();
      pid = e.pointerId; sx = e.clientX; sy = e.clientY; moved = false;
      try { h.setPointerCapture(pid); } catch (x) { /* */ }
    });
    h.addEventListener('pointermove', (e) => {
      if (e.pointerId !== pid) return;
      if (!moved && (Math.abs(e.clientX - sx) + Math.abs(e.clientY - sy)) / SCALE > 8) {
        moved = true; stage.classList.add('assigning');
        ghost = el('div', 'src-ghost', stage, M.SRC_SHORT[src]);
      }
      if (ghost) {
        const [gx, gy] = toStage(e.clientX, e.clientY); place(ghost, gx - 26, gy - 60);
        const t = document.elementFromPoint(e.clientX, e.clientY), k = t && t.closest ? t.closest('.ctl[data-dst]') : null;
        document.querySelectorAll('.ctl.drop').forEach((n) => { if (n !== k) n.classList.remove('drop'); });
        if (k) k.classList.add('drop');
      }
    });
    const end = (e) => {
      if (e.pointerId !== pid) return; pid = null;
      stage.classList.remove('assigning');
      document.querySelectorAll('.ctl.drop').forEach((n) => n.classList.remove('drop'));
      if (ghost) { ghost.remove(); ghost = null; }
      if (!moved) { if (o.onTap) o.onTap(); else toast(`Drag ${M.SRC_LABELS[src]} onto a knob to modulate it`); return; }
      if (e.type === 'pointercancel') return;
      const t = document.elementFromPoint(e.clientX, e.clientY), k = t && t.closest ? t.closest('.ctl[data-dst]') : null;
      if (!k) return;
      const dst = Number(k.dataset.dst), r = M.addRoute(patch, src, dst, 0.3);
      if (r === -2) toast('This knob already has three modulation slots');
      else if (r === -1) toast('All 16 modulation slots are in use (MATRIX)');
      else { touched(); toast(`${M.SRC_LABELS[src]} → ${M.DST_LABELS[dst]}: drag its slot under the knob for depth`); }
    };
    h.addEventListener('pointerup', end); h.addEventListener('pointercancel', end);
    return h;
  }

  // ------------------------------------------------------------- main panel ---
  function svgDefs() {
    const s = sv('svg', { width: 0, height: 0, style: 'position:absolute' }, stage);
    const defs = sv('defs', {}, s);
    const lg = sv('linearGradient', { id: 'gSkirt', x1: 0, y1: 0, x2: 1, y2: 1 }, defs);
    [['0', '#fbfbf9'], ['0.32', '#d4d5d3'], ['0.55', '#a3a4a2'], ['0.78', '#c9cac8'], ['1', '#8a8b89']].forEach(([o, c]) => sv('stop', { offset: o, 'stop-color': c }, lg));
    const rg = sv('radialGradient', { id: 'gCap', cx: 0.45, cy: 0.35, r: 0.75 }, defs);
    [['0', '#3c3c3c'], ['0.55', '#161616'], ['1', '#050505']].forEach(([o, c]) => sv('stop', { offset: o, 'stop-color': c }, rg));
    const hi = sv('linearGradient', { id: 'gHi', x1: 0, y1: 0, x2: 0, y2: 1 }, defs);
    sv('stop', { offset: 0, 'stop-color': '#fff', 'stop-opacity': 0.2 }, hi); sv('stop', { offset: 1, 'stop-color': '#fff', 'stop-opacity': 0 }, hi);
    // soft knob shadow as a gradient: same look as a 3.2 px blur, no filter (17 Nov: filters made the tablet repaint slowly)
    const sh = sv('radialGradient', { id: 'gShadow', cx: 0.5, cy: 0.5, r: 0.5 }, defs);
    [['0', 0.85], ['0.80', 0.75], ['0.90', 0.35], ['1', 0]].forEach(([o, a]) => sv('stop', { offset: o, 'stop-color': '#000', 'stop-opacity': a }, sh));
  }
  function section(parent, title, w, cls) {
    const s = el('div', cls ? `section ${cls}` : 'section', parent); s.style.width = `${w}px`;
    el('div', 'sec-head', s, title);
    return el('div', 'sec-body', s);
  }
  function silk(body, str, x, y, cls) { const s = el('div', cls ? `silk ${cls}` : 'silk', body, str); place(s, x, y); return s; }
  function rule(body, x, y, w) { place(el('div', 'rule', body), x, y, w); }
  const UROW = [0, 130, 260], UKY = 56;

  let lampEl = null, a440 = false;
  function buildPanel() {
    const up = $('upper'), lo = $('lower');
    // ---- OSCILLATORS
    let b = section(up, 'OSCILLATORS', 730);
    for (let i = 0; i < 3; i++) {
      const n = i + 1, t = UROW[i], cy = t + UKY;
      silk(b, `OSC ${n}`, 14, t + 10, 'big');
      rocker(b, `osc${n}.on`, 22, t + 44, 'blue', null);
      knob(b, `osc${n}.range`, 136, cy, { size: 50, noSlots: true });
      knob(b, `osc${n}.freq`, 248, cy, { size: 54, label: 'PITCH' });
      knob(b, `osc${n}.wave`, 368, cy, { size: 50 });
      knob(b, `osc${n}.warp`, 486, cy, { size: 54 });
      knob(b, `osc${n}.vol`, 590, cy, { size: 54, label: 'AMP' });
      knob(b, `osc${n}.route`, 680, cy, { size: 44 });
      if (i < 2) rule(b, 10, t + 129, 710);
    }
    // ---- MOD OSC / NOISE / FEEDBACK
    b = section(up, 'MOD OSC · NOISE · FEEDBACK', 330, 'narrow-head');
    silk(b, 'MOD OSC', 10, UROW[0] + 6, 'row');
    knob(b, 'mo.semis', 50, UROW[0] + UKY + 6, { size: 46 });
    [['rm', 'RING'], ['pm', 'PHASE'], ['pos', 'WAVE']].forEach(([k, lab], j) => {
      const cx = 138 + j * 74;
      knob(b, `mo.${k}`, cx, UROW[0] + UKY + 6 - 8, { size: 42, label: lab, labelGap: 32 });
      chip(b, `mo.${k}_osc`, cx - 31, UROW[0] + UKY + 6 + 18, 62, null, { cycle: true, h: 22 });
    });
    rule(b, 10, UROW[0] + 129, 310);
    silk(b, 'NOISE', 10, UROW[1] + 6, 'row');
    knob(b, 'noise.color', 70, UROW[1] + UKY + 6, { size: 50, label: 'COLOUR' });
    knob(b, 'noise.vol', 180, UROW[1] + UKY + 6, { size: 54, label: 'AMP' });
    knob(b, 'noise.route', 280, UROW[1] + UKY + 6, { size: 44 });
    rule(b, 10, UROW[1] + 129, 310);
    silk(b, 'FEEDBACK', 10, UROW[2] + 6, 'row');
    knob(b, 'feedback.amt', 70, UROW[2] + UKY + 6, { size: 50 });
    knob(b, 'feedback.route', 170, UROW[2] + UKY + 6, { size: 44 });
    knob(b, 'drift', 272, UROW[2] + UKY + 6, { size: 44, label: 'DRIFT', sub: 'ANALOG' });
    // ---- FILTERS
    b = section(up, 'FILTERS', 500);
    for (const [r, f, sel] of [[0, 'f1', 'f1.type'], [1, 'f2', 'f2.type']]) {
      const t = UROW[r], cy = t + UKY + 2;
      silk(b, r === 0 ? 'FILTER 1' : 'FILTER 2', 10, t + 6, 'row');
      bank(b, sel, 4, t + 22, 98, r === 0 ? 14.5 : 13, null, 10.5);
      knob(b, `${f}.cutoff`, 170, cy, { size: 62 });
      knob(b, `${f}.res`, 270, cy, { size: 52 });
      knob(b, `${f}.drive`, 360, cy, { size: 46 });
      knob(b, `${f}.kbd`, 446, cy, { size: 46 });
      rule(b, 10, t + 129, 480);
    }
    silk(b, 'ROUTING · MIX', 10, UROW[2] + 6, 'row');
    knob(b, 'serpar', 150, UROW[2] + UKY + 2, { size: 56 });
    knob(b, 'f1.mix', 280, UROW[2] + UKY + 2, { size: 50 });
    knob(b, 'f2.mix', 400, UROW[2] + UKY + 2, { size: 50 });
    // ---- MASTER
    b = section(up, 'MASTER', 232);
    knob(b, 'master', 78, UROW[0] + UKY + 2, { size: 64, label: 'VOLUME' });
    lampEl = place(el('div', 'lamp', b), 166, UROW[0] + 30);
    silk(b, 'PILOT', 167, UROW[0] + 74, 'dim');
    rule(b, 10, UROW[0] + 129, 212);
    knob(b, 'tune', 62, UROW[1] + UKY + 2, { size: 54 });
    knob(b, 'glide', 160, UROW[1] + UKY - 8, { size: 42, labelGap: 30 });
    rocker(b, 'glide.on', 192, UROW[1] + 34, 'white', null);
    rule(b, 10, UROW[1] + 129, 212);
    bank(b, 'voice', 4, UROW[2] + 8, 96, 24, 'VOICE', 11.5);
    rocker(b, null, 118, UROW[2] + 34, 'white', 'A-440', {
      get: () => (a440 ? 1 : 0),
      set: (v) => { a440 = !!v; if (a440) Bridge.noteOn(69, 100); else Bridge.noteOff(69); },
    });
    chip(b, 'bend', 166, UROW[2] + 44, 58, 'BEND', { cols: 6 });

    // ---- MODULATION (lower left)
    b = section(lo, 'MODULATION', 1100);
    buildModulators(b);
    // ---- MACROS
    b = section(lo, 'MACRO CONTROL', 330);
    for (let i = 0; i < 8; i++) {
      const cx = 44 + (i % 4) * 80, cy = i < 4 ? 50 : 168;
      knob(b, `macro${i + 1}`, cx, cy, { size: 44, label: '', noSlots: true });
      const h = sourceHandle(b, M.SRC.NONE + 14 + i, cx - 28, cy + 30, { text: `✛ ${i + 1}`, cls: 'macro' });
      const dl = place(el('div', 'macro-dest', b), cx - 40, cy + 58, 80);
      controls.push({ update() {
        const k = patch.mod.findIndex((m) => m.src === 14 + i && m.dst !== 0);
        dl.textContent = k < 0 ? '—' : M.DST_LABELS[patch.mod[k].dst];
      } });
      void h;
    }
    // ---- EFFECTS (mini strip; full units in the EFFECTS overlay)
    b = section(lo, 'EFFECTS', 362);
    M.FX_UNITS.forEach((u, i) => {
      const x = 6 + (i % 3) * 118, y = i < 3 ? 6 : 140;
      const box = place(el('div', 'fx-mini', b), x, y, 112, 128);
      const head = el('div', 'fx-mini-head', box, u.title);
      head.addEventListener('pointerdown', (e) => { e.preventDefault(); toggleOverlay('fx'); });
      const mixId = u.id === 'tape' ? 'tape.drive' : u.id === 'fuzz' ? 'fuzz.amount' : `${u.id}.mix`;
      knob(box, mixId, 34, 72, { size: 38, label: u.id === 'tape' ? 'DRIVE' : u.id === 'fuzz' ? 'FUZZ' : 'MIX', labelGap: 12 });
      if (u.level) rocker(box, `${u.id}.on`, 68, 38, 'blue', null);
      else chip(box, 'ensemble.mode', 62, 56, 46, null, { cycle: true, h: 26 });
    });
  }

  // ---------------------------------------------- modulators: ENV 1-4, 5-8 ---
  let modTab = 0;     // 0-3 envelopes, 4-7 modulator slots (lfo[0..3])
  function lfoPath(wave, w, h) {
    let d = '';
    const mid = h / 2, A = h / 2 - 12, N = 160, cycles = 2, rnd = [0.6, -0.2, 0.9, -0.7, 0.1, -0.95, 0.4, -0.4];
    for (let k = 0; k <= N; k++) {
      const x = k / N, ph = (x * cycles) % 1;
      let v;
      switch (wave) {
        case 0: v = Math.sin(2 * Math.PI * ph); break;
        case 1: v = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4; break;
        case 2: v = 2 * ph - 1; break;
        case 3: v = 1 - 2 * ph; break;
        case 4: v = ph < 0.5 ? 1 : -1; break;
        default: v = rnd[Math.min(7, Math.floor(x * 8))];
      }
      d += `${k ? 'L' : 'M'}${(10 + x * (w - 20)).toFixed(1)} ${(mid - v * A).toFixed(1)} `;
    }
    return d;
  }
  function envPath(e, w, h) {
    // delay, attack -> A-level, decay -> D-level, -> sustain over S-time, hold, release; time axis ~ log-ish
    const t = (q) => 0.12 + Math.pow(clamp(q / 32767, 0, 1), 1.6) * 1.0;
    const segs = [t(e.delay) * (e.delay > 0 ? 1 : 0), t(e.attack), t(e.decay), e.sust_time > 0 ? t(e.sust_time) : 0, 0.7, t(e.release)];
    const tot = segs.reduce((a, b) => a + b, 0), X = (s) => 12 + (s / tot) * (w - 24), Y = (lv) => h - 14 - clamp(lv, 0, 1) * (h - 34);
    const al = clamp(e.att_level / 32767, 0, 1), dl = e.dec_level < 0 ? e.sustain / 32767 : e.dec_level / 32767, sl = e.sustain / 32767;
    let s = 0; const pts = [[X(0), Y(0)]];
    s += segs[0]; pts.push([X(s), Y(0)]);
    s += segs[1]; pts.push([X(s), Y(al)]);
    s += segs[2]; pts.push([X(s), Y(dl)]);
    s += segs[3]; pts.push([X(s), Y(sl)]);
    s += segs[4]; pts.push([X(s), Y(sl)]);
    s += segs[5]; pts.push([X(s), Y(0)]);
    return [pts.map((p, i) => `${i ? 'L' : 'M'}${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join(' '), pts];
  }
  function shapeIcon(id) {
    const s = sv('svg', { viewBox: '0 0 46 30' });
    let d = '';
    for (let k = 0; k <= 30; k++) { const x = k / 30; d += `${k ? 'L' : 'M'}${(2 + x * 42).toFixed(1)} ${(27 - M.perfShape(id, Math.min(x, 0.9999)) * 24).toFixed(1)} `; }
    sv('path', { d }, s);
    return s;
  }

  function stepsEditor(parent, x, y, w, h) {
    const box = place(el('div', 'steps', parent), x, y, w, h);
    const svg = sv('svg', { viewBox: `0 0 ${w} ${h}`, preserveAspectRatio: 'none' }, box);
    const sw = w / 16, mid = h / 2 - 6, A = mid - 8;
    let sig = '';
    const slot = () => modTab - 4;
    function draw(force) {
      if (modTab < 4) return;
      const l = patch.lfo[slot()], kind = l.wave === M.W.PERFORMER ? 1 : l.wave === M.W.STEPPER ? 2 : 0;
      const s = JSON.stringify([modTab, kind, l.step_shape, l.step_level, l.loop_start, l.loop_end]);
      if (!force && s === sig) return; sig = s;
      svg.replaceChildren();
      if (!kind) return;
      const a = Math.min(l.loop_start, l.loop_end), b = Math.max(l.loop_start, l.loop_end);
      for (let k = 0; k < 16; k++) if (k < a || k > b) sv('rect', { x: k * sw, y: 0, width: sw, height: h, class: 'out' }, svg);
      for (let k = 1; k < 16; k++) sv('line', { x1: k * sw, x2: k * sw, y1: 0, y2: h - 16, class: k % 4 ? 'grid' : 'grid beat' }, svg);
      sv('line', { x1: 0, x2: w, y1: mid, y2: mid, class: 'grid beat' }, svg);
      if (kind === 1) {
        let d = '';
        for (let k = 0; k < 16; k++) for (let j = 0; j <= 14; j++) {
          const xx = j / 14, v = M.perfShape(l.step_shape[k], Math.min(xx, 0.9999)) * l.step_level[k] / 127;
          d += `${k === 0 && j === 0 ? 'M' : 'L'}${(k * sw + xx * sw).toFixed(1)} ${(mid - v * A).toFixed(1)} `;
        }
        sv('path', { d, class: 'curve' }, svg);
      } else {
        for (let k = 0; k < 16; k++) {
          const v = l.step_level[k] / 127, top = v >= 0 ? mid - v * A : mid, hh = Math.max(1.5, Math.abs(v) * A);
          sv('rect', { x: k * sw + 3, y: top, width: sw - 6, height: hh, rx: 1.5, class: k % 4 ? 'bar' : 'bar beat' }, svg);
        }
      }
      for (let k = 0; k < 16; k++) text(svg, k * sw + sw / 2, h - 4, String(k + 1), 10, 'stepn');
      const t = sv('text', { x: w - 8, y: 14, class: 'hint', 'text-anchor': 'end' }, svg);
      t.textContent = kind === 1 ? 'TAP A STEP: SHAPE · DRAG: LEVEL' : 'DRAG ACROSS THE STEPS: LEVEL';
    }
    views.push(draw);
    let pid = null, moved = false, sy = 0, col = 0;
    const at = (e) => { const r = stageRect(box); const [px, py] = toStage(e.clientX, e.clientY); return [clamp(Math.floor((px - r.x) / (r.w / 16)), 0, 15), clamp((mid - (py - r.y) * (h / r.h)) / A, -1, 1)]; };
    const setLevel = (k, v) => { patch.lfo[slot()].step_level[k] = clamp(Math.round(v * 127), -127, 127); touched(); };
    box.addEventListener('pointerdown', (e) => {
      if (modTab < 4) return;
      const l = patch.lfo[slot()]; if (l.wave < M.W.PERFORMER) return;
      e.preventDefault(); pid = e.pointerId; try { box.setPointerCapture(pid); } catch (x) { /* */ }
      moved = false; sy = e.clientY; [col] = at(e);
      if (l.wave === M.W.STEPPER) { const [k, v] = at(e); setLevel(k, v); moved = true; }
    });
    box.addEventListener('pointermove', (e) => {
      if (e.pointerId !== pid) return;
      if (!moved && Math.abs(e.clientY - sy) / SCALE > 5) moved = true;
      if (moved) { const [k, v] = at(e); setLevel(k, v); }
    });
    const end = (e) => {
      if (e.pointerId !== pid) return; pid = null;
      if (!moved && patch.lfo[slot()].wave === M.W.PERFORMER) {
        const k = col, i = slot();
        showPicker(`STEP ${k + 1} SHAPE`, M.PERF_NAMES.map((n, id) => ({ label: n, icon: shapeIcon(id) })), patch.lfo[i].step_shape[k],
          (id) => { patch.lfo[i].step_shape[k] = id; touched(); }, box, 4);
      }
    };
    box.addEventListener('pointerup', end); box.addEventListener('pointercancel', end);
    return box;
  }

  function buildModulators(b) {
    // tabs: ENV 1-4 and modulator slots 5-8, each with a drag handle (✛)
    const tabs = [];
    for (let k = 0; k < 8; k++) {
      const x = 8 + k * 112;
      const t = place(el('div', 'tab', b), x, 6, 106);
      const name = el('span', 'tab-name', t);
      const src = k < 4 ? 1 + k : 5 + (k - 4);
      sourceHandle(t, src, 4, 6, { onTap: () => selectTab(k) });
      t.addEventListener('pointerdown', (e) => { e.preventDefault(); selectTab(k); });
      tabs.push([t, name]);
    }
    controls.push({ update() {
      tabs.forEach(([t, name], k) => {
        t.classList.toggle('on', k === modTab);
        name.textContent = k < 4 ? `ENV ${k + 1}` : `${k + 1} ${['LFO', 'PERF', 'STEP'][M.get(patch, `lfo${k - 3}.kind`)]}`;
      });
    } });
    // other sources
    silk(b, 'SOURCES', 918, 2, 'dim');
    [[9, 'VEL'], [10, 'KEY'], [11, 'MW']].forEach(([src, lab], j) => sourceHandle(b, src, 914 + j * 60, 16, { text: `✛ ${lab}`, cls: 'wide' }));

    // ENV editor
    const envBox = place(el('div', 'mod-pane', b), 0, 58, 1100, 222);
    const envKeys = ['delay', 'attack', 'att_level', 'decay', 'dec_level', 'sustain', 'sust_time', 'release'];
    const envKnobs = [];
    for (let e = 0; e < 4; e++) {
      const g = el('div', 'env-set', envBox);
      envKeys.forEach((key, j) => envKnobs.push([e, knob(g, `env${e + 1}.${key}`, 52 + j * 80, 84, { size: 50 })]));
      envKnobs.push([e, rocker(g, `env${e + 1}.loop`, 676, 52, 'white', 'LOOP')]);
    }
    const graph = place(el('div', 'env-graph', envBox), 730, 10, 360, 200);
    const gsvg = sv('svg', { viewBox: '0 0 360 200', width: 360, height: 200 }, graph);
    for (let k = 1; k < 6; k++) sv('line', { x1: k * 60, x2: k * 60, y1: 8, y2: 192, class: 'grid' }, gsvg);
    const gfill = sv('path', { class: 'env-fill' }, gsvg), gpath = sv('path', { class: 'curve' }, gsvg);
    const gdots = sv('g', {}, gsvg);
    const glabel = sv('text', { x: 348, y: 22, class: 'hint', 'text-anchor': 'end' }, gsvg);
    let gsig = '';
    views.push(() => {
      if (modTab >= 4) return;
      const e = patch.env[modTab], s = JSON.stringify([modTab, e]);
      if (s === gsig) return; gsig = s;
      const [d, pts] = envPath(e, 360, 200);
      gpath.setAttribute('d', d); gfill.setAttribute('d', `${d} L${pts[pts.length - 1][0]} 186 L12 186 Z`);
      gdots.replaceChildren(); pts.slice(1, -1).forEach(([x, y]) => sv('circle', { cx: x, cy: y, r: 3.2, class: 'dot' }, gdots));
      glabel.textContent = modTab === 3 ? 'ENVELOPE 4 · LOUDNESS' : `ENVELOPE ${modTab + 1}`;
    });

    // modulator 5-8 editor
    const lfoBox = place(el('div', 'mod-pane', b), 0, 58, 1100, 222);
    const lfoSets = [];
    for (let i = 0; i < 4; i++) {
      const n = i + 1, g = el('div', 'lfo-set', lfoBox), kindOf = () => M.get(patch, `lfo${n}.kind`);
      const kb = place(el('div', 'slot-btn', g), 14, 6);
      controls.push({ update() { kb.textContent = M.SLOT_KINDS[kindOf()]; } });
      kb.addEventListener('pointerdown', (e) => { e.preventDefault(); edit(`lfo${n}.kind`, (kindOf() + 1) % 3); });
      const synced = () => M.get(patch, `lfo${n}.sync`) === 1;
      knob(g, `lfo${n}.rate`, 56, 108, { size: 54, dim: synced });
      rocker(g, `lfo${n}.sync`, 118, 76, 'blue', 'SYNC');
      chip(g, `lfo${n}.div`, 170, 88, 92, 'DIVISION', { dim: () => !synced() });
      rocker(g, `lfo${n}.key`, 282, 76, 'white', 'RETRIG');
      rocker(g, `lfo${n}.uni`, 336, 76, 'white', 'UNIPOLAR');
      const wchip = chip(g, `lfo${n}.wave`, 392, 88, 116, 'WAVE');
      const gl = knob(g, `lfo${n}.glide`, 440, 108, { size: 46 });
      const view = place(el('div', 'lfo-wave-view', g), 530, 10, 560, 200);
      const vsvg = sv('svg', { viewBox: '0 0 560 200', width: 560, height: 200 }, view);
      for (let k = 1; k < 8; k++) sv('line', { x1: k * 70, x2: k * 70, y1: 8, y2: 192, class: 'grid' }, vsvg);
      sv('line', { x1: 0, x2: 560, y1: 100, y2: 100, class: 'grid beat' }, vsvg);
      const vpath = sv('path', { d: '', class: 'curve' }, vsvg);
      let lastWave = -1;
      views.push(() => {
        const k = kindOf();
        view.classList.toggle('hidden', k !== 0); wchip.classList.toggle('hidden', k !== 0); wchip.titleEl.classList.toggle('hidden', k !== 0);
        gl.el.classList.toggle('hidden', k !== 2);
        const w = patch.lfo[i].wave;
        if (k === 0 && w !== lastWave) { lastWave = w; vpath.setAttribute('d', lfoPath(w, 560, 200)); }
        if (k !== 0) lastWave = -1;
      });
      lfoSets.push(g);
    }
    const steps = stepsEditor(lfoBox, 530, 10, 560, 200);
    views.push(() => {
      envBox.classList.toggle('hidden', modTab >= 4); lfoBox.classList.toggle('hidden', modTab < 4);
      envBox.querySelectorAll('.env-set').forEach((g, e) => g.classList.toggle('hidden', e !== modTab));
      lfoSets.forEach((g, i) => g.classList.toggle('hidden', i !== modTab - 4));
      steps.classList.toggle('hidden', modTab < 4 || M.get(patch, `lfo${modTab - 3}.kind`) === 0);
    });
    void envKnobs;
  }
  function selectTab(k) { modTab = k; refresh(true); }

  // -------------------------------------------------------------- overlays ---
  function card(parent, x, y, w, h, title) {
    const c = place(el('div', 'card', parent), x, y, w, h);
    const head = el('div', 'card-head', c);
    el('span', null, head, title);
    el('span', 'grow', head);
    return { c, head };
  }
  function buildFxOverlay() {
    const ov = $('overlay-fx');
    M.FX_UNITS.forEach((u, i) => {
      const x = 14 + i * 296;
      const { c } = card(ov, x, 14, 282, 672, u.title);
      c.classList.add('fx-unit');
      const pos = [[76, 150], [206, 150], [76, 320], [206, 320], [141, 480]];
      let k = 0;
      for (const [key, label] of u.knobs) {
        const id = `${u.id}.${key}`;
        if (id === 'ensemble.mode') { bank(c, id, 40, 300, 200, 40, 'MODE', 15); continue; }
        if (id === 'reverb.type') { bank(c, id, 60, 70, 170, 34, 'TYPE', 14); continue; }
        const [cx, cy] = u.id === 'ensemble' ? [141, 150] : u.id === 'reverb' ? [[76, 250], [206, 250], [76, 420], [206, 420]][k++] : pos[k++];
        knob(c, id, cx, cy, { size: 72, label, noSlots: true });
      }
      if (u.level) {
        const led = place(el('span', 'led red ped-led', c), 135, 568);
        const lvl = u.level;
        controls.push({ update() { led.classList.toggle('on', M.fxIsOn(patch, lvl)); } });
        rocker(c, `${u.id}.on`, 122, 592, 'blue', null);
      }
      if (u.id === 'echo' || u.id === 'oilcan') {   // tempo sync (17 Nov 2026): SYNC rocker, division chip, what plays
        const which = u.id === 'echo' ? 0 : 1;
        rocker(c, `${u.id}.sync`, 214, 592, 'white', 'SYNC');
        chip(c, `${u.id}.div`, 14, 592, 92, 'DIVISION', { dim: () => !M.get(patch, `${u.id}.sync`) });
        const rd = place(el('div', 'rk-lbl', c), 0, 650, 282);
        rd.style.width = '282px';
        controls.push({ update() { rd.textContent = `PLAYS ${M.fmtDelay(patch, which)}`; } });
      }
    });
    const note = el('div', 'ov-note', ov,
      'Dimmed controls arrive with the new effects engine; until then ENSEMBLE, BBD ECHO, REVERB and TAPE drive play on the current chorus, delay, reverb and drive.');
    place(note, 16, 700);
  }
  function buildMatrixOverlay() {
    const ov = $('overlay-matrix');
    for (let i = 0; i < 16; i++) {
      const col = i % 4, row = Math.floor(i / 4), x = 14 + col * 444, y = 14 + row * 180;
      const { c } = card(ov, x, y, 430, 168, `SLOT ${i + 1}`);
      chip(c, `mod${i + 1}.src`, 14, 70, 170, 'SOURCE', { cols: 5 });
      chip(c, `mod${i + 1}.dst`, 14, 124, 170, 'DESTINATION', { cols: 5 });
      knob(c, `mod${i + 1}.amt`, 300, 102, { size: 56, noSlots: true, dim: () => patch.mod[i].src === 0 || patch.mod[i].dst === 0 });
      const clr = place(el('div', 'btn dark small', c, 'CLEAR'), 352, 50);
      clr.style.position = 'absolute';
      clr.addEventListener('pointerdown', (e) => { e.preventDefault(); M.removeRoute(patch, i); touched(); });
    }
  }
  let tabMx, tabFx;
  function toggleOverlay(which) {
    const mx = $('overlay-matrix'), fx = $('overlay-fx');
    const openMx = which === 'matrix' && mx.classList.contains('hidden');
    const openFx = which === 'fx' && fx.classList.contains('hidden');
    mx.classList.toggle('hidden', !openMx); fx.classList.toggle('hidden', !openFx);
    for (const [t, open, name] of [[tabMx, openMx, 'MATRIX'], [tabFx, openFx, 'EFFECTS']]) {
      t.classList.toggle('sel', open); t.classList.toggle('blue', !open); t.textContent = open ? 'PANEL' : name;
    }
    refresh(true);
  }

  // --------------------------------------------------------------- the strip ---
  let midiLed, vLeds = [], infoEl, ledNum, ledName, ledFlag;
  function buildStrip() {
    const s = $('strip');
    const st = el('div', 'status', s); st.style.width = '200px';
    let r = el('div', 'row', st); midiLed = el('span', 'led', r); el('span', null, r, 'MIDI');
    const vm = el('span', 'vmeter', r); vm.style.marginLeft = '12px';
    for (let i = 0; i < 8; i++) vLeds.push(el('span', 'led', vm));
    r = el('div', 'row', st); infoEl = el('span', null, r, Bridge.isMock ? 'DESKTOP PREVIEW' : '');
    const btn = (label, cls, fn) => { const b = el('div', `btn ${cls || ''}`, s, label); b.addEventListener('pointerdown', (e) => { e.preventDefault(); fn(b); }); return b; };
    btn('◀', 'dark arrow', () => step(-1));
    const disp = el('div', null, s); disp.id = 'led-display';
    ledNum = el('span', null, disp); ledNum.id = 'led-num';
    ledName = el('span', null, disp); ledName.id = 'led-name';
    ledFlag = el('span', null, disp); ledFlag.id = 'led-flag';
    disp.addEventListener('pointerdown', (e) => { e.preventDefault(); openBrowser(); });
    btn('▶', 'dark arrow', () => step(1));
    btn('SAVE', '', () => openSave());
    btn('IMPORT .NMSV', '', () => { toast('Choose a preset file (.nmsv)'); Bridge.pickNmsv(); });
    btn('INIT', '', () => { patch = Core.defaultPatch(); current = -1; dirty = false; sendNow(); refresh(true); });
    el('div', 'spacer', s);
    tabMx = btn('MATRIX', 'blue', () => toggleOverlay('matrix'));
    tabFx = btn('EFFECTS', 'blue', () => toggleOverlay('fx'));
  }
  function updateDisplay() {
    if (!ledName) return;
    const e = entries[current];
    ledNum.textContent = current < 0 || !e ? '--' : `${e.kind}${String(entries.filter((x) => x.kind === e.kind).indexOf(e) + 1).padStart(2, '0')}`;
    ledName.textContent = (patch.name || 'UNTITLED').toUpperCase();
    ledFlag.textContent = dirty ? 'EDITED' : '';
  }

  // ------------------------------------------------------------------ presets ---
  function loadEntries() {
    entries = FACTORY.map((p) => ({ kind: 'F', name: p.name, patch: p }));
    for (const u of Bridge.listPresets()) if (u && typeof u.name === 'string' && typeof u.b64 === 'string') entries.push({ kind: 'U', name: u.name, b64: u.b64 });
  }
  function patchOf(e) { return e.patch ? M.clone(e.patch) : Core.mfpRead(Core.b64decode(e.b64)); }
  function loadEntry(i) {
    i = ((i % entries.length) + entries.length) % entries.length;
    try { patch = patchOf(entries[i]); } catch (err) { toast(`Cannot open ${entries[i].name}: ${err.message}`); return; }
    current = i; dirty = false; sendNow(); refresh(true);
  }
  function step(d) { loadEntry(current < 0 ? 0 : current + d); }

  const dialog = $('dialog');
  function closeDialog() { dialog.classList.add('hidden'); dialog.replaceChildren(); }
  function dlgButtons(box, list) {
    const row = el('div', 'row', box);
    for (const [label, cls, fn] of list) { const b = el('div', `btn ${cls}`, row, label); b.addEventListener('pointerup', (e) => { e.preventDefault(); fn(); }); }
  }
  function openSave() {
    dialog.replaceChildren(); dialog.classList.remove('hidden');
    const box = el('div', 'dlg', dialog);
    el('h2', null, box, 'SAVE PRESET');
    const inp = el('input', null, box); inp.maxLength = 23; inp.value = patch.name || ''; inp.spellcheck = false;
    inp.setAttribute('autocomplete', 'off'); inp.setAttribute('autocapitalize', 'characters');
    el('p', null, box, 'Saving with the name of one of your presets replaces it.');
    const save = () => {
      const name = M.setName(patch, inp.value.trim() || 'UNTITLED');
      const res = Bridge.savePreset(name, Core.b64encode(Core.mfpWrite(patch)));
      if (res !== undefined && res !== 'ok') { toast(`Not saved: ${res}`); return; }
      loadEntries();
      current = entries.findIndex((e) => e.kind === 'U' && e.name === name);
      dirty = false; closeDialog(); refresh(false); toast(`Saved ${name}`);
    };
    inp.addEventListener('keydown', (e) => { if (e.key === 'Enter') save(); });
    dlgButtons(box, [['CANCEL', 'dark', closeDialog], ['SAVE', 'sel', save]]);
    setTimeout(() => { inp.focus(); inp.select(); }, 50);
  }
  dialog.addEventListener('pointerdown', (e) => { if (e.target === dialog) closeDialog(); });

  const browser = $('browser');
  function openBrowser() {
    browser.replaceChildren(); browser.classList.remove('hidden');
    const box = el('div', 'brw', browser);
    const h = el('h2', null, box, 'PRESETS'); el('span', 'spacer', h);
    const close = el('div', 'btn dark small', h, 'CLOSE'); close.addEventListener('pointerup', () => browser.classList.add('hidden'));
    const sc = el('div', 'scroll', box);
    for (const kind of ['F', 'U']) {
      const list = entries.map((e, i) => [e, i]).filter(([e]) => e.kind === kind);
      el('h3', null, sc, kind === 'F' ? 'FACTORY' : (list.length ? 'YOUR PRESETS' : 'YOUR PRESETS (NONE YET: SAVE ONE)'));
      const g = el('div', 'grid', sc);
      list.forEach(([e, i], k) => {
        const b = el('div', i === current ? 'pre on' : 'pre', g);
        el('span', 'n', b, `${kind}${String(k + 1).padStart(2, '0')}`);
        el('span', null, b, e.name.toUpperCase());
        let sx = 0, sy = 0;
        b.addEventListener('pointerdown', (ev) => { sx = ev.clientX; sy = ev.clientY; });
        b.addEventListener('pointerup', (ev) => {
          if (Math.abs(ev.clientX - sx) + Math.abs(ev.clientY - sy) > 12 * SCALE) return;
          if (ev.target.classList.contains('del')) return;
          loadEntry(i); browser.classList.add('hidden');
        });
        if (kind === 'U') {
          const del = el('span', 'del', b, 'DELETE');
          del.addEventListener('pointerup', (ev) => {
            ev.stopPropagation();
            if (!del.classList.contains('sure')) { del.classList.add('sure'); del.textContent = 'SURE?'; setTimeout(() => { del.classList.remove('sure'); del.textContent = 'DELETE'; }, 2500); return; }
            const cur = entries[current];
            Bridge.deletePreset(e.name);
            loadEntries();
            current = i === current || !cur ? -1 : entries.findIndex((x) => x.kind === cur.kind && x.name === cur.name);
            openBrowser();
          });
        }
      });
    }
  }
  browser.addEventListener('pointerdown', (e) => { if (e.target === browser) browser.classList.add('hidden'); });

  // .nmsv import: Java hands the picked file's bytes back here
  window.onNmsvPicked = function (name, b64) {
    if (name === null || name === undefined) { if (b64) toast(String(b64)); return; }
    let r;
    try { r = Core.convertBytes(Core.b64decode(String(b64)), String(name), PROFILES); } catch (err) {
      dialog.replaceChildren(); dialog.classList.remove('hidden');
      const box = el('div', 'dlg', dialog); el('h2', null, box, 'IMPORT FAILED');
      el('p', null, box, `${name}: ${err.message || err}`);
      dlgButtons(box, [['OK', 'sel', closeDialog]]);
      return;
    }
    patch = r.patch;
    if (!patch.name || patch.name === 'INIT') M.setName(patch, String(name).replace(/\.[^.]*$/, ''));
    current = -1; dirty = true; sendNow(); refresh(true);
    const c = r.ir.compatibility, ns = c.supported.length, na = c.approximated.length, nu = c.unsupported.length, tot = ns + na + nu || 1;
    dialog.replaceChildren(); dialog.classList.remove('hidden');
    const box = el('div', 'dlg', dialog);
    el('h2', null, box, 'IMPORTED');
    el('p', null, box, `${String(name)}  →  ${patch.name}`);
    const bar = el('div', 'compat', box);
    for (const [n, col] of [[ns, '#8fb34a'], [na, '#ffb02e'], [nu, '#c8432b']]) { const sp = el('span', null, bar); sp.style.width = `${(100 * n / tot).toFixed(1)}%`; sp.style.background = col; }
    const k = el('div', 'compat-k', box);
    for (const [n, t, col] of [[ns, 'supported', '#8fb34a'], [na, 'approximated', '#ffb02e'], [nu, 'not supported', '#c8432b']]) {
      const s = el('span', null, k); const bb = el('b', null, s, String(n)); bb.style.color = col; s.appendChild(document.createTextNode(t));
    }
    if (c.missing_wavetables.length) el('p', null, box, `Wavetables replaced by the classic waveforms: ${c.missing_wavetables.length}`);
    const lst = el('div', 'compat-list', box);
    for (const line of c.unsupported.slice(0, 6)) el('div', null, lst, `· ${line}`);
    if (nu > 6) el('div', null, lst, `· and ${nu - 6} more`);
    dlgButtons(box, [['PLAY IT', 'dark', closeDialog], ['SAVE…', 'sel', () => { closeDialog(); openSave(); }]]);
  };

  // ------------------------------------------------------------- wheels/keys ---
  let octave = 0;
  function buildWheels() {
    const box = $('wheelbox');
    const oct = el('div', 'oct', box);
    el('div', 'octl', oct, 'OCTAVE');
    const dn = el('div', 'btn dark small', oct, '−'); const ov = el('div', 'octv', oct, '0'); const up = el('div', 'btn dark small', oct, '+');
    const setOct = (o) => { octave = clamp(o, -3, 3); ov.textContent = octave > 0 ? `+${octave}` : String(octave); allNotesOff(); labelKeys(); };
    dn.addEventListener('pointerdown', (e) => { e.preventDefault(); setOct(octave - 1); });
    up.addEventListener('pointerdown', (e) => { e.preventDefault(); setOct(octave + 1); });
    makeWheel(box, 26, 'PITCH', true, (v) => Bridge.pitchBend(Math.round(8192 + v * (v > 0 ? 8191 : 8192))));
    makeWheel(box, 128, 'MOD', false, (v) => Bridge.controlChange(1, Math.round(v * 127)));
  }
  function makeWheel(parent, x, label, spring, send) {
    const well = place(el('div', 'wheel-well', parent), x, 74);
    const w = el('div', 'wheel', well), mark = el('div', 'mark', w);
    place(el('div', 'wheel-lbl', parent, label), x - 11, 268);
    let v = 0, pid = null, lastY = 0, anim = 0, lastSent = null;
    const H = 188;
    const draw = () => {
      const pos = spring ? v : v * 2 - 1;
      w.style.backgroundPosition = `0 ${(-pos * 40).toFixed(1)}px`;
      mark.style.top = `${(H / 2 - 3 - pos * (H / 2 - 18)).toFixed(1)}px`;
      const q = Math.round(v * 1000); if (q !== lastSent) { lastSent = q; send(v); }
    };
    draw();
    well.addEventListener('pointerdown', (e) => { e.preventDefault(); cancelAnimationFrame(anim); pid = e.pointerId; lastY = e.clientY; try { well.setPointerCapture(pid); } catch (x) { /* */ } });
    well.addEventListener('pointermove', (e) => {
      if (e.pointerId !== pid) return;
      const dy = (lastY - e.clientY) / SCALE; lastY = e.clientY;
      v = clamp(v + dy / (spring ? 80 : 160), spring ? -1 : 0, 1); draw();
    });
    const end = (e) => {
      if (e.pointerId !== pid) return; pid = null;
      if (spring) { const back = () => { v *= 0.6; if (Math.abs(v) < 0.01) v = 0; draw(); if (v) anim = requestAnimationFrame(back); }; anim = requestAnimationFrame(back); }
    };
    well.addEventListener('pointerup', end); well.addEventListener('pointercancel', end);
  }

  const KEY_BASE = 48;       // C3 at octave 0
  const keyEls = [];
  const held = new Map();    // pointerId -> {i, note, vel}
  const noteCount = new Map();
  function buildKeys() {
    const k = $('keys');
    const KW = 1532, WW = KW / 22, BW = 42;
    const shift = { 1: -0.15, 3: 0.15, 6: -0.18, 8: 0, 10: 0.18 };   // black-key offsets as on a real keyboard
    let wi = 0;
    for (let i = 0; i <= 36; i++) {
      const pc = i % 12, black = [1, 3, 6, 8, 10].includes(pc);
      const e = el('div', black ? 'key b' : 'key w', k);
      e.dataset.i = i;
      if (black) place(e, wi * WW - BW / 2 + shift[pc] * BW, undefined, BW);
      else { place(e, wi * WW + 1, undefined, WW - 2); wi++; }
      if (pc === 0) e.cn = el('span', 'cnote', e);
      keyEls.push(e);
    }
    el('div', 'keys-felt', k);
    labelKeys();
    const keyAt = (cx, cy) => {
      const t = document.elementFromPoint(cx, cy), key = t && t.closest ? t.closest('.key') : null;
      if (!key) return null;
      const r = key.getBoundingClientRect(), frac = clamp((cy - r.top) / r.height, 0, 1);
      return { i: Number(key.dataset.i), vel: clamp(Math.round(28 + 99 * Math.pow(frac, 0.8)), 1, 127) };
    };
    k.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      try { k.setPointerCapture(e.pointerId); } catch (x) { /* */ }
      const h = keyAt(e.clientX, e.clientY); if (h) press(e.pointerId, h.i, h.vel);
    });
    k.addEventListener('pointermove', (e) => {
      if (!held.has(e.pointerId)) return;
      const h = keyAt(e.clientX, e.clientY), cur = held.get(e.pointerId);
      if (h && h.i !== cur.i) { release(e.pointerId); press(e.pointerId, h.i, cur.vel); }
    });
    const up = (e) => release(e.pointerId);
    k.addEventListener('pointerup', up); k.addEventListener('pointercancel', up); k.addEventListener('lostpointercapture', up);
  }
  function labelKeys() { keyEls.forEach((e, i) => { if (e.cn) e.cn.textContent = `C${Math.floor((KEY_BASE + octave * 12 + i) / 12) - 1}`; }); }
  function press(pid, i, vel) {
    const note = clamp(KEY_BASE + octave * 12 + i, 0, 127);
    held.set(pid, { i, note, vel });
    const n = (noteCount.get(note) || 0) + 1; noteCount.set(note, n);
    keyEls[i].classList.add('down');
    if (n === 1) Bridge.noteOn(note, vel);
  }
  function release(pid) {
    const h = held.get(pid); if (!h) return;
    held.delete(pid);
    const n = (noteCount.get(h.note) || 1) - 1;
    if (n <= 0) { noteCount.delete(h.note); Bridge.noteOff(h.note); } else noteCount.set(h.note, n);
    if (![...held.values()].some((x) => x.i === h.i)) keyEls[h.i].classList.remove('down');
  }
  function allNotesOff() { for (const pid of [...held.keys()]) release(pid); }

  // ------------------------------------------------------------ status poll ---
  let lastMidi = -1, midiFlash = 0;
  function poll() {
    const s = Bridge.getStatus();
    if (!s) return;
    const voices = clamp(s.voices | 0, 0, 8);
    vLeds.forEach((l, i) => l.classList.toggle('on', i < voices));
    if (typeof s.midi === 'number') { if (lastMidi >= 0 && s.midi !== lastMidi) midiFlash = Date.now(); lastMidi = s.midi; }
    midiLed.classList.toggle('on', Date.now() - midiFlash < 150);
    const devs = Array.isArray(s.midiDevices) ? s.midiDevices.filter((x) => typeof x === 'string') : [];
    const parts = [];
    if (s.sampleRate) parts.push(`${(s.sampleRate / 1000).toFixed(1)} kHz`);
    if (s.latencyMs) parts.push(`${Math.round(s.latencyMs)} ms`);
    parts.push(devs.length ? devs[0].slice(0, 14).toUpperCase() : 'NO MIDI IN');
    infoEl.textContent = parts.join(' · ');
    if (lampEl) lampEl.style.setProperty('--lit', String(0.72 + 0.28 * Math.min(1, voices / 3)));
  }

  // ------------------------------------------- title hold: settings (3 s) ---
  function buildTitleHold() {
    const t = $('title-hold');
    let timer = 0, pid = null;
    const stop = () => { clearTimeout(timer); timer = 0; pid = null; t.classList.remove('holding'); };
    t.addEventListener('pointerdown', (e) => {
      e.preventDefault(); if (pid !== null) return;
      pid = e.pointerId; try { t.setPointerCapture(pid); } catch (x) { /* */ }
      void t.offsetWidth; t.classList.add('holding');
      timer = setTimeout(() => { stop(); Bridge.openSettings(); }, 3000);
    });
    for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) t.addEventListener(ev, (e) => { if (e.pointerId === pid) stop(); });
  }

  // ------------------------------------------------------ VST3 plugin host ---
  // The plugin keeps the sound in the DAW project and moves it by automation; the tablet never calls these.
  function hostState() { const e = entries[current]; return { kind: e ? e.kind : '', name: e ? e.name : '', dirty }; }
  function hostPatch(b64) {          // automation: new values for some fields, no echo to the engine
    let p;
    try { p = Core.mfpRead(Core.b64decode(String(b64))); } catch (err) { return false; }
    patch = p; refresh(false);
    return true;
  }
  function hostRestore(st) {         // project reopened / program change: patch + which preset it is
    if (!st || typeof st.b64 !== 'string') return false;
    let p;
    try { p = Core.mfpRead(Core.b64decode(st.b64)); } catch (err) { return false; }
    const u = st.ui && typeof st.ui === 'object' ? st.ui : {};
    patch = p;
    current = entries.findIndex((e) => e.kind === u.kind && e.name === u.name);
    dirty = !!u.dirty;
    refresh(true);
    return true;
  }

  // --------------------------------------------------------------------- boot ---
  function boot() {
    fit();
    $('panel').style.backgroundImage = `url(${noiseTexture(256, [20, 19, 18], 9, 7)})`;
    const otex = noiseTexture(256, [17, 16, 15], 8, 11);
    for (const o of document.querySelectorAll('.overlay')) o.style.backgroundImage = `url(${otex})`;
    document.querySelectorAll('canvas.wood').forEach((c, i) => drawWood(c, 1234 + i * 777));
    svgDefs();
    buildPanel(); buildFxOverlay(); buildMatrixOverlay(); buildStrip(); buildWheels(); buildKeys(); buildTitleHold();
    loadEntries();
    // the VST3 plugin (plugin/web/desktop_shim.js) starts with the sound the host restored, not F01
    const hostInit = window.MFHost && typeof window.MFHost.initialState === 'function' ? window.MFHost.initialState() : null;
    if (!(hostInit && hostRestore(hostInit))) loadEntry(0);
    setInterval(poll, 100);
    // hooks for screenshots and desktop checks; hostState/hostPatch/hostRestore are the plugin's (tablet: unused)
    window.MFUI = { toggleOverlay, loadEntry, selectTab, get patch() { return patch; }, entries: () => entries, refresh,
      toast, hostState, hostPatch, hostRestore };
    const q = location.hash.slice(1).split(',');
    for (const a of q) {
      if (a === 'matrix' || a === 'fx') toggleOverlay(a);
      else if (/^p\d+$/.test(a)) loadEntry(Number(a.slice(1)));
      else if (/^t\d$/.test(a)) selectTab(Number(a.slice(1)));
    }
  }
  boot();
})();
