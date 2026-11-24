// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk Android UI: the native bridge. On the tablet window.AndroidSynth is the Java
// @JavascriptInterface object; in a desktop browser it is absent and a mock stands in (logs to the
// console, keeps user presets in localStorage, opens a file input for .nmsv), so the page can be tested
// in Chrome. Calls used (contract with the native side):
//   loadPreset(b64) -> "ok" | error     noteOn(note, vel)   noteOff(note)   pitchBend(v14)
//   controlChange(cc, value)            getStatus() -> JSON {voices, midi, midiDevices, sampleRate, latencyMs}
//   listPresets() -> JSON [{name, b64}] savePreset(name, b64) -> "ok"   deletePreset(name)
//   pickNmsv() -> later window.onNmsvPicked(name, b64) or window.onNmsvPicked(null, errorText)
//   openSettings()                      (3-second hold on the panel title)
'use strict';
(function (root) {
  function makeMock() {
    const log = (...a) => { try { console.log('[AndroidSynth mock]', ...a); } catch (e) { /* no console */ } };
    const KEY = 'mfm1.mock.presets';
    let mem = [];
    const load = () => { try { const t = root.localStorage.getItem(KEY); if (t) return JSON.parse(t); } catch (e) { /* storage blocked */ } return mem; };
    const store = (list) => { mem = list; try { root.localStorage.setItem(KEY, JSON.stringify(list)); } catch (e) { /* storage blocked */ } };
    let voices = 0, midi = 0;
    const held = new Set();
    return {
      isMock: true,
      loadPreset(b64) { log('loadPreset', b64.length, 'chars'); return 'ok'; },
      noteOn(n, v) { held.add(n); voices = Math.min(8, held.size); log('noteOn', n, v); },
      noteOff(n) { held.delete(n); voices = Math.min(8, held.size); log('noteOff', n); },
      pitchBend(v) { log('pitchBend', v); },
      controlChange(cc, v) { log('controlChange', cc, v); },
      getStatus() {
        if (Math.random() < 0.02) midi++;
        return JSON.stringify({ voices, midi, midiDevices: ['Mock MIDI keyboard'], sampleRate: 44100, latencyMs: 11.6 });
      },
      listPresets() { return JSON.stringify(load()); },
      savePreset(name, b64) { const l = load().filter((p) => p.name !== name); l.push({ name, b64 }); l.sort((a, b) => (a.name < b.name ? -1 : 1)); store(l); return 'ok'; },
      deletePreset(name) { store(load().filter((p) => p.name !== name)); return 'ok'; },
      pickNmsv() {
        const doc = root.document; if (!doc) return;
        const inp = doc.createElement('input'); inp.type = 'file'; inp.accept = '.nmsv';
        inp.style.display = 'none';
        inp.addEventListener('change', () => {
          const f = inp.files && inp.files[0]; inp.remove();
          if (!f) { root.onNmsvPicked(null, 'No file chosen'); return; }
          const r = new FileReader();
          r.onload = () => {
            const b = new Uint8Array(r.result); let s = '';
            for (let i = 0; i < b.length; i += 8192) s += String.fromCharCode.apply(null, b.subarray(i, i + 8192));
            root.onNmsvPicked(f.name, root.btoa(s));
          };
          r.onerror = () => root.onNmsvPicked(null, 'Could not read the file');
          r.readAsArrayBuffer(f);
        });
        doc.body.appendChild(inp); inp.click();
      },
      openSettings() { log('openSettings'); },
    };
  }

  const native = root.AndroidSynth;
  const api = native || makeMock();
  // every call guarded: a missing or throwing native method never breaks the panel
  function call(name, ...args) {
    try { if (typeof api[name] === 'function') return api[name](...args); } catch (e) { try { console.warn('bridge', name, e); } catch (x) { /* */ } }
    return undefined;
  }
  function json(name, fallback, ...args) {
    const t = call(name, ...args);
    if (typeof t !== 'string') return fallback;
    try { return JSON.parse(t); } catch (e) { return fallback; }
  }
  root.MFBridge = {
    isMock: !native,
    loadPreset: (b64) => call('loadPreset', b64),
    noteOn: (n, v) => call('noteOn', n | 0, v | 0),
    noteOff: (n) => call('noteOff', n | 0),
    pitchBend: (v) => call('pitchBend', Math.max(0, Math.min(16383, v | 0))),
    controlChange: (cc, v) => call('controlChange', cc | 0, Math.max(0, Math.min(127, v | 0))),
    getStatus: () => json('getStatus', null),
    listPresets: () => { const l = json('listPresets', []); return Array.isArray(l) ? l : []; },
    savePreset: (name, b64) => call('savePreset', name, b64),
    deletePreset: (name) => call('deletePreset', name),
    pickNmsv: () => call('pickNmsv'),
    openSettings: () => call('openSettings'),
  };
})(typeof self !== 'undefined' ? self : this);
