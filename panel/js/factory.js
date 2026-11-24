// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk Android UI: the factory bank. Original sounds designed for the engine's classic
// oscillators (wavetable CLASSIC = MFE_WT_MOOG frames 0..7) and its ladder / acid / state-variable
// filters. Built with mf_core's defaultPatch and the panel model, so every preset is a valid .mfp v4.
// node: require('./factory.js')(MFCore, MFPanel); page: MFFactory.build(MFCore, MFPanel).
'use strict';
(function (root, factory) {
  const m = factory();
  if (typeof module === 'object' && module.exports) module.exports = m;
  else root.MFFactory = m;
})(typeof self !== 'undefined' ? self : this, function () {
  // wave frames: 0 tri, 1 shark, 2 rev saw, 3 saw, 4 square, 5 wide, 6 narrow, 7 narrowest
  function build(Core, M) {
    const S = M.SRC, D = M.DST, T = M.secToEnv;
    function base(name) {
      const p = Core.defaultPatch();
      M.setName(p, name);
      p.filt[0].type = M.FILTER_ENUM.LADDER;
      for (const o of p.osc) o.drift = 24;
      return p;
    }
    // osc(p, n, {range, wave, freq, vol, on})  range index: 0 LO 1 32' 2 16' 3 8' 4 4' 5 2'
    function osc(p, n, o) {
      if (o.range !== undefined) M.set(p, `osc${n}.range`, o.range);
      if (o.wave !== undefined) M.set(p, `osc${n}.wave`, o.wave);
      if (o.freq !== undefined) M.set(p, `osc${n}.freq`, o.freq);
      M.set(p, `osc${n}.vol`, o.vol === undefined ? 0.8 : o.vol);
      M.set(p, `osc${n}.on`, o.on === undefined ? 1 : o.on);
    }
    function env(p, n, a, d, s, r) {     // seconds, level 0..1
      const e = p.env[n - 1];
      e.attack = M.q15(T(a)); e.decay = M.q15(T(d)); e.sustain = M.q15(s); e.release = M.q15(T(r));
    }
    function filt(p, type, hz, res, contour, kbd) {
      p.filt[0].type = M.FILTER_ENUM[type];
      p.filt[0].cutoff = M.q15(M.hzToCutoff(hz));
      p.filt[0].res = M.q15(res);
      M.setRoute(p, S.ENV1, D.F1_CUTOFF, contour);
      p.filt[0].keytrack = M.q15(kbd || 0);
    }
    // preset effects written with the classic names, stored in the v5 chain: delay -> BBD ECHO,
    // chorus -> ENSEMBLE I, reverb -> TWIN TANK (one AMOUNT knob: mix and size together), drive -> TAPE
    const V5 = { delay_mix: 'bbd_mix', delay_time: 'bbd_time', delay_feedback: 'bbd_feedback', chorus_mix: 'ens_mix',
      chorus_rate: 'ens_rate', chorus_depth: 'ens_depth', drive: 'tape_drive' };
    function fx(p, o) {
      for (const k of Object.keys(o)) {
        if (V5[k]) p.fx[V5[k]] = M.q15(o[k]);
        else if (k in p.fx) p.fx[k] = M.q15(o[k]);
      }
      if (o.chorus_mix) p.fx.ens_mode = 1;
      if (o.reverb_mix) p.fx.rev_amount = M.q15(Math.min(1, o.reverb_mix * 0.6 + (o.reverb_size || 0.5) * 0.4));
    }
    const bank = [];
    const add = (p) => { bank.push(p); return p; };

    // 1 bass: two saws an octave apart through a dark ladder, punchy contour
    let p = add(base('Walnut Bass'));
    p.voice_mode = 1; p.glide = M.q15(0.18);
    osc(p, 1, { range: 2, wave: 3, vol: 0.85 }); osc(p, 2, { range: 1, wave: 4, freq: 0.06, vol: 0.6 }); osc(p, 3, { range: 2, wave: 3, freq: -0.08, vol: 0.5 });
    filt(p, 'LADDER', 180, 0.28, 0.32, 0.3); env(p, 1, 0.002, 0.28, 0.15, 0.12); env(p, 4, 0.002, 0.6, 0.85, 0.09);
    M.setRoute(p, S.MODWHEEL, D.F1_CUTOFF, 0.35);

    // 2 lead: saw + square, legato with glide, singing emphasis, vibrato on the wheel via LFO rate
    p = add(base('Copper Lead'));
    p.voice_mode = 2; p.glide = M.q15(0.32);
    osc(p, 1, { range: 3, wave: 3, vol: 0.8 }); osc(p, 2, { range: 3, wave: 4, freq: 0.07, vol: 0.65 }); osc(p, 3, { range: 4, wave: 3, freq: -0.05, vol: 0.35 });
    filt(p, 'LADDER', 900, 0.45, 0.28, 0.5); env(p, 1, 0.01, 0.5, 0.45, 0.3); env(p, 4, 0.006, 0.4, 0.9, 0.25);
    p.lfo[0].rate = M.q15(M.hzToLfo(5.5)); M.setRoute(p, S.LFO1, D.PITCH, 0.0035);
    M.setRoute(p, S.MODWHEEL, D.PITCH, 0.0);
    fx(p, { delay_mix: 0.18, delay_time: 0.75, delay_feedback: 0.3, reverb_mix: 0.15, reverb_size: 0.5, reverb_damp: 0.5 });

    // 3 brass: three detuned saws, slow-ish filter swell
    p = add(base('Brass Section'));
    osc(p, 1, { range: 3, wave: 3, vol: 0.7 }); osc(p, 2, { range: 3, wave: 3, freq: 0.1, vol: 0.7 }); osc(p, 3, { range: 3, wave: 3, freq: -0.12, vol: 0.7 });
    filt(p, 'LP4', 420, 0.12, 0.36, 0.4); env(p, 1, 0.09, 0.6, 0.55, 0.3); env(p, 4, 0.04, 0.5, 0.85, 0.3);
    M.setRoute(p, S.VEL, D.F1_CUTOFF, 0.12);
    fx(p, { chorus_mix: 0.25, chorus_rate: 0.3, chorus_depth: 0.4, reverb_mix: 0.2, reverb_size: 0.6, reverb_damp: 0.4 });

    // 4 pad: shark + triangle, slow attack, wide chorus and long reverb
    p = add(base('Velvet Pad'));
    osc(p, 1, { range: 3, wave: 1, vol: 0.7 }); osc(p, 2, { range: 2, wave: 0, freq: 0.08, vol: 0.6 }); osc(p, 3, { range: 3, wave: 5, freq: -0.1, vol: 0.45 });
    filt(p, 'LP4', 1400, 0.1, 0.12, 0.3); env(p, 1, 1.2, 2.5, 0.6, 2.5); env(p, 4, 0.9, 1.5, 0.9, 2.4);
    p.lfo[1].rate = M.q15(M.hzToLfo(0.25)); M.setRoute(p, S.LFO2, D.OSC3_POS, 0.12);
    fx(p, { chorus_mix: 0.55, chorus_rate: 0.22, chorus_depth: 0.7, reverb_mix: 0.42, reverb_size: 0.85, reverb_damp: 0.45 });

    // 5 pluck: narrow pulse, fast filter decay, echo
    p = add(base('Glass Pluck'));
    osc(p, 1, { range: 3, wave: 6, vol: 0.8 }); osc(p, 2, { range: 4, wave: 0, vol: 0.4 });
    filt(p, 'LP2', 300, 0.3, 0.5, 0.6); env(p, 1, 0.001, 0.22, 0.0, 0.2); env(p, 4, 0.001, 0.9, 0.0, 0.5);
    fx(p, { delay_mix: 0.3, delay_time: 0.56, delay_feedback: 0.45, reverb_mix: 0.2, reverb_size: 0.6, reverb_damp: 0.3 });

    // 6 acid: one saw through the diode-ladder filter, high emphasis, snappy contour, mono slide
    p = add(base('Acid Line'));
    p.voice_mode = 2; p.glide = M.q15(0.2);
    osc(p, 1, { range: 2, wave: 3, vol: 0.9 });
    filt(p, 'ACID', 260, 0.78, 0.4, 0.4); env(p, 1, 0.001, 0.18, 0.0, 0.08); env(p, 4, 0.001, 0.3, 0.7, 0.05);
    p.filt[0].drive = M.q15(0.35);
    M.setRoute(p, S.VEL, D.F1_CUTOFF, 0.15); M.setRoute(p, S.MODWHEEL, D.F1_RES, 0.2);
    fx(p, { delay_mix: 0.2, delay_time: 0.56, delay_feedback: 0.4 });

    // 7 sync-ish lead: the SYNC wavetable swept by ENV 2 (hard-sync sweep sound)
    p = add(base('Sync Sweeper'));
    p.voice_mode = 2; p.glide = M.q15(0.12);
    osc(p, 1, { range: 3, wave: 3, vol: 0.8 }); p.osc[0].table = 2; p.osc[0].position = M.q15(0.15);
    osc(p, 2, { range: 2, wave: 4, vol: 0.45 });
    filt(p, 'LADDER', 3200, 0.2, 0.1, 0.5); env(p, 1, 0.005, 0.4, 0.6, 0.3); env(p, 4, 0.003, 0.4, 0.9, 0.2);
    env(p, 2, 0.002, 0.7, 0.2, 0.4); M.setRoute(p, S.ENV2, D.OSC1_POS, 0.7);
    M.setRoute(p, S.MODWHEEL, D.OSC1_POS, 0.5);
    fx(p, { delay_mix: 0.2, delay_time: 0.75, delay_feedback: 0.3, reverb_mix: 0.12, reverb_size: 0.5, reverb_damp: 0.5 });

    // 8 wobble: Performer in slot 1 on the cutoff, tempo-synced eighths
    p = add(base('Wobble Engine'));
    p.voice_mode = 1;
    osc(p, 1, { range: 2, wave: 3, vol: 0.8 }); osc(p, 2, { range: 2, wave: 4, freq: 0.05, vol: 0.6 }); osc(p, 3, { range: 1, wave: 0, vol: 0.6 });
    filt(p, 'LADDER', 110, 0.55, 0.0, 0.2); env(p, 4, 0.002, 0.4, 1.0, 0.12);
    p.filt[0].drive = M.q15(0.4);
    { const l = p.lfo[0]; l.wave = M.W.PERFORMER; l.flags = M.LFO_SYNC | M.LFO_KEYSYNC; l.sync_div = 5;
      const sh = [11, 11, 10, 3, 11, 9, 11, 26, 11, 11, 10, 3, 2, 2, 12, 27];
      const lv = [127, 100, 127, 90, 127, 70, 127, 110, 127, 100, 127, 90, 110, 120, 127, 100];
      for (let s = 0; s < 16; s++) { l.step_shape[s] = sh[s]; l.step_level[s] = lv[s]; } }
    M.setRoute(p, S.LFO1, D.F1_CUTOFF, 0.42); M.setRoute(p, S.MODWHEEL, D.LFO1_RATE, 0.3);

    // 9 arp-like Stepper: slot 2 steps the pitch (5 levels per semitone: amount 127/320)
    p = add(base('Step Arpeggio'));
    osc(p, 1, { range: 3, wave: 5, vol: 0.75 }); osc(p, 2, { range: 4, wave: 3, freq: 0.04, vol: 0.4 });
    filt(p, 'LP4', 1200, 0.35, 0.3, 0.5); env(p, 1, 0.001, 0.16, 0.2, 0.1); env(p, 4, 0.002, 0.3, 0.8, 0.2);
    { const l = p.lfo[1]; l.wave = M.W.STEPPER; l.flags = M.LFO_SYNC | M.LFO_KEYSYNC; l.sync_div = 2; l.glide = 0;
      const semis = [0, 7, 12, 15, 0, 7, 12, 19, 0, 7, 12, 15, 24, 19, 15, 12];
      for (let s = 0; s < 16; s++) { l.step_level[s] = semis[s] * 5; l.step_shape[s] = 0; } }
    M.setRoute(p, S.LFO2, D.PITCH, 127 / 320);
    { const l = p.lfo[2]; l.wave = M.W.PERFORMER; l.flags = M.LFO_SYNC | M.LFO_KEYSYNC | M.LFO_UNIPOLAR; l.sync_div = 2;
      for (let s = 0; s < 16; s++) { l.step_shape[s] = 12; l.step_level[s] = 127; } }
    M.setRoute(p, S.LFO3, D.F1_CUTOFF, 0.18);
    fx(p, { delay_mix: 0.25, delay_time: 0.75, delay_feedback: 0.35, reverb_mix: 0.18, reverb_size: 0.6, reverb_damp: 0.4 });

    // 10 organ-ish keys: square + octave triangle, full sustain, gentle vibrato
    p = add(base('Parlour Keys'));
    osc(p, 1, { range: 3, wave: 4, vol: 0.6 }); osc(p, 2, { range: 4, wave: 0, vol: 0.5 }); osc(p, 3, { range: 2, wave: 0, vol: 0.5 });
    filt(p, 'LP2', 2600, 0.05, 0.0, 0.5); env(p, 4, 0.004, 0.3, 1.0, 0.08);
    p.lfo[0].rate = M.q15(M.hzToLfo(6.2)); M.setRoute(p, S.LFO1, D.PITCH, 0.0025);
    fx(p, { chorus_mix: 0.4, chorus_rate: 0.38, chorus_depth: 0.8, reverb_mix: 0.2, reverb_size: 0.45, reverb_damp: 0.5 });

    // 11 bell: the mod osc ring-modulates oscillator 1 at a non-harmonic ratio
    p = add(base('Iron Bell'));
    osc(p, 1, { range: 3, wave: 0, vol: 0.8 }); osc(p, 2, { range: 5, wave: 0, freq: 0.15, vol: 0.25 });
    filt(p, 'LP2', 6000, 0.0, 0.1, 0.6); env(p, 1, 0.001, 1.0, 0.3, 1.0); env(p, 4, 0.001, 2.6, 0.0, 2.2);
    p.modosc.semis = 19; p.modosc.rm_osc = 1; p.modosc.rm = M.q15(0.8);
    M.setRoute(p, S.MODWHEEL, D.OSC1_PITCH, 0.0);
    fx(p, { reverb_mix: 0.35, reverb_size: 0.8, reverb_damp: 0.25 });

    // 12 kick-ish drum: triangle with a fast pitch envelope (ENV 2 -> PITCH)
    p = add(base('Thunder Kick'));
    p.voice_mode = 1;
    osc(p, 1, { range: 1, wave: 0, vol: 1.0 }); p.osc[0].flags |= M.OSC_RESET; p.osc[0].drift = 0;
    filt(p, 'LP2', 4000, 0.0, 0.0, 0.0); env(p, 4, 0.001, 0.35, 0.0, 0.3);
    env(p, 2, 0.001, 0.05, 0.0, 0.05); M.setRoute(p, S.ENV2, D.PITCH, 0.6);
    p.fx.tape_drive = M.q15(0.3);

    // 13 rubbery funk bass: square + narrow pulse, ladder, contour with velocity
    p = add(base('Rubber Funk'));
    p.voice_mode = 1;
    osc(p, 1, { range: 2, wave: 4, vol: 0.8 }); osc(p, 2, { range: 2, wave: 6, freq: -0.04, vol: 0.5 });
    filt(p, 'LADDER', 140, 0.62, 0.4, 0.3); env(p, 1, 0.001, 0.14, 0.05, 0.08); env(p, 4, 0.001, 0.4, 0.6, 0.06);
    M.setRoute(p, S.VEL, D.F1_CUTOFF, 0.2);

    // 14 PWM strings: pulse frames swept by slot 2, slow attack, ensemble
    p = add(base('Silk Strings'));
    osc(p, 1, { range: 3, wave: 5, vol: 0.7 }); osc(p, 2, { range: 3, wave: 5, freq: 0.09, vol: 0.7 }); osc(p, 3, { range: 2, wave: 3, freq: -0.07, vol: 0.35 });
    filt(p, 'LP4', 2400, 0.08, 0.05, 0.5); env(p, 1, 0.5, 1.0, 0.8, 1.2); env(p, 4, 0.45, 1.0, 0.95, 1.4);
    p.lfo[1].rate = M.q15(M.hzToLfo(0.6)); p.lfo[1].wave = M.W.TRI; M.setRoute(p, S.LFO2, D.OSC1_POS, 0.12); M.setRoute(p, S.LFO2, D.OSC2_POS, -0.1);
    fx(p, { chorus_mix: 0.6, chorus_rate: 0.3, chorus_depth: 0.65, reverb_mix: 0.35, reverb_size: 0.75, reverb_damp: 0.5 });

    // 15 noise wind: noise through a resonant band-pass swept by a slow LFO
    p = add(base('Night Wind'));
    osc(p, 1, { range: 3, wave: 0, vol: 0.0, on: 0 });
    p.noise.amp = M.q15(0.9); p.noise.color = M.q15(0.3);
    filt(p, 'BP', 800, 0.7, 0.0, 0.0); env(p, 4, 1.5, 1.0, 1.0, 3.0);
    p.lfo[0].rate = M.q15(M.hzToLfo(0.12)); M.setRoute(p, S.LFO1, D.F1_CUTOFF, 0.18);
    p.lfo[1].rate = M.q15(M.hzToLfo(0.31)); p.lfo[1].wave = M.W.TRI; M.setRoute(p, S.LFO2, D.F1_RES, 0.15);
    fx(p, { reverb_mix: 0.5, reverb_size: 0.9, reverb_damp: 0.3 });

    // 16 three-osc unison stab with a high-pass for chords
    p = add(base('Hollow Stab'));
    osc(p, 1, { range: 3, wave: 3, vol: 0.7 }); osc(p, 2, { range: 3, wave: 3, freq: 7, vol: 0.55 }); osc(p, 3, { range: 4, wave: 4, vol: 0.4 });
    filt(p, 'LADDER', 700, 0.35, 0.45, 0.5); env(p, 1, 0.001, 0.25, 0.0, 0.2); env(p, 4, 0.001, 0.45, 0.0, 0.35);
    fx(p, { delay_mix: 0.25, delay_time: 0.62, delay_feedback: 0.4, reverb_mix: 0.25, reverb_size: 0.7, reverb_damp: 0.4 });

    // output levels, set by rendering each preset (chords of three voices) to peak below about -1 dBFS
    const LEVEL = { 'Brass Section': 0.42, 'Velvet Pad': 0.38, 'Acid Line': 0.6, 'Step Arpeggio': 0.5, 'Parlour Keys': 0.4,
      'Iron Bell': 0.75, 'Silk Strings': 0.45, 'Night Wind': 0.55, 'Hollow Stab': 0.5, 'Glass Pluck': 0.95 };
    for (const b of bank) if (LEVEL[b.name]) b.master = Math.round(8192 * LEVEL[b.name]);
    return bank;
  }
  return { build };
});
