// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk editor: the converter core, a faithful JavaScript port of
//   tools/preset_decoder/{fastlz.py, nis.py, massive.py} and
//   tools/preset_converter/{convert.py, massive_rules.py, mfp.py}.
// No DOM use: node loads this file with require(); the page inlines it (global MFCore).
// The .mfp bytes must equal the Python converter's (tests/test_editor.mjs checks this).
// Python semantics kept on purpose: round() is round-half-even, min()/max() keep the first
// argument on ties and NaN, int() truncates, round(x, 5) is decimal-exact.
'use strict';
(function (root, factory) {
  const m = factory();
  if (typeof module === 'object' && module.exports) module.exports = m;
  else root.MFCore = m;
})(typeof self !== 'undefined' ? self : this, function () {

  // ------------------------------------------------------------- Python helpers ---
  class PyError extends Error {
    constructor(kind, msg) { super(msg); this.kind = kind; }
  }
  function pyRound(x) {
    if (!Number.isFinite(x)) throw new PyError('ValueError', 'cannot convert float NaN/inf to integer');
    const f = Math.floor(x), d = x - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return (f % 2 === 0) ? f : f + 1;
  }
  function pyRoundN(x, n) { return parseFloat(x.toFixed(n)); }   // decimal-exact like Python round(x, n)
  function pmin(a, b) { return b < a ? b : a; }
  function pmax(a, b) { return b > a ? b : a; }
  function pyInt(x) {
    if (typeof x === 'boolean') return x ? 1 : 0;
    if (typeof x === 'string') {
      if (!/^\s*[+-]?\d+\s*$/.test(x)) throw new PyError('ValueError', `invalid literal for int(): '${x}'`);
      return parseInt(x, 10);
    }
    if (!Number.isFinite(x)) throw new PyError('ValueError', 'cannot convert float to integer');
    return Math.trunc(x) + 0;
  }
  function num(x) { return typeof x === 'boolean' ? (x ? 1 : 0) : x; }
  function fmtF(x, n, plus) {           // f"{x:.nf}" / f"{x:+.nf}"
    if (Number.isNaN(x)) return (plus ? '+' : '') + 'nan';
    const neg = x < 0 || Object.is(x, -0);
    let s = Math.abs(x).toFixed(n);
    // exact ties (e.g. 0.625 to 2 places): toFixed rounds up, Python rounds half to even
    if (Math.abs(x) < 1e21) {
      const ex = Math.abs(x).toFixed(100), dot = ex.indexOf('.');
      if (/^50*$/.test(ex.slice(dot + 1 + n))) {
        const kept = n ? ex.slice(0, dot + 1 + n) : ex.slice(0, dot);
        if (parseInt(kept[kept.length - 1], 10) % 2 === 0) s = kept;
      }
    }
    return (neg ? '-' : (plus ? '+' : '')) + s;
  }
  function fmtG(x, p) {                 // f"{x:.pg}"
    if (Number.isNaN(x)) return 'nan';
    if (!Number.isFinite(x)) return x > 0 ? 'inf' : '-inf';
    if (x === 0) return Object.is(x, -0) ? '-0' : '0';
    const e = x.toExponential(p - 1);
    const exp = parseInt(e.split('e')[1], 10);
    const strip = (s) => (s.indexOf('.') >= 0 ? s.replace(/0+$/, '').replace(/\.$/, '') : s);
    if (exp >= -4 && exp < p) return strip(x.toFixed(p - 1 - exp));
    const [m] = e.split('e');
    return strip(m) + 'e' + (exp < 0 ? '-' : '+') + String(Math.abs(exp)).padStart(2, '0');
  }
  function pyRepr(v) {
    if (v === null || v === undefined) return 'None';
    if (v === true) return 'True';
    if (v === false) return 'False';
    if (typeof v === 'number') {
      if (Number.isInteger(v) && Math.abs(v) < 1e16) return String(v);
      let s = String(v);
      if (/e/.test(s)) s = s.replace(/e([+-])(\d)$/, 'e$10$2');
      return s;
    }
    if (typeof v === 'string') return "'" + v.replace(/\\/g, '\\\\').replace(/'/g, "\\'") + "'";
    if (Array.isArray(v)) return '[' + v.map(pyRepr).join(', ') + ']';
    return '{' + Object.keys(v).map((k) => pyRepr(k) + ': ' + pyRepr(v[k])).join(', ') + '}';
  }
  function deepCopy(o) { return JSON.parse(JSON.stringify(o)); }

  // -------------------------------------------------------------- bytes helpers ---
  const te = (s) => { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 255; return a; };
  function latin1(b) { let s = ''; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); return s; }
  function findBytes(hay, needle, start) {
    const n = needle.length;
    outer: for (let i = start || 0; i + n <= hay.length; i++) {
      for (let k = 0; k < n; k++) if (hay[i + k] !== needle[k]) continue outer;
      return i;
    }
    return -1;
  }
  function concat(parts) {
    let n = 0; for (const p of parts) n += p.length;
    const out = new Uint8Array(n); let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  }
  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  function b64encode(b) {
    let s = '';
    for (let i = 0; i < b.length; i += 3) {
      const n = (b[i] << 16) | ((b[i + 1] || 0) << 8) | (b[i + 2] || 0);
      s += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (i + 1 < b.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < b.length ? B64[n & 63] : '=');
    }
    return s;
  }
  function b64decode(s) {
    const clean = s.replace(/[^A-Za-z0-9+/]/g, '');
    const out = new Uint8Array(Math.floor(clean.length * 3 / 4));
    let o = 0, acc = 0, bits = 0;
    for (const c of clean) {
      acc = (acc << 6) | B64.indexOf(c); bits += 6;
      if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 255; }
    }
    return out.subarray(0, o);
  }
  function hex(b) { let s = ''; for (const x of b) s += x.toString(16).padStart(2, '0'); return s; }

  let CRC_TABLE = null;
  function crc32(b) {
    if (!CRC_TABLE) {
      CRC_TABLE = new Uint32Array(256);
      for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; CRC_TABLE[n] = c >>> 0; }
    }
    let c = 0xFFFFFFFF;
    for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  function sha256hex(msg) {
    const K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2];
    const H = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    const l = msg.length, nblk = ((l + 9 + 63) >> 6);
    const buf = new Uint8Array(nblk * 64); buf.set(msg); buf[l] = 0x80;
    const dv = new DataView(buf.buffer);
    dv.setUint32(buf.length - 8, Math.floor(l / 0x20000000)); dv.setUint32(buf.length - 4, (l * 8) >>> 0);
    const W = new Uint32Array(64);
    const r = (x, n) => (x >>> n) | (x << (32 - n));
    for (let b = 0; b < nblk; b++) {
      for (let t = 0; t < 16; t++) W[t] = dv.getUint32(b * 64 + t * 4);
      for (let t = 16; t < 64; t++) {
        const s0 = r(W[t - 15], 7) ^ r(W[t - 15], 18) ^ (W[t - 15] >>> 3);
        const s1 = r(W[t - 2], 17) ^ r(W[t - 2], 19) ^ (W[t - 2] >>> 10);
        W[t] = (W[t - 16] + s0 + W[t - 7] + s1) >>> 0;
      }
      let [a, bb, c, d, e, f, g, h] = H;
      for (let t = 0; t < 64; t++) {
        const t1 = (h + (r(e, 6) ^ r(e, 11) ^ r(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + W[t]) >>> 0;
        const t2 = ((r(a, 2) ^ r(a, 13) ^ r(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c))) >>> 0;
        h = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = bb; bb = a; a = (t1 + t2) >>> 0;
      }
      H[0] = (H[0] + a) >>> 0; H[1] = (H[1] + bb) >>> 0; H[2] = (H[2] + c) >>> 0; H[3] = (H[3] + d) >>> 0;
      H[4] = (H[4] + e) >>> 0; H[5] = (H[5] + f) >>> 0; H[6] = (H[6] + g) >>> 0; H[7] = (H[7] + h) >>> 0;
    }
    return H.map((x) => x.toString(16).padStart(8, '0')).join('');
  }

  // --------------------------------------------------------------------- FastLZ ---
  class FastLZError extends Error {}
  const MAX_OUTPUT = 64 * 1024 * 1024;
  function fastlzDecompress(data, expectedSize) {
    if (!data.length) return new Uint8Array(0);
    const level = (data[0] >> 5) + 1;
    if (level !== 1 && level !== 2) throw new FastLZError(`unknown FastLZ level ${level}`);
    const limit = expectedSize !== undefined && expectedSize !== null ? expectedSize : MAX_OUTPUT;
    if (limit > MAX_OUTPUT) throw new FastLZError('declared output too large');
    let out = new Uint8Array(Math.min(limit, 1 << 16) || 16), olen = 0;
    const grow = (need) => { if (need > out.length) { let n = out.length * 2; while (n < need) n *= 2; const o2 = new Uint8Array(n); o2.set(out.subarray(0, olen)); out = o2; } };
    const byte = (i) => { if (i >= data.length) throw new FastLZError('truncated FastLZ stream'); return data[i]; };
    let ip = 0; const n = data.length;
    let ctrl = data[ip] & 31; ip += 1;
    for (;;) {
      if (ctrl >= 32) {
        let length = (ctrl >> 5) - 1;
        let ofs = (ctrl & 31) << 8;
        let ref;
        if (level === 1) {
          if (length === 6) { length += byte(ip); ip += 1; }
          const code = byte(ip); ip += 1;
          ref = olen - ofs - 1 - code;
        } else {
          if (length === 6) { for (;;) { const c = byte(ip); ip += 1; length += c; if (c !== 255) break; } }
          const code = byte(ip); ip += 1;
          ref = olen - ofs - 1 - code;
          if (code === 255 && ofs === (31 << 8)) {
            ofs = (byte(ip) << 8) | byte(ip + 1); ip += 2;
            ref = olen - ofs - 8191 - 1;
          }
        }
        length += 3;
        if (ref < 0) throw new FastLZError('back reference before start of output');
        if (olen + length > limit) throw new FastLZError('output exceeds declared size');
        grow(olen + length);
        for (let k = 0; k < length; k++) out[olen++] = out[ref++];
      } else {
        ctrl += 1;
        if (ip + ctrl > n) throw new FastLZError('literal run past end of input');
        if (olen + ctrl > limit) throw new FastLZError('output exceeds declared size');
        grow(olen + ctrl);
        out.set(data.subarray(ip, ip + ctrl), olen); olen += ctrl; ip += ctrl;
      }
      if (ip >= n) break;
      ctrl = data[ip]; ip += 1;
    }
    if (expectedSize !== undefined && expectedSize !== null && olen !== expectedSize)
      throw new FastLZError(`decompressed ${olen} bytes, expected ${expectedSize}`);
    return out.slice(0, olen);
  }

  // ------------------------------------------------------------------ NI Sound ---
  class NISError extends Error {}
  const DOMAIN_NISD = 'DSIN';
  const ITEM_NAMES = {
    0x01: 'Item', 0x64: 'Bank', 0x65: 'Preset', 0x66: 'BankContainer', 0x67: 'PresetContainer',
    0x68: 'BinaryChunkItem', 0x6A: 'Authorization', 0x6C: 'SoundInfoItem', 0x6D: 'PresetChunkItem',
    0x6E: 'ExternalFileReference', 0x6F: 'Resources', 0x70: 'AudioSampleItem',
    0x71: 'InternalResourceReferenceItem', 0x72: 'PictureItem', 0x73: 'SubtreeItem', 0x74: 'EncryptionItem',
    0x75: 'AppSpecific', 0x76: 'RepositoryRoot', 0x78: 'AutomationParameters', 0x79: 'ControllerAssignments',
    0x7A: 'Module', 0x7B: 'ModuleBank',
  };
  const APP_IDS = { 6: 'Absynth', 7: 'Massive' };
  const MAX_DEPTH = 32;
  function need(buf, off, n) {
    if (off < 0 || off + n > buf.length) throw new NISError(`truncated at offset ${off} (need ${n} bytes, have ${buf.length - off})`);
  }
  function u32(buf, off) { need(buf, off, 4); return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0; }
  function u64(buf, off) { need(buf, off, 8); return u32(buf, off) + u32(buf, off + 4) * 4294967296; }
  function frameName(f) {
    if (f.domain === DOMAIN_NISD) return ITEM_NAMES[f.itemId] || `Unknown_0x${f.itemId.toString(16)}`;
    return `${f.domain.split('').reverse().join('')}_0x${f.itemId.toString(16)}`;
  }
  function uuidStr(b) {
    const h = (i, j) => { let s = ''; for (let k = j - 1; k >= i; k--) s += b[k].toString(16).padStart(2, '0'); return s; };
    return `${h(0, 4)}-${h(4, 6)}-${h(6, 8)}-${hex(b.subarray(8, 10))}-${hex(b.subarray(10, 16))}`;
  }
  class Item {
    constructor() { this.version = 0; this.flags = 0; this.uuid = null; this.frames = []; this.children = []; this.subtree = null; }
    get typeName() { return this.frames.length ? frameName(this.frames[0]) : '?'; }
    frame(id) { for (const f of this.frames) if (f.domain === DOMAIN_NISD && f.itemId === id) return f; return null; }
    *walk(depth = 0) {
      yield [depth, this];
      if (this.subtree) yield* this.subtree.walk(depth + 1);
      for (const c of this.children) yield* c.walk(depth + 1);
    }
  }
  function nisParse(buf, depth = 0) {
    if (depth > MAX_DEPTH) throw new NISError('nesting too deep');
    return parseItem(buf, 0, depth)[0];
  }
  function parseItem(buf, off, depth) {
    const length = u64(buf, off);
    if (length < 40 || off + length > buf.length) throw new NISError(`bad item length ${length} at ${off}`);
    need(buf, off, 40);
    const version = u32(buf, off + 8);
    if (latin1(buf.subarray(off + 12, off + 16)) !== 'hsin') throw new NISError(`missing 'hsin' magic at ${off + 12}`);
    const it = new Item();
    it.version = version;
    it.flags = u32(buf, off + 16);
    it.uuid = uuidStr(buf.subarray(off + 24, off + 40));
    const end = off + length;
    let p = parseFrames(buf, off + 40, end, it.frames, 0);
    const cver = u32(buf, p), count = u32(buf, p + 4);
    p += 8;
    if (cver !== 1) throw new NISError(`children list version ${cver} at ${p - 8}`);
    if (count > 10000) throw new NISError('implausible child count');
    for (let i = 0; i < count; i++) {
      p += 12;
      const [child, np] = parseItem(buf, p, depth + 1);
      it.children.push(child); p = np;
    }
    if (p !== end) throw new NISError(`item at ${off}: ${end - p} unread bytes`);
    const st = it.frame(0x73);
    if (st) it.subtree = nisParse(subtreePayload(st.data), depth + 1);
    return [it, end];
  }
  function parseFrames(buf, p, limit, out, depth) {
    if (depth > MAX_DEPTH) throw new NISError('frame stack too deep');
    const size = u64(buf, p);
    if (size < 20 || p + size > limit) throw new NISError(`bad frame size ${size} at ${p}`);
    const domain = latin1(buf.subarray(p + 8, p + 12));
    const itemId = u32(buf, p + 12), version = u32(buf, p + 16);
    const end = p + size;
    let q = p + 20;
    const fr = { domain, itemId, version, data: null };
    out.push(fr);
    if (!(domain === DOMAIN_NISD && itemId === 1)) q = parseFrames(buf, q, end, out, depth + 1);
    fr.data = buf.subarray(q, end);
    return end;
  }
  function subtreePayload(data) {
    if (data.length < 5) throw new NISError('SubtreeItem too short');
    const ver = u32(data, 0);
    if (ver !== 1) throw new NISError(`SubtreeItem version ${ver}`);
    if (data[4]) {
      const dsize = u32(data, 5), csize = u32(data, 9);
      need(data, 13, csize);
      try { return fastlzDecompress(data.subarray(13, 13 + csize), dsize); } catch (e) {
        if (e instanceof FastLZError) throw new NISError(`SubtreeItem: ${e.message}`);
        throw e;
      }
    }
    const size = u64(data, 5);
    need(data, 5, size);
    return data.slice(5, 5 + size);   // as nis.py (the size field is part of the slice there too)
  }
  const UTF16 = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-16le') : null;
  function readWstring(buf, off) {
    const n = u32(buf, off);
    if (n > (1 << 16)) throw new NISError('string too long');
    need(buf, off + 4, 2 * n);
    return [UTF16.decode(buf.subarray(off + 4, off + 4 + 2 * n)), off + 4 + 2 * n];
  }

  // ------------------------------------------------------------------ massive.py ---
  function firstFrame(root, id) {
    for (const [, it] of root.walk()) { const f = it.frame(id); if (f) return f; }
    return null;
  }
  function tree(item) {
    const node = { type: item.typeName, frames: item.frames.map((f) => ({ name: frameName(f), id: f.itemId, version: f.version, bytes: f.data.length })) };
    if (item.subtree) node.subtree = tree(item.subtree);
    if (item.children.length) node.children = item.children.map(tree);
    return node;
  }
  function presetFrame(data, warnings) {
    const md = {};
    try {
      const chk = (o, n) => { if (o + n > data.length) throw new NISError(`unpack_from requires a buffer of at least ${o + n} bytes`); };
      chk(0, 4); md.preset_item_version = u32(data, 0);
      chk(5, 4); md.app_id = u32(data, 5);
      md.app = APP_IDS[md.app_id] || 'unknown';
      md.app_version = readWstring(data, 13)[0];
    } catch (e) {
      if (!(e instanceof NISError)) throw e;
      warnings.push(`Preset item: ${e.message}`);
    }
    return md;
  }
  const PRINTABLE = /^[^\p{C}\p{Z}]*$/u;
  function soundInfo(data, warnings) {
    const md = {};
    let p;
    try { [md.name, p] = readWstring(data, 16); } catch (e) {
      if (!(e instanceof NISError)) throw e;
      warnings.push(`SoundInfoItem name: ${e.message}`); return md;
    }
    const strings = [];
    let q = p;
    const strict = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-16le', { fatal: true }) : null;
    while (q + 4 <= data.length) {
      const n = u32(data, q);
      if (n > 0 && n < 256 && q + 4 + 2 * n <= data.length) {
        try {
          const s = strict.decode(data.subarray(q + 4, q + 4 + 2 * n));
          if (PRINTABLE.test(s.replace(/ /g, 'x'))) { strings.push({ offset: q, text: s }); q += 4 + 2 * n; continue; }
        } catch (e) { /* UnicodeDecodeError */ }
      }
      q += 1;
    }
    md.sound_info_strings = strings;
    md.sound_info_raw_b64 = b64encode(data);
    return md;
  }
  function floatBlock(state) {
    const vals = [];
    const dv = new DataView(state.buffer, state.byteOffset, state.byteLength);
    let o = 4;
    while (o + 4 <= state.length) {
      const v = dv.getFloat32(o, true);
      if (!(v === 0 || (1e-6 < Math.abs(v) && Math.abs(v) <= 1.0001))) break;
      vals.push(v); o += 4;
    }
    return [vals, o];
  }
  function macroNames(state) {
    const names = [];
    for (let k = 1; k <= 8; k++) {
      let found = null, start = 0;
      const digit = 48 + k;
      for (;;) {
        const i = state.indexOf(digit, start);
        if (i < 4) { if (i < 0) break; start = i + 1; continue; }
        const n = u32(state, i - 4);
        if (n >= 1 && n <= 64 && i + n <= state.length) {
          const s = state.subarray(i, i + n);
          if (s.every((c) => c >= 32 && c < 127)) {
            found = { offset: i - 4, text: latin1(s) };
            if (!names.length || found.offset > names[names.length - 1].offset) break;
          }
        }
        start = i + 1;
      }
      if (found === null) break;
      names.push(found);
    }
    return names;
  }
  function decodeStateChunk(data, warnings) {
    if (data.length < 20) { warnings.push('PresetChunkItem too short'); return { raw_b64: b64encode(data) }; }
    const h = [u32(data, 0), u32(data, 4), u32(data, 8)];
    let size = u64(data, 12);
    if (20 + size > data.length) { warnings.push('state size larger than chunk; using what is there'); size = data.length - 20; }
    let state = data.subarray(20, 20 + size);
    const trailer = data.subarray(20 + size);
    let wrapper = null;
    if (state.length >= 20 && u32(state, 0) === 5 && latin1(state.subarray(12, 16)) === 'tsrp') {
      wrapper = { version: 5, size: u32(state, 4), tag: 'prst' };
      state = state.subarray(16);
    }
    const out = {
      chunk_header: h, size: state.length, version: state.length >= 4 ? u32(state, 0) : null,
      raw_b64: b64encode(state), chunk_trailer_hex: hex(trailer), wrapper,
    };
    if (out.version !== 23 && out.version !== 24) warnings.push(`state version ${out.version === null ? 'None' : out.version} (only 23 and 24 have been seen)`);
    const [floats, end] = floatBlock(state);
    out.float_block = { offset: 4, end, count: floats.length,
      boundary: 'heuristic (first value that is not 0 or a float in [1e-6, 1])', values: floats };
    out.macro_names = macroNames(state);
    out.tail_offset = end;
    return out;
  }
  function decodeBytes(buf, filename = '') {
    buf = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    const root = nisParse(buf);
    const warnings = [];
    const out = {
      schema: 'vintage-chonk/decoded-preset', schema_version: 1,
      source: { file: filename, size: buf.length, sha256: sha256hex(buf) },
      container: { root_version: root.version, tree: tree(root) },
      metadata: {}, state: null, warnings,
    };
    const preset = firstFrame(root, 0x65);
    if (preset) Object.assign(out.metadata, presetFrame(preset.data, warnings));
    else warnings.push('no Preset item: not a single-preset file');
    const info = firstFrame(root, 0x6C);
    if (info) Object.assign(out.metadata, soundInfo(info.data, warnings));
    const app = out.metadata.app_id;
    if (app !== undefined && app !== null && app !== 7) warnings.push(`authoring app id ${app} is not Massive (7)`);
    const chunk = firstFrame(root, 0x6D);
    if (!chunk) warnings.push('no PresetChunkItem: no synth state found');
    else out.state = decodeStateChunk(chunk.data, warnings);
    return out;
  }

  // ----------------------------------------------------------------------- mfp.py ---
  const MFP = {
    MAGIC: 'MFP1', VERSION: 5,
    WAVETABLES: ['BASIC', 'PWM', 'SYNC', 'FORMANT', 'ORGAN', 'SQUASH', 'MOOG'],
    FILTERS: ['OFF', 'LP2', 'LP4', 'BP', 'HP', 'NOTCH', 'LADDER', 'ACID'],
    LFO_WAVES: ['SINE', 'TRI', 'SAW_UP', 'SAW_DOWN', 'SQUARE', 'SH', 'PERFORMER', 'STEPPER'],
    LFO_SYNC: 4, VOICE_MODES: ['POLY', 'MONO', 'LEGATO'],
    ENS_MODES: ['OFF', 'I', 'II', 'I+II', 'VIBRATO'], REV_TYPES: ['TWIN TANK', 'SPRING'],
    SOURCES: ['NONE', 'ENV1', 'ENV2', 'ENV3', 'ENV4', 'LFO1', 'LFO2', 'LFO3', 'LFO4', 'VEL', 'KEY', 'MODWHEEL', 'BEND',
      'AFTERTOUCH', 'MACRO1', 'MACRO2', 'MACRO3', 'MACRO4', 'MACRO5', 'MACRO6', 'MACRO7', 'MACRO8', 'RANDOM', 'ONE', 'OSC3'],
    DESTINATIONS: ['NONE', 'PITCH', 'OSC1_PITCH', 'OSC2_PITCH', 'OSC3_PITCH', 'OSC1_POS', 'OSC2_POS', 'OSC3_POS',
      'OSC1_WARP', 'OSC2_WARP', 'OSC3_WARP', 'OSC1_AMP', 'OSC2_AMP', 'OSC3_AMP', 'NOISE_AMP', 'F1_CUTOFF', 'F2_CUTOFF',
      'F1_RES', 'F2_RES', 'SERPAR', 'DRIVE', 'AMP', 'PAN', 'LFO1_RATE', 'LFO2_RATE'],
    OSC_ON: 1, OSC_RESET: 2, OSC_LO: 4, LFO_KEYSYNC: 1, LFO_UNIPOLAR: 2, ENV_LOOP: 1,
    NOSC: 3, NFILT: 2, NENV: 4, NLFO: 4, NMOD: 16, NMACRO: 8,
  };
  MFP.BODY = '<24s4B10h' + 'BBbb5h'.repeat(3) + '3h' + 'BB4h'.repeat(2) + '3h' + '5hBB3h'.repeat(4)
    + 'BBhhBBBBh16B16b'.repeat(4) + 'BBh'.repeat(16) + '8hBB13hBBh' + 'bBBBBB4h';
  function fmtCodes(fmt) {
    const codes = []; const re = /(\d*)([sBbhHI])/g; let m;
    while ((m = re.exec(fmt.replace(/^</, '')))) {
      const n = m[1] === '' ? 1 : parseInt(m[1], 10);
      if (m[2] === 's') codes.push(['s', n]); else for (let i = 0; i < n; i++) codes.push([m[2], 1]);
    }
    return codes;
  }
  const SZ = { B: 1, b: 1, h: 2, H: 2, I: 4 };
  function calcsize(fmt) { return fmtCodes(fmt).reduce((a, [c, n]) => a + (c === 's' ? n : SZ[c]), 0); }
  MFP.SIZE = 8 + calcsize(MFP.BODY) + 4;
  const RANGES = { B: [0, 255], b: [-128, 127], h: [-32768, 32767], H: [0, 65535], I: [0, 4294967295] };
  function structPack(fmt, values) {
    const codes = fmtCodes(fmt);
    if (codes.length !== values.length) throw new PyError('struct.error', `pack expected ${codes.length} items for packing (got ${values.length})`);
    const out = new Uint8Array(calcsize(fmt)); const dv = new DataView(out.buffer); let o = 0;
    codes.forEach(([c, n], i) => {
      let v = values[i];
      if (c === 's') { const b = v.subarray(0, n); out.set(b, o); o += n; return; }
      v = num(v);
      if (!Number.isInteger(v)) throw new PyError('struct.error', 'required argument is not an integer');
      const [lo, hi] = RANGES[c];
      if (v < lo || v > hi) throw new PyError('struct.error', `'${c}' format requires ${lo} <= number <= ${hi}`);
      if (c === 'B') dv.setUint8(o, v); else if (c === 'b') dv.setInt8(o, v);
      else if (c === 'h') dv.setInt16(o, v, true); else if (c === 'H') dv.setUint16(o, v, true); else dv.setUint32(o, v, true);
      o += SZ[c];
    });
    return out;
  }
  function structUnpack(fmt, buf, off) {
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength); const vals = []; let o = off;
    for (const [c, n] of fmtCodes(fmt)) {
      if (c === 's') { vals.push(buf.slice(o, o + n)); o += n; continue; }
      vals.push(c === 'B' ? dv.getUint8(o) : c === 'b' ? dv.getInt8(o) : c === 'h' ? dv.getInt16(o, true) : c === 'H' ? dv.getUint16(o, true) : dv.getUint32(o, true));
      o += SZ[c];
    }
    return vals;
  }
  const FX_DEFAULTS = { fuzz_amount: 0, fuzz_octave: 32767, fuzz_tone: 16384, fuzz_level: 16384,
    tape_drive: 0, tape_tone: 16384, tape_level: 16384, tape_wow: 0, ens_mode: 0, ens_mix: 16384, ens_rate: 16384, ens_depth: 16384,
    bbd_mix: 0, bbd_time: 9830, bbd_feedback: 13107, bbd_mod: 6553, bbd_tone: 16384,
    oil_mix: 0, oil_time: 16384, oil_feedback: 22938, oil_warble: 4915, oil_smear: 3277, rev_type: 0, rev_hold: 0, rev_amount: 0, fx_sync: 0 };
  // tempo divisions in beats (dsp mfe_sync_beats); fx_sync / nearestDelayDiv as mfp.py
  const SYNC_DIVS = [[1, 8], [1, 6], [1, 4], [1, 3], [3, 8], [1, 2], [2, 3], [3, 4], [1, 1], [3, 2], [2, 1], [4, 1], [8, 1]];
  function fxSync(bbdDiv, oilDiv) { return ((bbdDiv == null ? 0 : bbdDiv + 1) << 4) | (oilDiv == null ? 0 : oilDiv + 1); }
  function nearestDelayDiv(num, den) {
    let best = 0, bn = null, bd = null;
    SYNC_DIVS.forEach(([a, b], i) => {
      const hi = Math.max(num * b, a * den), lo = Math.min(num * b, a * den);
      if (bn === null || hi * bd < bn * lo) { best = i; bn = hi; bd = lo; }
    });
    return best;
  }
  function defaultPatch() {
    const r = (n, f) => Array.from({ length: n }, (_, i) => f(i));
    return {
      name: 'INIT', voice_mode: 0, max_voices: 8, bend_range: 2, vel_sens: 64, glide: 0, master: 8192, macro: r(8, () => 0),
      osc: r(3, (i) => ({ table: 6, flags: i === 0 ? MFP.OSC_ON : 0, semis: 0, drift: 0, fine: 0, position: 14043, warp: 0, amp: 26000, route: 0 })),
      noise: { amp: 0, color: 0, route: 0 },
      filt: r(2, (i) => ({ type: i === 0 ? 1 : 0, cutoff: i === 0 ? 30000 : 0, res: 0, drive: 0, keytrack: 0 })),
      serpar: 0, f1_mix: 32767, f2_mix: 32767,
      env: r(4, () => ({ delay: 0, attack: 2000, decay: 16000, sustain: 32767, release: 12000, flags: 0, att_level: 32767, dec_level: -1, sust_time: 0 })),
      lfo: r(4, () => ({ wave: 0, flags: 0, rate: 16000, phase: 0, sync_div: 8, loop_start: 0, loop_end: 15, glide: 0, step_shape: r(16, () => 0), step_level: r(16, () => 127) })),
      mod: r(16, () => ({ src: 0, dst: 0, amount: 0 })),
      modosc: { semis: 0, rm_osc: 0, pm_osc: 0, pos_osc: 0, ffm_filter: 0, rm: 0, pm: 0, pos: 0, ffm: 0 },
      fx: Object.assign({}, FX_DEFAULTS),
    };
  }
  // mfe_fx_t (dsp/include/mfe.h) in file order; 'fx_sync' was the spare byte 'ens_reserved' (older files read 0 = free)
  const FX_ORDER = ['fuzz_amount', 'fuzz_octave', 'fuzz_tone', 'fuzz_level', 'tape_drive', 'tape_tone', 'tape_level', 'tape_wow',
    'ens_mode', 'fx_sync', 'ens_mix', 'ens_rate', 'ens_depth', 'bbd_mix', 'bbd_time', 'bbd_feedback', 'bbd_mod', 'bbd_tone',
    'oil_mix', 'oil_time', 'oil_feedback', 'oil_warble', 'oil_smear', 'rev_type', 'rev_hold', 'rev_amount'];
  function mfpWrite(p) {
    const nameB = new Uint8Array(24);
    const nm = Array.from(p.name).map((c) => (c.codePointAt(0) < 128 ? c.codePointAt(0) : 63)).slice(0, 23);
    nameB.set(nm);
    const v = [nameB, p.voice_mode, p.max_voices, p.bend_range, p.vel_sens, p.glide, p.master, ...p.macro];
    for (const o of p.osc) v.push(o.table, o.flags, o.semis, o.drift || 0, o.fine, o.position, o.warp, o.amp, o.route);
    v.push(p.noise.amp, p.noise.color, p.noise.route);
    for (const f of p.filt) v.push(f.type, 0, f.cutoff, f.res, f.drive, f.keytrack);
    v.push(p.serpar, p.f1_mix, p.f2_mix);
    for (const e of p.env) v.push(e.delay, e.attack, e.decay, e.sustain, e.release, e.flags, 0, e.att_level, e.dec_level, e.sust_time);
    for (const l of p.lfo) v.push(l.wave, l.flags, l.rate, l.phase, l.sync_div, l.loop_start, l.loop_end, 0, l.glide, ...l.step_shape, ...l.step_level);
    for (const m of p.mod) v.push(m.src, m.dst, m.amount);
    for (const k of FX_ORDER) v.push(k === 'fx_sync' ? (p.fx.fx_sync || 0) : p.fx[k]);
    const mo = p.modosc;
    v.push(mo.semis, mo.rm_osc, mo.pm_osc, mo.pos_osc, mo.ffm_filter, 0, mo.rm, mo.pm, mo.pos, mo.ffm);
    const body = structPack(MFP.BODY, v);
    const head = new Uint8Array(8); head.set(te(MFP.MAGIC));
    new DataView(head.buffer).setUint16(4, MFP.VERSION, true); new DataView(head.buffer).setUint16(6, MFP.SIZE - 12, true);
    const data = concat([head, body]);
    const crc = new Uint8Array(4); new DataView(crc.buffer).setUint32(0, crc32(data), true);
    return concat([data, crc]);
  }
  function mfpCheck(buf) {   // null if valid, else the reason (as mfp.read)
    if (buf.length < MFP.SIZE) return 'short preset';
    if (latin1(buf.subarray(0, 4)) !== MFP.MAGIC) return 'bad magic';
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    if (dv.getUint16(4, true) !== MFP.VERSION || dv.getUint16(6, true) !== MFP.SIZE - 12) return 'unsupported preset version';
    if (crc32(buf.subarray(0, MFP.SIZE - 4)) !== dv.getUint32(MFP.SIZE - 4, true)) return 'CRC mismatch';
    return null;
  }
  function mfpRead(buf) {
    const why = mfpCheck(buf); if (why) throw new Error(why);
    const v = structUnpack(MFP.BODY, buf, 8); let i = 0; const take = (n) => { const r = v.slice(i, i + n); i += n; return r; };
    const p = defaultPatch();
    const nb = v[i++]; let z = nb.indexOf(0); if (z < 0) z = nb.length;
    p.name = Array.from(nb.subarray(0, z)).map((c) => (c < 128 ? String.fromCharCode(c) : '�')).join('');
    for (const k of ['voice_mode', 'max_voices', 'bend_range', 'vel_sens', 'glide', 'master']) p[k] = v[i++];
    p.macro = take(8);
    for (const o of p.osc) [o.table, o.flags, o.semis, o.drift, o.fine, o.position, o.warp, o.amp, o.route] = take(9);
    [p.noise.amp, p.noise.color, p.noise.route] = take(3);
    for (const f of p.filt) [f.type, , f.cutoff, f.res, f.drive, f.keytrack] = take(6);
    [p.serpar, p.f1_mix, p.f2_mix] = take(3);
    for (const e of p.env) [e.delay, e.attack, e.decay, e.sustain, e.release, e.flags, , e.att_level, e.dec_level, e.sust_time] = take(10);
    for (const l of p.lfo) {
      [l.wave, l.flags, l.rate, l.phase, l.sync_div, l.loop_start, l.loop_end, , l.glide] = take(9);
      l.step_shape = take(16); l.step_level = take(16);
    }
    for (const m of p.mod) [m.src, m.dst, m.amount] = take(3);
    const fxv = take(FX_ORDER.length); p.fx = {}; FX_ORDER.forEach((k, j) => { p.fx[k] = fxv[j]; });
    const mo = p.modosc;
    [mo.semis, mo.rm_osc, mo.pm_osc, mo.pos_osc, mo.ffm_filter] = take(6);
    [mo.rm, mo.pm, mo.pos, mo.ffm] = take(4);
    return p;
  }

  // ------------------------------------------------------------------- convert.py ---
  function defaultIr() {
    const r = (n, f) => Array.from({ length: n }, (_, i) => f(i));
    return {
      schema: 'vintage-chonk/ir', schema_version: 1,
      metadata: { name: 'INIT' },
      voice: { mode: 'POLY', max_voices: 8, bend_range: 2, glide: 0.0, vel_sens: 0.5, master: 0.25 },
      macros: r(8, (i) => ({ name: String(i + 1), value: 0.0 })),
      oscillators: r(3, (i) => ({ enabled: i === 0, wavetable: 'MOOG', position: 3 / 7, warp: 0.0, semis: 0, fine_cents: 0, amp: 0.8,
        route: 0.0, key_reset: false, drift: 0.25, lo_range: false })),
      noise: { amp: 0.0, color: 0.0, route: 0.0 },
      mod_osc: { enabled: true, semis: 0, rm: 0.0, pm: 0.0, pos: 0.0, ffm: 0.0, rm_osc: 0, pm_osc: 0, pos_osc: 0, ffm_filter: 0 },
      filters: r(2, (i) => ({ type: i === 0 ? 'LP2' : 'OFF', cutoff: i === 0 ? 0.92 : 0.0, resonance: 0.0, drive: 0.0, keytrack: 0.0 })),
      routing: { serpar: 0.0, f1_mix: 1.0, f2_mix: 1.0 },
      envelopes: r(4, () => ({ delay: 0.0, attack: 0.06, decay: 0.49, sustain: 1.0, release: 0.37, loop: false, att_level: 1.0, dec_level: null, sust_time: 0.0 })),
      lfos: r(4, () => ({ wave: 'SINE', rate: 0.49, phase: 0.0, keysync: false, unipolar: false, sync: false, sync_div: 8, loop_start: 0,
        loop_end: 15, glide: 0.0, steps: r(16, () => ({ shape: 0, level: 1.0 })) })),
      modulation: [],
      effects: { fuzz: { amount: 0.0, octave: 1.0, tone: 0.5, level: 0.5 }, tape: { drive: 0.0, tone: 0.5, level: 0.5, wow: 0.0 },
        ensemble: { mode: 'OFF', mix: 0.5, rate: 0.5, depth: 0.5 }, bbd: { mix: 0.0, time: 0.3, feedback: 0.4, mod: 0.2, tone: 0.5, sync_div: null },
        oilcan: { mix: 0.0, time: 0.5, feedback: 0.7, warble: 0.15, smear: 0.1, sync_div: null }, reverb: { type: 'TWIN TANK', amount: 0.0, hold: false } },
      midi: { macro_ccs: r(8, (i) => 16 + i), modwheel_cc: 1, sustain_cc: 64 },
      compatibility: { supported: [], approximated: [], unsupported: [], missing_wavetables: [], notes: [] },
      preserved: {},
    };
  }
  function pickProfile(decoded, profiles) {   // profiles: [{name, profile}] sorted by file name
    const ver = (decoded.state || {}).version;
    const v = ver === undefined ? null : ver;
    const sorted = profiles.slice().sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    for (const p of sorted) {
      const sv = p.profile.state_version === undefined ? null : p.profile.state_version;
      if (sv === v) return [p.profile, p.name];
    }
    return [null, null];
  }
  const isDigit = (s) => /^[0-9]+$/.test(s);
  function irSet(ir, path, value) {
    const parts = path.split('.');
    let node = ir;
    for (const p of parts.slice(0, -1)) {
      if (isDigit(p)) {
        if (!Array.isArray(node)) throw new PyError('KeyError', String(parseInt(p, 10)));
        const k = parseInt(p, 10); if (k >= node.length) throw new PyError('IndexError', 'list index out of range');
        node = node[k];
      } else {
        if (Array.isArray(node) || node === null || typeof node !== 'object') throw new PyError('TypeError', 'list indices must be integers or slices, not str');
        if (!Object.prototype.hasOwnProperty.call(node, p)) throw new PyError('KeyError', `'${p}'`);
        node = node[p];
      }
    }
    const last = parts[parts.length - 1];
    if (isDigit(last)) {
      const k = parseInt(last, 10);
      if (Array.isArray(node)) { if (k >= node.length) throw new PyError('IndexError', 'list assignment index out of range'); node[k] = value; }
      else node[k] = value;
    } else {
      if (Array.isArray(node)) throw new PyError('TypeError', 'list indices must be integers or slices, not str');
      node[last] = value;
    }
  }
  function pyLog(x) { if (!(x > 0)) throw new PyError('ValueError', 'math domain error'); return Math.log(x); }
  function sortPts(points, filterY) {
    const pts = points.filter(([, y]) => (filterY ? (y !== 0 && y !== null && y !== false) : true)).map((p) => [p[0], p[1]]);
    pts.sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));
    return pts;
  }
  function seg(pts, v) {
    if (pts.length < 2) throw new PyError('IndexError', 'list index out of range');
    if (v <= pts[0][0]) return [pts[0], pts[1]];
    if (v >= pts[pts.length - 1][0]) return [pts[pts.length - 2], pts[pts.length - 1]];
    let k = 1; while (!(pts[k][0] >= v)) k++;
    return [pts[k - 1], pts[k]];
  }
  function logInterp(points, v) {
    const [[x0, y0], [x1, y1]] = seg(sortPts(points, true), v);
    return Math.exp(pyLog(y0) + (v - x0) * (pyLog(y1) - pyLog(y0)) / (x1 - x0));
  }
  function transform(v, t) {
    const kind = t.kind === undefined ? 'linear' : t.kind;
    if (kind === 'linear') {
      const [a, b] = t.from || [0, 1];
      const [c, d] = t.to || [0, 1];
      let x = c + (v - a) * (d - c) / (b - a);
      if (t.round) x = pyRound(x);
      return x;
    }
    if (kind === 'enum') {
      if (!t.values) throw new PyError('KeyError', "'values'");
      const names = t.values;
      return names[pmax(0, pmin(names.length - 1, pyRound(v * (names.length - 1))))];
    }
    if (kind === 'massive_time') {
      if (!t.points) throw new PyError('KeyError', "'points'");
      let sec = logInterp(t.points, v);
      if (t.curve === 'ramp90') sec /= 0.9;
      return pmax(0.0, pmin(1.0, pyLog(pmax(sec, 0.0005) / 0.0005) / Math.log(60000.0)));
    }
    if (kind === 'massive_lfo_rate') {
      if (!t.points) throw new PyError('KeyError', "'points'");
      const hz = logInterp(t.points, v);
      return pmax(0.0, pmin(1.0, pyLog(pmax(hz, 0.02) / 0.02) / Math.log(2000.0)));
    }
    if (kind === 'table') {
      if (!t.points) throw new PyError('KeyError', "'points'");
      const pts = sortPts(t.points, false);
      if (v <= pts[0][0]) return pts[0][1];
      for (let i = 0; i + 1 < pts.length; i++) {
        const [x0, y0] = pts[i], [x1, y1] = pts[i + 1];
        if (v <= x1) return y0 + (v - x0) * (y1 - y0) / (x1 - x0);
      }
      return pts[pts.length - 1][1];
    }
    if (kind === 'power') return Math.pow(pmax(0.0, pmin(1.0, v)), t.exp === undefined ? 2.0 : t.exp);
    if (kind === 'massive_master') {
      if (t.k === undefined) throw new PyError('KeyError', "'k'");
      return pmax(0.0, pmin(1.0, t.k * v * v / 4.0));
    }
    if (kind === 'massive_cutoff') {
      if (!t.points) throw new PyError('KeyError', "'points'");
      const hz = logInterp(t.points, v);
      const note = 69 + 12 * Math.log2(pmax(hz, 8.0) / 440.0);
      return pmax(0.0, pmin(1.0, note / 136.0));
    }
    if (kind === 'bool') return v >= (t.threshold === undefined ? 0.5 : t.threshold);
    throw new PyError('ValueError', `unknown transform ${kind}`);
  }
  const CAUGHT = new Set(['KeyError', 'IndexError', 'ValueError']);
  function decodedToIr(decoded, profile, profileName = '') {
    const ir = defaultIr();
    const comp = ir.compatibility;
    const md = decoded.metadata || {};
    ir.metadata = { name: md.name || 'UNNAMED', source_app: md.app === undefined ? null : md.app,
      source_app_version: md.app_version === undefined ? null : md.app_version,
      source_sha256: (decoded.source || {}).sha256 === undefined ? null : decoded.source.sha256, mapping_profile: profileName };
    const st = decoded.state || {};
    const fb = st.float_block || {};
    const vals = fb.values || [];
    ir.preserved = { state_version: st.version === undefined ? null : st.version, float_block: vals,
      float_block_boundary: fb.boundary === undefined ? null : fb.boundary,
      raw_state_b64: st.raw_b64 === undefined ? null : st.raw_b64, decoder_warnings: decoded.warnings || [] };
    (st.macro_names || []).slice(0, 8).forEach((m, i) => { ir.macros[i].name = m.text; });
    if (!profile) {
      comp.notes.push(`no mapping profile for state version ${st.version === undefined || st.version === null ? 'None' : st.version}: every Massive value is preserved, none is applied`);
      for (let i = 0; i < vals.length; i++) comp.unsupported.push(`float[${i}]`);
      return ir;
    }
    const mapped = new Set();
    for (const ent of profile.float_params || []) {
      const i = ent.index;
      const mname = ent.massive === undefined ? '?' : ent.massive;
      if (i >= vals.length) { comp.unsupported.push(`float[${i}] (${mname}): missing in this file`); continue; }
      try {
        irSet(ir, ent.ir, transform(vals[i], ent.transform || {}));
      } catch (e) {
        if (e instanceof PyError && CAUGHT.has(e.kind)) { comp.unsupported.push(`float[${i}] (${mname}): ${e.message}`); continue; }
        throw e;
      }
      mapped.add(i);
      const label = `float[${i}] ${mname} -> ${ent.ir}`;
      if (ent.status === 'verified' && !ent.approximation) comp.supported.push(label);
      else comp.approximated.push(`${label} (${ent.approximation || `mapping ${ent.status === undefined ? 'unverified' : ent.status}`})`);
    }
    for (const ent of profile.modulation || []) {
      ir.modulation.push(Object.assign({}, ent.slot));
      comp.approximated.push(`modulation ${pyRepr(ent.slot)} (${ent.status === undefined ? 'unverified' : ent.status})`);
    }
    const names = new Map();
    for (const e of profile.float_names || []) names.set(e.index, e.massive);
    for (let i = 0; i < vals.length; i++) {
      if (!mapped.has(i)) {
        const label = names.has(i) ? ` ${names.get(i)}` : '';
        comp.unsupported.push(`float[${i}]${label} = ${fmtG(vals[i], 5)} (unmapped, preserved)`);
      }
    }
    if (profile.rules === 'massive_v24' && st.raw_b64) {
      const byName = {};
      for (const [i, n] of names) if (i < vals.length) byName[n] = vals[i];
      rulesApply(ir, b64decode(st.raw_b64), profile, byName);
    }
    for (const o of ir.oscillators) if (o.massive_wavetable) comp.missing_wavetables.push(o.massive_wavetable);
    comp.notes.push(...(profile.notes || []));
    comp.notes.push('wavetables are Vintage Chonk originals, not Massive\'s factory tables',
      'filters are a generic TPT state-variable design, not measured against Massive',
      'no claim of acoustic identity: no reference recordings compared yet');
    return ir;
  }
  function q(x, lo = 0.0, hi = 1.0) {
    x = pmax(lo, pmin(hi, Number(num(x))));
    return pmax(-32768, pmin(32767, pyRound(x * 32767)));
  }
  function idx(list, v, what) {
    const i = list.indexOf(v);
    if (i < 0) throw new PyError('ValueError', `${pyRepr(v)} is not in list (${what})`);
    return i;
  }
  function get(o, k, d) { return o && Object.prototype.hasOwnProperty.call(o, k) ? o[k] : d; }
  function truthy(v) { return !(v === undefined || v === null || v === false || v === 0 || v === '' || (Array.isArray(v) && !v.length)); }
  function irToPatch(ir) {
    const p = defaultPatch();
    const nm = get(ir.metadata, 'name', 'UNNAMED');
    p.name = Array.from(String(nm).replace(/[^ -~]/gu, '?')).slice(0, 23).join('');
    const v = ir.voice;
    p.voice_mode = idx(MFP.VOICE_MODES, v.mode, 'VOICE_MODES');
    p.max_voices = pyInt(pmax(1, pmin(8, num(v.max_voices))));
    p.bend_range = pyInt(pmax(0, pmin(24, num(v.bend_range))));
    p.glide = q(v.glide);
    p.vel_sens = pyRound(pmax(0, pmin(1, num(v.vel_sens))) * 127);
    p.master = q(v.master);
    p.macro = ir.macros.map((m) => q(m.value));
    p.osc.forEach((o, i) => {
      const s = ir.oscillators[i]; if (!s) return;
      o.table = idx(MFP.WAVETABLES, s.wavetable, 'WAVETABLES');
      o.flags = (truthy(s.enabled) ? MFP.OSC_ON : 0) | (truthy(get(s, 'key_reset')) ? MFP.OSC_RESET : 0) | (truthy(get(s, 'lo_range')) ? MFP.OSC_LO : 0);
      o.drift = pyRound(pmax(0.0, pmin(1.0, num(get(s, 'drift', 0.0)))) * 127);
      o.semis = pyInt(pmax(-64, pmin(63, num(s.semis))));
      o.fine = pyInt(pmax(-100, pmin(100, num(s.fine_cents))));
      o.position = q(s.position); o.warp = q(s.warp); o.amp = q(s.amp); o.route = q(s.route);
    });
    const n = ir.noise;
    p.noise = { amp: q(n.amp), color: q(n.color), route: q(n.route) };
    p.filt.forEach((f, i) => {
      const s = ir.filters[i]; if (!s) return;
      f.type = idx(MFP.FILTERS, s.type, 'FILTERS');
      f.cutoff = q(s.cutoff); f.res = q(s.resonance); f.drive = q(s.drive);
      f.keytrack = q(s.keytrack, -1, 1);
    });
    const r = ir.routing;
    p.serpar = q(r.serpar); p.f1_mix = q(r.f1_mix); p.f2_mix = q(r.f2_mix);
    p.env.forEach((e, i) => {
      const s = ir.envelopes[i]; if (!s) return;
      Object.assign(e, { delay: q(s.delay), attack: q(s.attack), decay: q(s.decay), sustain: q(s.sustain), release: q(s.release),
        flags: truthy(s.loop) ? MFP.ENV_LOOP : 0, att_level: q(get(s, 'att_level', 1.0)),
        dec_level: get(s, 'dec_level', null) === null ? -1 : q(s.dec_level), sust_time: q(get(s, 'sust_time', 0.0)) });
    });
    p.lfo.forEach((l, i) => {
      const s = ir.lfos[i]; if (!s) return;
      const given = get(s, 'steps') || [];
      let steps = given.slice(0, 16);
      const pad = 16 - given.length;
      for (let k = 0; k < pad; k++) steps.push({ shape: 0, level: 1.0 });
      Object.assign(l, { wave: idx(MFP.LFO_WAVES, s.wave, 'LFO_WAVES'), rate: q(s.rate), phase: q(s.phase),
        flags: (truthy(s.keysync) ? MFP.LFO_KEYSYNC : 0) | (truthy(s.unipolar) ? MFP.LFO_UNIPOLAR : 0) | (truthy(get(s, 'sync')) ? MFP.LFO_SYNC : 0),
        sync_div: pyInt(pmax(0, pmin(12, num(get(s, 'sync_div', 8))))),
        loop_start: pyInt(pmax(0, pmin(15, num(get(s, 'loop_start', 0))))),
        loop_end: pyInt(pmax(0, pmin(15, num(get(s, 'loop_end', 15))))),
        glide: q(get(s, 'glide', 0.0)),
        step_shape: steps.map((st) => pyInt(pmax(0, pmin(31, num(get(st, 'shape', 0)))))),
        step_level: steps.map((st) => pyRound(pmax(-1.0, pmin(1.0, num(get(st, 'level', 1.0)))) * 127)) });
    });
    if (ir.modulation.length > MFP.NMOD)
      ir.compatibility.unsupported.push(`${ir.modulation.length - MFP.NMOD} modulation slots beyond the ${MFP.NMOD} the engine has`);
    ir.modulation.slice(0, MFP.NMOD).forEach((s, i) => {
      Object.assign(p.mod[i], { src: idx(MFP.SOURCES, s.source, 'SOURCES'), dst: idx(MFP.DESTINATIONS, s.destination, 'DESTINATIONS'), amount: q(s.amount, -1, 1) });
    });
    const mo = ir.mod_osc || {};
    const on = truthy(get(mo, 'enabled', true));
    p.modosc = { semis: pyInt(pmax(-48, pmin(48, num(get(mo, 'semis', 0))))),
      rm_osc: on ? pyInt(get(mo, 'rm_osc', 0)) : 0, pm_osc: on ? pyInt(get(mo, 'pm_osc', 0)) : 0,
      pos_osc: on ? pyInt(get(mo, 'pos_osc', 0)) : 0, ffm_filter: on ? pyInt(get(mo, 'ffm_filter', 0)) : 0,
      rm: q(get(mo, 'rm', 0)), pm: q(get(mo, 'pm', 0)), pos: q(get(mo, 'pos', 0)), ffm: q(get(mo, 'ffm', 0)) };
    const fx = ir.effects;
    const fz = fx.fuzz, tp = fx.tape, en = fx.ensemble, bb = fx.bbd, oc = fx.oilcan, rv = fx.reverb;
    p.fx = { fuzz_amount: q(fz.amount), fuzz_octave: q(fz.octave), fuzz_tone: q(fz.tone), fuzz_level: q(fz.level),
      tape_drive: q(tp.drive), tape_tone: q(tp.tone), tape_level: q(tp.level), tape_wow: q(tp.wow),
      ens_mode: idx(MFP.ENS_MODES, en.mode, 'ENS_MODES'), ens_mix: q(en.mix), ens_rate: q(en.rate), ens_depth: q(en.depth),
      bbd_mix: q(bb.mix), bbd_time: q(bb.time), bbd_feedback: q(bb.feedback), bbd_mod: q(bb.mod), bbd_tone: q(bb.tone),
      oil_mix: q(oc.mix), oil_time: q(oc.time), oil_feedback: q(oc.feedback), oil_warble: q(oc.warble), oil_smear: q(oc.smear),
      rev_type: idx(MFP.REV_TYPES, rv.type, 'REV_TYPES'), rev_hold: truthy(rv.hold) ? 1 : 0, rev_amount: q(rv.amount),
      fx_sync: fxSync(get(bb, 'sync_div'), get(oc, 'sync_div')) };
    return p;
  }
  function reportMarkdown(ir) {
    const c = ir.compatibility, m = ir.metadata;
    const s = (x) => (x === undefined || x === null ? 'None' : x);
    const lines = [`# Compatibility report: ${s(m.name)}`, '', `Mapping profile: ${m.mapping_profile || 'none'}  `,
      `Source: ${s(m.source_app)} ${s(m.source_app_version)}, sha256 ${s(m.source_sha256)}`, '',
      '| Supported | Approximated | Unsupported (preserved) | Missing wavetables |', '|---|---|---|---|',
      `| ${c.supported.length} | ${c.approximated.length} | ${c.unsupported.length} | ${c.missing_wavetables.length} |`, ''];
    for (const [title, key] of [['Supported', 'supported'], ['Approximated', 'approximated'], ['Unsupported', 'unsupported'],
      ['Missing wavetables', 'missing_wavetables'], ['Fidelity notes', 'notes']]) {
      lines.push(`## ${title}`);
      lines.push(...(c[key].length ? c[key].map((x) => `- ${x}`) : ['- (none)']));
      lines.push('');
    }
    return lines.join('\n');
  }

  // -------------------------------------------------------------- massive_rules.py ---
  const R_FILTERS = { None: 'OFF', 'Lowpass 4': 'LP4', 'Lowpass 2': 'LP2', 'Highpass 4': 'HP', 'Highpass 2': 'HP', Bandpass: 'BP', Bandreject: 'NOTCH', 'Double Notch': 'NOTCH' };
  const FILTER_APPROX = { 'Highpass 4': 'HP4 played as a 2-pole HP', Allpass: 'no allpass: LP2', Scream: 'no Scream model: LP2',
    Daft: 'no Daft model: LP2', Comb: 'no comb filter: BP', Acid: 'Massive Acid -> MFE ACID (303-style, not measured against Massive)',
    'Double Notch': 'Double Notch is not provided : one notch' };
  const FILTER_FALLBACK = { Allpass: 'LP2', Scream: 'LP2', Daft: 'LP2', Comb: 'BP', Acid: 'ACID' };
  // Massive master FX -> Vintage Chonk master effects (17 Nov 2026); twin of massive_rules.py
  const FX = { Reverb: 'reverb', 'Reverb Sm': 'reverb', Chorus: 'ensemble', 'Chorus 1': 'ensemble', 'Chorus E': 'ensemble',
    'Dim Exp': 'ensemble', 'Flanger +': 'ensemble', 'Flanger +1': 'ensemble', 'Flanger -': 'ensemble', 'Flanger -1': 'ensemble',
    Phaser: 'ensemble', 'Phaser 1': 'ensemble', Delay: 'bbd', 'Delay S': 'bbd', 'C Tube': 'tape', 'T Tube': 'tape', 'B Tube': 'tape' };
  const FX_SCREEN = { reverb: 'TWIN TANK', ensemble: 'ENSEMBLE', bbd: 'BBD ECHO', tape: 'TAPE' };
  const FX_NOTE = { 'Flanger +': 'no flanger: ENSEMBLE II', 'Flanger +1': 'no flanger: ENSEMBLE II',
    'Flanger -': 'no flanger: ENSEMBLE II', 'Flanger -1': 'no flanger: ENSEMBLE II',
    Phaser: 'no phaser: ENSEMBLE II', 'Phaser 1': 'no phaser: ENSEMBLE II',
    'C Tube': 'tube as TAPE drive', 'T Tube': 'tube as TAPE drive', 'B Tube': 'tube as TAPE drive' };
  const MOOG_WAVES = ['triangle', 'shark', 'reverse saw', 'saw', 'square', 'wide pulse', 'narrow pulse', 'narrowest pulse'];
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  function moogWaveFor(name) {
    const n = name.toLowerCase();
    if (['saw', '-sw', 'sw '].some((k) => n.includes(k))) return 3;
    if (n.includes('pwm') || n.includes('pulse')) return 5;
    if (n.includes('squ') || n.includes('sqr') || n.includes('square')) return 4;
    if (n.includes('tri')) return 0;
    if (n.includes('sin')) return 0;
    return 3;
  }
  function choice(raw, ent) {
    const off = ent.offset;
    if (off >= raw.length) return null;
    if (has(ent, 'values')) { const k = String(raw[off]); return has(ent.values, k) ? ent.values[k] : null; }
    if (has(ent, 'bit')) {
      const on = (raw[off] >> ent.bit) & 1;
      const other = ent.choices.filter((c) => c !== ent.set_means);
      return on ? ent.set_means : (other.length ? other[0] : null);
    }
    return null;
  }
  // ------------------------------------------------- massive_mods.py (slots 5-8) ---
  const MM = {
    STEPS: 16, FLOAT_BASE: 4, F_RATE: 58, F_AMP: 62, F_XFADE_GLIDE: 66, F_IND_LEV: 70, F_SYNC: 74, F_RESTART: 78,
    KEY_GLIDE_TIME: 126, KEY_RATE: 58, PRE_BYTES: 17, KIND: { 0: 'lfo', 1: 'performer', 2: 'stepper' },
    SIZE: { 0: 24, 1: 336, 2: 144 }, DEN: [1, 2, 4, 8, 12, 16, 24, 32],
    MFE_SYNC_BEATS: [1 / 8, 1 / 6, 1 / 4, 1 / 3, 3 / 8, 1 / 2, 2 / 3, 3 / 4, 1, 3 / 2, 2, 4, 8],
    LFO_CURVES: 35, PERF_SHAPES: 16,
    GLIDE_TAU: [[0.0, 0.0], [0.25, 0.002], [0.5, 0.055], [0.625, 0.12], [0.75, 0.235], [0.875, 0.30], [1.0, 0.31]],
    RATE_HZ: [[0.3, 0.2], [0.5, 1.0], [0.7, 5.4], [0.9, 28.0], [1.0, 41.0]], HOST_BPM: 120.0,
  };
  const W = { SINE: 0, TRI: 1, SAW_UP: 2, SAW_DOWN: 3, SQUARE: 4, SH: 5, PERFORMER: 6, STEPPER: 7 };
  const LFO_FIT = {
    0: [W.SINE, 0.0, false, 0.03], 1: [W.SAW_DOWN, 0.0, false, 0.0], 2: [W.SQUARE, 0.0, false, 0.0],
    3: [W.TRI, 0.75, false, 0.03], 4: [W.SAW_DOWN, 0.0, false, 0.66], 5: [W.SINE, 0.0, false, 0.34],
    6: [W.SAW_DOWN, 0.925, false, 0.24], 7: [W.SAW_DOWN, 0.925, false, 0.44],
    8: [W.SAW_DOWN, 0.925, false, 0.50], 9: [W.SAW_UP, 0.3, false, 0.71], 10: [W.SAW_UP, 0.075, false, 0.79],
    11: [W.SAW_DOWN, 0.875, false, 0.59], 12: [W.SAW_UP, 0.4, false, 0.80], 13: [W.SINE, 0.75, false, 0.28],
    14: [W.TRI, 0.675, false, 0.43], 15: [W.TRI, 0.8, false, 0.45], 16: [W.SAW_UP, 0.05, false, 0.45],
    17: [W.SINE, 0.175, false, 0.39], 18: [W.SAW_UP, 0.65, false, 0.57], 19: [W.SAW_UP, 0.45, false, 0.56],
    20: [W.TRI, 0.8, false, 0.34], 21: [W.SAW_DOWN, 0.75, false, 0.63], 22: [W.SAW_UP, 0.725, false, 0.61],
    23: [W.SAW_DOWN, 0.0, false, 0.64], 24: [W.SAW_DOWN, 0.0, false, 0.88], 25: [W.SAW_DOWN, 0.0, false, 0.59],
    26: [W.SAW_DOWN, 0.0, false, 0.66], 27: [W.SQUARE, 0.95, true, 0.14], 28: [W.TRI, 0.05, false, 0.31],
    29: [W.SAW_UP, 0.25, true, 0.51], 30: [W.SQUARE, 0.0, true, 0.57], 31: [W.SAW_UP, 0.075, true, 0.10],
    32: [W.SINE, 0.75, true, 0.02], 33: [W.TRI, 0.75, true, 0.02], 34: [W.TRI, 0.75, true, 0.24],
  };
  const WAVE_NAME = ['SINE', 'TRI', 'SAW_UP', 'SAW_DOWN', 'SQUARE', 'SH', 'PERFORMER', 'STEPPER'];
  const PERF_TO_MFE = Array.from({ length: 16 }, (_, i) => i);
  // readers that fail with CPython's messages (a failure text ends up in a compatibility label)
  function sErr(b, o, n) {
    return new PyError('struct.error', `unpack_from requires a buffer of at least ${o + n} bytes for unpacking ${n} bytes at offset ${o} (actual buffer size is ${b.length})`);
  }
  const dvOf = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
  const rd = (n, f) => (b, o) => { if (o < 0 || o + n > b.length) throw sErr(b, o, n); return f(dvOf(b), o); };
  const mU32 = rd(4, (d, o) => d.getUint32(o, true)), mI32 = rd(4, (d, o) => d.getInt32(o, true));
  const mF32 = rd(4, (d, o) => d.getFloat32(o, true)), mU16 = rd(2, (d, o) => d.getUint16(o, true));
  function mByte(b, o) { if (o >= b.length) throw new PyError('IndexError', 'index out of range'); return b[o]; }
  function pyDiv(a, b) { if (b === 0) throw new PyError('ZeroDivisionError', 'float division by zero'); return a / b; }
  const ModSlotError = (msg) => new PyError('ModSlotError', msg);

  function findKnobRecord(state, key, start = 4) {
    const pat = new Uint8Array(5); dvOf(pat).setUint32(0, key, true);
    let i = findBytes(state, pat, start);
    while (i >= 0) {
      for (let n = 1; n < 9; n++) {
        const c = i - 10 * n - 4;
        if (c < 12 || mU32(state, c) !== n) continue;
        let ok = true;
        for (let k = 0; k < n; k++) {
          const p = c + 4 + 10 * k;
          if (p + 10 > state.length) throw sErr(state, p, 10);
          const d = mF32(state, p), s = mI32(state, p + 4), u = mU16(state, p + 8);
          if (!(-1.0001 <= d && d <= 1.0001 && -1 <= s && s <= 64 && (u === 0 || u === 256 || u === 512))) { ok = false; break; }
        }
        if (ok) return i + 5;
      }
      i = findBytes(state, pat, i + 1);
    }
    throw ModSlotError(`no record for knob ${key}`);
  }
  function interp(table, x) {
    if (x <= table[0][0]) return table[0][1];
    for (let k = 0; k + 1 < table.length; k++) {
      const [x0, y0] = table[k], [x1, y1] = table[k + 1];
      if (x <= x1) return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
    }
    return table[table.length - 1][1];
  }
  function rateHz(knob) {
    const pts = MM.RATE_HZ.map(([k, h]) => [k, Math.log(h)]);
    if (knob < pts[0][0]) {
      const [[k0, l0], [k1, l1]] = pts;
      return Math.exp(l0 + (l1 - l0) * (knob - k0) / (k1 - k0));
    }
    return Math.exp(interp(pts, knob));
  }
  function nearestSyncDiv(beats) {   // Python min() keeps the first of equal keys
    let best = 0, bestV = null;
    for (let d = 0; d < MM.MFE_SYNC_BEATS.length; d++) {
      const v = Math.abs(pyLog(pyDiv(MM.MFE_SYNC_BEATS[d], beats)));
      if (bestV === null || v < bestV) { bestV = v; best = d; }
    }
    return [best, Math.abs(MM.MFE_SYNC_BEATS[best] - beats) < 1e-9];
  }
  function flags16(b, o) {
    const out = [];
    for (let k = 0; k < MM.STEPS; k++) out.push(Array.from(b.subarray(Math.min(o + 3 * k, b.length), Math.min(o + 3 * k + 3, b.length))));
    return out;
  }
  function parseLfo(b, o) {
    if (o + 4 + 20 > b.length) throw sErr(b, o + 4, 20);
    return { curve_count: mU32(b, o + 4), phase_1: mF32(b, o + 8), phase_2: mF32(b, o + 12), curve_1: mU32(b, o + 16), curve_2: mU32(b, o + 20) };
  }
  function parsePerformer(b, o) {
    const hdr = mU32(b, o + 4), steps = [];
    for (let k = 0; k < MM.STEPS; k++) {
      const p = o + 8 + 17 * k;
      if (p + 17 > b.length) throw sErr(b, p, 17);
      steps.push({ level_1: mF32(b, p), curve_1: mU32(b, p + 4), level_2: mF32(b, p + 8), curve_2: mU32(b, p + 12), own_row_2: b[p + 16] });
    }
    const q = o + 8 + 17 * MM.STEPS;
    return { header: hdr, steps, row_count: mU32(b, q), step_flags: flags16(b, q + 4), after_flags: mU32(b, q + 4 + 48) };
  }
  function parseStepper(b, o) {
    const hdr = mU32(b, o + 4);
    if (o + 8 + 64 > b.length) throw sErr(b, o + 8, 64);
    const vals = []; for (let k = 0; k < 16; k++) vals.push(mF32(b, o + 8 + 4 * k));
    const glide = Array.from(b.subarray(o + 72, o + 88));
    const q = o + 88;
    return { header: hdr, values: vals, glide_steps: glide, row_count: mU32(b, q), step_flags: flags16(b, q + 4), after_flags: mU32(b, q + 4 + 48) };
  }
  function parseRaw(state) {
    if (state.length < 4 + 4 * 137) throw ModSlotError('state too short');
    const ver = mU32(state, 0);
    if (![22, 23, 24].includes(ver)) throw ModSlotError(`state version ${ver} not supported (22-24 decoded)`);
    let p = findKnobRecord(state, MM.KEY_GLIDE_TIME, 4 + 4 * 137) + MM.PRE_BYTES;
    const raw = [];
    for (let i = 0; i < 4; i++) {
      const t = mU32(state, p);
      if (!has(MM.KIND, t)) throw ModSlotError(`slot ${i + 5}: type ${t} at offset ${p} (expected 0, 1 or 2)`);
      const rec = [parseLfo, parsePerformer, parseStepper][t](state, p);
      Object.assign(rec, { type: t, kind: MM.KIND[t], offset: p });
      raw.push(rec);
      p += MM.SIZE[t];
    }
    if (mU32(state, p) !== 3) throw ModSlotError(`no end marker (u32 3) after slot 8 at offset ${p}`);
    const nPre = ver === 22 ? 2 : 3;
    for (let i = 0; i < 4; i++) {
      const d = findKnobRecord(state, MM.KEY_RATE + i, 4 + 4 * 137);
      Object.assign(raw[i], { pos_sync: mByte(state, d + 1), sync_num: mU32(state, d + nPre), sync_den_index: mU32(state, d + nPre + 4),
        sync_bytes_offset: d, rate_record_bytes: hex(state.subarray(d, d + nPre)) });
    }
    return raw;
  }
  function q15(x) { return pmax(-32768, pmin(32767, pyRound(x * 32767))); }
  function knobs(state, i) {
    const f = (ix) => mF32(state, MM.FLOAT_BASE + 4 * (ix + i));
    return { rate: f(MM.F_RATE), amp: f(MM.F_AMP), xfade_glide: f(MM.F_XFADE_GLIDE), ind_lev: f(MM.F_IND_LEV), sync: f(MM.F_SYNC), restart: f(MM.F_RESTART) };
  }
  function stepGain(flag, indLev) {
    if (flag.length !== 3) throw new PyError('ValueError', `not enough values to unpack (expected 3, got ${flag.length})`);
    const [x, on, z] = flag;
    if (!on || z) return 0.0;
    return x ? indLev : 1.0;
  }
  function decodeModSlots(state) {
    const raw = parseRaw(state);
    const out = [];
    raw.forEach((r, i) => {
      const kn = knobs(state, i);
      const notes = [];
      const conf = { kind: 'verified', output_gain: 'verified', rate_knob: 'verified' };
      const num = r.sync_num, di = r.sync_den_index;
      const den = di < MM.DEN.length ? MM.DEN[di] : null;
      const m = { wave: W.SINE, flags: 0, rate_knob: kn.rate, rate_hz: rateHz(kn.rate), phase: 0, sync_div: 8, loop_start: 0,
        loop_end: MM.STEPS - 1, glide: 0, step_shape: new Array(16).fill(0), step_level: new Array(16).fill(127) };
      if (kn.sync >= 0.5) {
        m.flags |= 4;
        if (den) {
          const beats = 4.0 * num / den;
          const [sd, exact] = nearestSyncDiv(beats);
          m.sync_div = sd; m.sync_beats = beats;
          conf.sync_div = exact ? 'verified' : 'approximate';
          if (!exact) notes.push(`sync ${num}/${den} (${fmtG(beats, 6)} beats) -> nearest engine division ${fmtG(MM.MFE_SYNC_BEATS[sd], 6)} beats`);
        } else { conf.sync_div = 'unknown'; notes.push(`sync denominator index ${di} unknown`); }
      }
      if (kn.restart >= 0.5) m.flags |= 1;
      conf['flags.keysync'] = 'verified';
      conf.pos_sync = 'hypothesis';
      if (kn.restart < 0.5 && r.pos_sync) notes.push('Restart off with Pos Sync on: phase follows the host position in Massive (assumed); the engine free-runs');
      const xf = kn.xfade_glide;
      if (r.type === 0) {
        const row1 = xf >= 0.5;
        let curve = row1 ? r.curve_1 : r.curve_2;
        const ph = row1 ? r.phase_1 : r.phase_2;
        if (curve >= MM.LFO_CURVES) curve = 0;
        const [wave, fph, uni, err] = LFO_FIT[curve];
        m.wave = wave;
        const s = ph + fph;
        if (!Number.isFinite(s)) throw new PyError('ValueError', 'math domain error');
        m.phase = q15(s % 1.0);
        if (uni) m.flags |= 2;
        conf.wave = curve <= 3 ? 'verified' : (err < 0.35 ? 'approximate' : 'poor');
        conf.phase = 'verified';
        conf.xfade = 'verified';
        if (xf > 0.0 && xf < 1.0 && r.curve_1 !== r.curve_2) {
          notes.push(`XFade ${fmtF(xf, 2)} blends curves ${r.curve_1} and ${r.curve_2}; the engine plays curve ${curve} only`);
          conf.wave = 'approximate';
        }
        if (curve > 3) notes.push(`Massive LFO library curve ${curve}: nearest engine wave ${WAVE_NAME[wave]} (rms error ${fmtF(err, 2)} of full scale)`);
      } else if (r.type === 1) {
        m.wave = W.PERFORMER; m.flags |= 2;
        r.steps.forEach((s, k) => {
          const a = [s.level_1, s.curve_1];
          const b = s.own_row_2 ? [s.level_2, s.curve_2] : a;
          let [lev, cv] = xf >= 0.5 ? a : b;
          if (a[1] === b[1]) lev = xf * a[0] + (1 - xf) * b[0];
          else if (xf > 0.0 && xf < 1.0) notes.push(`step ${k + 1}: XFade ${fmtF(xf, 2)} between curves ${a[1]} and ${b[1]}; curve ${cv} kept`);
          if (cv >= MM.PERF_SHAPES) cv = 1;
          const g = stepGain(r.step_flags[k], kn.ind_lev);
          m.step_shape[k] = PERF_TO_MFE[cv];
          m.step_level[k] = pyRound(127 * pmax(-1.0, pmin(1.0, lev * g)));
        });
        Object.assign(conf, { step_shape: 'verified', step_level: 'verified', loop: 'verified' });
        if (r.steps.some((s) => s.curve_1 === 3 || s.curve_1 === 8)) {
          notes.push('curves 3/8 used: engine shapes 3 (VALLEY) / 8 (HOLD DROP) differ from Massive\'s (exponential fall / full hold)');
          conf.step_shape = 'approximate';
        }
      } else {
        m.wave = W.STEPPER; m.flags |= 2;
        r.values.forEach((v, k) => {
          const g = stepGain(r.step_flags[k], kn.ind_lev);
          m.step_level[k] = pyRound(127 * pmax(-1.0, pmin(1.0, v * g)));
        });
        const on = []; r.glide_steps.forEach((g, k) => { if (g) on.push(k); });
        const tau = interp(MM.GLIDE_TAU, xf);
        m.glide_seconds = on.length ? tau : 0.0;
        const stepS = (m.flags & 4 && den) ? 4.0 * num / den * 60.0 / MM.HOST_BPM : pyDiv(1.0, m.rate_hz);
        m.glide = on.length ? q15(pmin(1.0, pyDiv(tau, stepS))) : 0;
        Object.assign(conf, { step_level: 'verified', loop: 'verified', glide_steps: 'verified', glide: 'approximate' });
        if (on.length && on.length < MM.STEPS) notes.push(`glide only into steps [${on.map((k) => k + 1).join(', ')}] in Massive; the engine glides every step`);
      }
      out.push({ slot: i + 5, source: 4 + i, kind: r.kind, mfe: m, output_gain: kn.amp * kn.amp, knobs: kn, massive: r, confidence: conf, notes });
    });
    return out;
  }
  function applySlots(ir, raw) {
    const comp = ir.compatibility;
    let slots;
    try { slots = decodeModSlots(raw); } catch (e) {
      comp.unsupported.push(`modulator slots 5-8 not decoded (${e.message})`);
      return [1.0, 1.0, 1.0, 1.0];
    }
    const gains = [];
    slots.slice(0, 4).forEach((s, i) => {
      const m = s.mfe, l = ir.lfos[i];
      l.wave = m.wave < WAVE_NAME.length ? WAVE_NAME[m.wave] : 'SINE';
      l.keysync = !!(m.flags & 1); l.unipolar = !!(m.flags & 2); l.sync = !!(m.flags & 4);
      l.sync_div = m.sync_div;
      l.phase = m.phase / 32767;
      l.loop_start = m.loop_start; l.loop_end = m.loop_end;
      l.glide = m.glide / 32767;
      l.steps = m.step_shape.map((sh, k) => ({ shape: sh, level: m.step_level[k] / 127 }));
      if (truthy(m.rate_hz)) l.rate = pmax(0.0, pmin(1.0, pyLog(pmax(m.rate_hz, 0.02) / 0.02) / Math.log(2000.0)));
      gains.push(s.output_gain === undefined ? 1.0 : s.output_gain);
      const conf = s.confidence || {};
      const approx = Object.keys(conf).filter((k) => conf[k] !== 'verified');
      (approx.length ? comp.approximated : comp.supported).push(`slot ${5 + i}: ${l.wave}` + (approx.length ? ` (${approx.join(', ')} not exact)` : ''));
    });
    // for the page only (not part of the Python IR or its JSON): each slot's kind, notes and knobs
    Object.defineProperty(ir, '_slots', { value: slots, enumerable: false, configurable: true });
    return gains;
  }

  function rulesApply(ir, raw, profile, floatsByName) {
    const comp = ir.compatibility;
    const names = new Map();
    for (const e of profile.float_names || []) names.set(e.index, e.massive);
    const gains = applySlots(ir, raw);
    applyModulation(ir, raw, names, gains);
    const choices = {};
    for (const [name, ent] of Object.entries(profile.enum_params || {})) {
      const c = choice(raw, ent);
      if (c !== null && c !== undefined) choices[name] = c;
    }
    ir.preserved.massive_choices = choices;
    const ch = (k) => (has(choices, k) ? choices[k] : null);
    for (let n = 0; n < 3; n++) {
      const o = ir.oscillators[n];
      const on = ch(`osc${n + 1}_on_off`);
      if (on !== null) { o.enabled = on === 'on'; comp.supported.push(`osc${n + 1} on/off = ${on}`); }
      const wt = ch(`osc${n + 1}_wavetable`);
      if (truthy(wt)) {
        const f = moogWaveFor(wt);
        o.massive_wavetable = wt;
        o.massive_position = get(o, 'position', null);
        o.wavetable = 'MOOG';
        o.position = f / 7;
        comp.approximated.push(`osc${n + 1} wavetable '${wt}' -> Moog-style ${MOOG_WAVES[f]} (Massive wt-position ${fmtF(o.massive_position, 2)} not used)`);
      }
      const mode = ch(`osc${n + 1}_mode`);
      if (truthy(mode) && mode !== 'Spectrum') comp.approximated.push(`osc${n + 1} intensity mode '${mode}' -> phase warp`);
      else if (mode === 'Spectrum') comp.approximated.push(`osc${n + 1} intensity mode Spectrum -> phase warp (no spectral mode)`);
    }
    if (!ir.mod_osc) ir.mod_osc = {};
    const mo = ir.mod_osc;
    if (ch('mod_osc_on_off') === 'off') mo.enabled = false;
    for (const [key, field] of [['mod_osc_rm_osc', 'rm_osc'], ['mod_osc_phase_osc', 'pm_osc'], ['mod_osc_position_osc', 'pos_osc'], ['mod_osc_filter_fm_osc', 'ffm_filter']]) {
      const c = ch(key);
      if (c !== null) mo[field] = c === 'off' ? 0 : pyInt(c);
    }
    if (['rm_osc', 'pm_osc', 'pos_osc'].some((f) => truthy(get(mo, f))) && truthy(get(mo, 'enabled', true)))
      comp.approximated.push(`mod osc: ring -> osc ${get(mo, 'rm_osc', 0)}, phase -> osc ${get(mo, 'pm_osc', 0)}, position -> osc ${get(mo, 'pos_osc', 0)} (depth scales not measured)`);
    if (truthy(get(mo, 'ffm_filter')) && truthy(get(mo, 'ffm'))) comp.unsupported.push('mod osc filter FM (stored, not rendered yet)');
    if (ch('noise_on_off') === 'off') ir.noise.amp = 0.0;
    for (let n = 0; n < 2; n++) {
      const f = ir.filters[n];
      const t = ch(`filter${n + 1}_type`);
      if (t !== null) {
        f.type = (has(R_FILTERS, t) && R_FILTERS[t]) || (has(FILTER_FALLBACK, t) ? FILTER_FALLBACK[t] : 'LP2');
        (has(FILTER_APPROX, t) ? comp.approximated : comp.supported).push(`filter${n + 1} type '${t}' -> ${f.type}` + (has(FILTER_APPROX, t) ? ` (${FILTER_APPROX[t]})` : ''));
      }
      if (ch(`filter${n + 1}_on_off`) === 'off') f.type = 'OFF';
    }
    const vm = ch('voicemode');
    if (truthy(vm)) {
      ir.voice.mode = vm === 'Monophon' ? 'MONO' : 'POLY';
      (vm !== 'Monorotate' ? comp.supported : comp.approximated).push(`voice mode ${vm}`);
    }
    for (const slot of [1, 2]) {
      const t = ch(`master_fx${slot}_type`);
      if (!truthy(t) || t === 'None' || ch(`master_fx${slot}_on_off`) === 'off') continue;
      const kind = has(FX, t) ? FX[t] : null;
      const mix = has(floatsByName, `master_fx${slot}_dry_wet`) ? floatsByName[`master_fx${slot}_dry_wet`] : 0.0;
      if (kind) {
        const e = ir.effects;
        if (kind === 'reverb') Object.assign(e.reverb, { type: 'TWIN TANK', amount: mix });
        if (kind === 'ensemble') Object.assign(e.ensemble, { mode: has(FX_NOTE, t) ? 'II' : 'I', mix });
        if (kind === 'bbd') {
          Object.assign(e.bbd, { mix, time: 0.3, feedback: 0.4 });
          // Massive's delay times are note values (left n / d of a whole note): synced BBD ECHO (as massive_rules.py)
          const ln = ch(`master_fx${slot}_sd_ln`), ld = ch(`master_fx${slot}_sd_ld`);
          if (ln !== null && ld !== null && /^[0-9]+$/.test(String(ln)) && /^[0-9]+$/.test(String(ld)) && pyInt(ld) > 0) {
            e.bbd.sync_div = nearestDelayDiv(4 * pyInt(ln), pyInt(ld));
            comp.approximated.push(`master FX${slot} delay ${ln}/${ld} -> BBD ECHO synced, division ${e.bbd.sync_div}`);
          }
        }
        if (kind === 'tape') e.tape.drive = pmax(e.tape.drive, mix);
        const note = has(FX_NOTE, t) ? ` (${FX_NOTE[t]})` : '';
        comp.approximated.push(`master FX${slot} '${t}' -> ${FX_SCREEN[kind]}, dry/wet ${fmtF(mix, 2)}${note}`);
      } else comp.unsupported.push(`master FX${slot} '${t}' (no equivalent)`);
    }
    for (const slot of [1, 2]) {
      const t = ch(`insert${slot}_type`);
      if (truthy(t) && t !== 'None' && ch(`insert${slot}_on_off`) !== 'off') {
        if (['Clip', 'S Shaper', 'P Shaper'].includes(t)) {
          const k = `insert${slot}_dw_prm_1`;
          ir.effects.tape.drive = pmax(ir.effects.tape.drive, has(floatsByName, k) ? floatsByName[k] : 0.0);
          comp.approximated.push(`insert${slot} '${t}' -> TAPE drive`);
        } else comp.unsupported.push(`insert${slot} '${t}'`);
      }
    }
    return ir;
  }
  const SOURCES = { 0: 'ENV1', 1: 'ENV2', 2: 'ENV3', 3: 'ENV4', 4: 'LFO1', 5: 'LFO2', 6: 'LFO3', 7: 'LFO4', 13: 'MACRO1', 14: 'MACRO2',
    15: 'MACRO3', 16: 'MACRO4', 17: 'MACRO5', 18: 'MACRO6', 19: 'MACRO7', 20: 'MACRO8', 21: 'KEY', 22: 'VEL', 23: 'RANDOM', 24: 'AFTERTOUCH', 25: 'KEY' };
  const SOURCE_NOTES = { 21: 'Massive KTr is unipolar across the keyboard; MFE KEY is bipolar around C4', 25: 'second key-tracking source, same behaviour as 21',
    24: 'aftertouch inferred, not measured', 4: 'Massive LFO 5 -> MFE LFO 1', 5: 'Massive LFO 6 -> MFE LFO 2', 6: 'Massive LFO 7 -> MFE LFO 3', 7: 'Massive LFO 8 -> MFE LFO 4' };
  const DESTS = { osc1_pitch: 'OSC1_PITCH', osc2_pitch: 'OSC2_PITCH', osc3_pitch: 'OSC3_PITCH', osc1_position: 'OSC1_POS', osc2_position: 'OSC2_POS',
    osc3_position: 'OSC3_POS', osc1_param2: 'OSC1_WARP', osc2_param2: 'OSC2_WARP', osc3_param2: 'OSC3_WARP', osc1_amp: 'OSC1_AMP', osc2_amp: 'OSC2_AMP',
    osc3_amp: 'OSC3_AMP', noise_amp: 'NOISE_AMP', filter1_cut_prm_1: 'F1_CUTOFF', filter2_cut_prm_1: 'F2_CUTOFF', filter1_res_prm_3: 'F1_RES',
    filter2_res_prm_3: 'F2_RES', seriell_parallel: 'SERPAR', pan_fltr_path: 'PAN', insert1_dw_prm_1: 'DRIVE', mod5_rate: 'LFO1_RATE', mod6_rate: 'LFO2_RATE' };
  const CUTOFF_SCALE = 123.0 / 136.0, PITCH_SCALE = 128.0 / 64.0;
  const DEPTH_SCALE = { F1_CUTOFF: CUTOFF_SCALE, F2_CUTOFF: CUTOFF_SCALE, PITCH: PITCH_SCALE, OSC1_PITCH: PITCH_SCALE, OSC2_PITCH: PITCH_SCALE, OSC3_PITCH: PITCH_SCALE };
  const DEPTH_MEASURED = new Set(['F1_CUTOFF', 'F2_CUTOFF', 'OSC1_PITCH', 'OSC2_PITCH', 'OSC3_PITCH']);
  function modSlots(raw, index) {
    const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    for (let t = 0; t < 10; t++) {
      const key = new Uint8Array(9); const kd = new DataView(key.buffer);
      kd.setUint32(0, index >>> 0, true); key[4] = 0; kd.setUint32(5, t, true);
      const k = findBytes(raw, key, 0);
      if (k < 0) continue;
      for (const n of [3, 2, 1, 4]) {
        const c = k - 10 * n - 4;
        if (c >= 0 && dv.getUint32(c, true) === n) {
          const out = [];
          for (let s = 0; s < n; s++) {
            const depth = dv.getFloat32(c + 4 + 10 * s, true), src = dv.getInt32(c + 8 + 10 * s, true);
            if (src >= 0) out.push([depth, src]);
          }
          return out;
        }
      }
      return null;
    }
    return null;
  }
  function applyModulation(ir, raw, names, slotGains) {
    const comp = ir.compatibility;
    const items = Array.from(names.entries()).sort((a, b) => (a[0] - b[0]) || (a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0));
    for (const [i, name] of items) {
      const slots = modSlots(raw, i);
      if (!slots || !slots.length) continue;
      for (const [depth, src] of slots) {
        const s = has(SOURCES, src) ? SOURCES[src] : null, d = has(DESTS, name) ? DESTS[name] : null;
        const label = `modulation source ${src} -> ${name} depth ${fmtF(depth, 2, true)}`;
        if (s && d) {
          let amt = depth * (has(DEPTH_SCALE, d) ? DEPTH_SCALE[d] : 1.0);
          if (src >= 4 && src <= 7 && slotGains && slotGains.length) amt *= slotGains[src - 4];   // the slot's Amp knob (measured amp^2)
          const clipped = Math.abs(amt) > 1.0;
          amt = pmax(-1.0, pmin(1.0, amt));
          ir.modulation.push({ source: s, destination: d, amount: pyRoundN(amt, 5) });
          const why = [DEPTH_MEASURED.has(d) ? 'depth scale measured' : 'depth taken as knob fraction'];
          if (clipped) why.push('depth beyond the MFE range, clipped');
          if (has(SOURCE_NOTES, src)) why.push(SOURCE_NOTES[src]);
          comp.approximated.push(`${label} -> ${s} -> ${d} amount ${fmtF(amt, 3, true)} (${why.join('; ')})`);
        } else comp.unsupported.push(label + (!s ? ' (source not identified)' : ' (no MFE destination)'));
      }
    }
  }

  // ------------------------------------------------------------------------ top ---
  function convertBytes(buf, filename, profiles) {
    const decoded = decodeBytes(buf, filename);
    const [profile, pname] = pickProfile(decoded, profiles);
    const ir = decodedToIr(decoded, profile, pname);
    const patch = irToPatch(ir);           // may add to ir.compatibility, as convert.py does before writing the IR
    const mfp = mfpWrite(patch);
    return { decoded, ir, patch, mfp, profileName: pname, report: reportMarkdown(ir) };
  }

  return {
    PyError, NISError, FastLZError, pyRound, pyRoundN, fmtG, fmtF, crc32, sha256hex, b64encode, b64decode, hex,
    fastlzDecompress, nisParse, decodeBytes, MFP, MOOG_WAVES, defaultPatch, mfpWrite, mfpRead, mfpCheck, SYNC_DIVS, fxSync, nearestDelayDiv,
    defaultIr, pickProfile, transform, decodeModSlots, applySlots, decodedToIr, irToPatch, reportMarkdown, rulesApply, modSlots, convertBytes,
  };
});
