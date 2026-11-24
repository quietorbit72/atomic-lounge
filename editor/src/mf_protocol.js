// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk editor: the SysEx "MF" protocol v2 (docs/FIRMWARE_ARCHITECTURE.md), host side.
// No DOM use; node loads it with require(), the page inlines it (global MFProtocol).
//
// SAFETY: this module can only build frames F0 7D 4D 46 <cmd> <7-bit args> F7 for the commands
// HELLO, GET_EDIT, PUT_EDIT, STORE, LOAD and DUMP. Nothing else (no firmware update, no vendor
// SysEx, no SET_PARAM) can be produced: buildFrame() refuses any other command or any byte >= 0x80,
// and MFClient checks every frame again before it reaches the port.
'use strict';
(function (root, factory) {
  const m = factory();
  if (typeof module === 'object' && module.exports) module.exports = m;
  else root.MFProtocol = m;
})(typeof self !== 'undefined' ? self : this, function () {
  const HEADER = [0xF0, 0x7D, 0x4D, 0x46];     // F0, non-commercial id 7D, 'M', 'F'
  const CMD = { HELLO: 1, GET_EDIT: 2, PUT_EDIT: 3, STORE: 4, LOAD: 5, DUMP: 6 };
  const ALLOWED = new Set(Object.values(CMD));
  const CMD_NAME = Object.fromEntries(Object.entries(CMD).map(([k, v]) => [v, k]));
  const STATUS = ['ok', 'bad frame', 'bad preset (CRC/version)', 'bad slot', 'Flash error', 'out of range'];
  const CHUNK_RAW = 98, EDIT_SLOT = 0x7F, SLOTS = 64, MAX_IN_FRAME = 1024;

  function pack7(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i += 7) {
      const g = Math.min(7, bytes.length - i);
      let hi = 0;
      for (let k = 0; k < g; k++) hi |= ((bytes[i + k] >> 7) & 1) << k;
      out.push(hi);
      for (let k = 0; k < g; k++) out.push(bytes[i + k] & 127);
    }
    return Uint8Array.from(out);
  }
  function unpack7(data) {
    const out = [];
    for (let i = 0; i < data.length;) {
      const hi = data[i++];
      for (let k = 0; k < 7 && i < data.length; k++, i++) out.push((data[i] | (((hi >> k) & 1) << 7)) & 255);
    }
    return Uint8Array.from(out);
  }

  function buildFrame(cmd, args = []) {
    if (!ALLOWED.has(cmd)) throw new Error(`refused: command ${cmd} is not an MF editor command`);
    const a = Array.from(args);
    for (const b of a) if (!Number.isInteger(b) || b < 0 || b > 127) throw new Error('refused: SysEx data byte out of range');
    const f = Uint8Array.from([...HEADER, cmd, ...a, 0xF7]);
    if (f.length > MAX_IN_FRAME) throw new Error(`refused: frame of ${f.length} bytes exceeds the FM1's ${MAX_IN_FRAME}`);
    return f;
  }
  function isSafeFrame(f) {
    if (!f || f.length < 6 || f[f.length - 1] !== 0xF7) return false;
    for (let i = 0; i < 4; i++) if (f[i] !== HEADER[i]) return false;
    if (!ALLOWED.has(f[4])) return false;
    for (let i = 5; i < f.length - 1; i++) if (f[i] > 127) return false;
    return f.length <= MAX_IN_FRAME;
  }
  function checkSlot(slot) {
    if (!Number.isInteger(slot) || slot < 0 || slot >= SLOTS) throw new Error(`slot ${slot} out of range 0..${SLOTS - 1}`);
    return slot;
  }
  const frames = {
    hello: () => buildFrame(CMD.HELLO),
    getEdit: () => buildFrame(CMD.GET_EDIT),
    putEdit: (mfp) => buildFrame(CMD.PUT_EDIT, pack7(mfp)),
    store: (slot) => buildFrame(CMD.STORE, [checkSlot(slot)]),
    load: (slot) => buildFrame(CMD.LOAD, [checkSlot(slot)]),
    dump: (slot) => buildFrame(CMD.DUMP, [checkSlot(slot)]),
  };

  /** Parse an incoming message. Returns null for anything that is not an MF frame. */
  function parseFrame(m) {
    if (!m || m.length < 6 || m[m.length - 1] !== 0xF7) return null;
    for (let i = 0; i < 4; i++) if (m[i] !== HEADER[i]) return null;
    return { cmd: m[4], args: m.subarray ? m.subarray(5, m.length - 1) : Uint8Array.from(m.slice(5, m.length - 1)) };
  }
  function parseHello(args) {
    let s = '';
    for (let i = 6; i < args.length && args[i] !== 0; i++) s += String.fromCharCode(args[i]);
    return { protocol: args[0], presetVersion: args[1], presetSize: args[2] | (args[3] << 7), slots: args[4], currentSlot: args[5], firmware: s };
  }

  /** Reassembles one chunked GET_EDIT / DUMP reply (v2). feed() returns the raw bytes when complete. */
  class Reassembler {
    constructor(check) { this.check = check; this.reset(); }
    reset() { this.cur = null; }
    feed(args) {
      if (args.length < 5) throw new Error('short chunk frame');
      const slot = args[0], index = args[1], count = args[2], len = args[3] | (args[4] << 7);
      if (count < 1 || index >= count || count !== Math.ceil(len / CHUNK_RAW)) throw new Error('inconsistent chunk header');
      const c = this.cur;
      if (!c || c.slot !== slot || c.count !== count || c.len !== len) this.cur = { slot, count, len, parts: new Array(count) };
      const raw = unpack7(args.subarray ? args.subarray(5) : args.slice(5));
      const want = Math.min(CHUNK_RAW, len - CHUNK_RAW * index);
      if (raw.length !== want) throw new Error(`chunk ${index}: ${raw.length} bytes, expected ${want}`);
      this.cur.parts[index] = raw;
      for (let i = 0; i < count; i++) if (!this.cur.parts[i]) return null;
      const out = new Uint8Array(len);
      this.cur.parts.forEach((p, i) => out.set(p, i * CHUNK_RAW));
      const done = { slot, bytes: out };
      this.reset();
      if (this.check) { const why = this.check(out); if (why) throw new Error(`reassembled preset rejected: ${why}`); }
      return done;
    }
  }

  /**
   * Request/reply client over a port: { send(Uint8Array) } plus client.receive(bytes) for input.
   * One request at a time; replies matched by command byte.
   */
  class MFClient {
    constructor(port, opts = {}) {
      this.port = port;
      this.check = opts.check || null;          // e.g. MFCore.mfpCheck
      this.log = opts.log || (() => {});
      this.timeoutMs = opts.timeoutMs || 3000;
      this.pending = null;
      this.chain = Promise.resolve();
      this.re = new Reassembler(this.check);
    }
    _send(frame) {
      if (!isSafeFrame(frame)) throw new Error('refused to send a non-MF frame');
      if (!this.port) throw new Error('no MIDI output chosen');
      this.log('out', frame);
      this.port.send(frame);
    }
    receive(bytes) {
      const m = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes);
      const f = parseFrame(m);
      if (!f) return;
      this.log('in', m);
      const p = this.pending;
      if (!p || f.cmd !== p.cmd) return;
      try {
        if (p.kind === 'hello') { if (f.args.length >= 6) this._done(null, parseHello(f.args)); return; }
        if (p.kind === 'status' || f.args.length === 1) {
          const st = f.args[0];
          if (st === 0 && p.kind === 'status') this._done(null, { status: 0 });
          else this._done(new Error(`FM1 replied ${CMD_NAME[f.cmd]}: ${STATUS[st] || 'status ' + st}`));
          return;
        }
        const r = this.re.feed(f.args);
        if (r) {
          if (r.slot !== p.slot) throw new Error(`reply for slot ${r.slot}, asked ${p.slot}`);
          this._done(null, r);
        }
      } catch (e) { this._done(e); }
    }
    _done(err, val) {
      const p = this.pending; if (!p) return;
      this.pending = null; clearTimeout(p.timer);
      if (err) p.reject(err); else p.resolve(val);
    }
    _request(frame, kind, slot, timeoutMs) {
      const run = () => new Promise((resolve, reject) => {
        this.re.reset();
        const p = { cmd: frame[4], kind, slot, resolve, reject };
        p.timer = setTimeout(() => { if (this.pending === p) { this.pending = null; reject(new Error(`no reply to ${CMD_NAME[p.cmd]} (timeout)`)); } }, timeoutMs || this.timeoutMs);
        this.pending = p;
        try { this._send(frame); } catch (e) { this.pending = null; clearTimeout(p.timer); reject(e); }
      });
      const r = this.chain.then(run, run);
      this.chain = r.catch(() => {});
      return r;
    }
    hello() { return this._request(frames.hello(), 'hello'); }
    putEdit(mfp) {
      if (this.check) { const why = this.check(mfp); if (why) return Promise.reject(new Error(`not sending: ${why}`)); }
      return this._request(frames.putEdit(mfp), 'status');
    }
    store(slot) { return this._request(frames.store(slot), 'status', undefined, 10000); }
    load(slot) { return this._request(frames.load(slot), 'status'); }
    dump(slot) { return this._request(frames.dump(slot), 'preset', slot, 5000).then((r) => r.bytes); }
    getEdit() { return this._request(frames.getEdit(), 'preset', EDIT_SLOT, 5000).then((r) => r.bytes); }
  }

  return { HEADER, CMD, CMD_NAME, STATUS, CHUNK_RAW, EDIT_SLOT, SLOTS, pack7, unpack7, buildFrame, isSafeFrame, frames,
    parseFrame, parseHello, Reassembler, MFClient };
});
