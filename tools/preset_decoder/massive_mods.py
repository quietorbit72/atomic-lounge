# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: decode Massive's assignable modulator slots 5-8 ("LFO 5-8" tabs: LFO, Performer or
# Stepper) from the inner synth state (state versions 22, 23, 24; the bytes massive.py exports as
# state.raw_b64) into the Vintage Chonk engine's LFO model (dsp/include/mfe.h, mfe_lfo_t).
#
# Byte layout and every measurement behind it: docs/MASSIVE_MODULATORS.md. Found 16-17 Nov 2026 by
# writing candidate bytes into our own state for the installed Massive 1.7 (headless, pedalboard)
# and measuring the slot's output as OSC 1 pitch. No NI data is stored here: only format facts and
# our own fits.
#
#   from massive_mods import decode_mod_slots
#   slots = decode_mod_slots(state_bytes)      # list of 4 dicts (slot 5..8) or raises ModSlotError
#
#   python -I massive_mods.py FILE.nmsv        # prints the decoded slots as JSON
import json
import math
import struct
import sys

STEPS = 16
FLOAT_BASE = 4                    # floats start after the u32 state version

# float indices of the slot knobs (maps/massive_state_v24.json, verified, fit error 0); slot i = 0..3
F_RATE, F_AMP, F_XFADE_GLIDE, F_IND_LEV, F_SYNC, F_RESTART = 58, 62, 66, 70, 74, 78

KEY_GLIDE_TIME = 126              # the modulator block follows this knob's record
KEY_RATE = 58                     # slot i's rate knob record holds pos_sync / num / den (keys 58..61)
PRE_BYTES = 17                    # bytes between the knob-126 record and slot 5 (meaning unknown)
TYPE_LFO, TYPE_PERFORMER, TYPE_STEPPER = 0, 1, 2
KIND = {TYPE_LFO: "lfo", TYPE_PERFORMER: "performer", TYPE_STEPPER: "stepper"}
SIZE = {TYPE_LFO: 24, TYPE_PERFORMER: 336, TYPE_STEPPER: 144}   # bytes including the u32 type
DEN = [1, 2, 4, 8, 12, 16, 24, 32]                               # den byte -> denominator (verified)

# MFE enums (dsp/include/mfe.h)
W_SINE, W_TRI, W_SAW_UP, W_SAW_DOWN, W_SQUARE, W_SH, W_PERFORMER, W_STEPPER = range(8)
WAVE_NAME = ["SINE", "TRI", "SAW_UP", "SAW_DOWN", "SQUARE", "SH", "PERFORMER", "STEPPER"]
LFO_KEYSYNC, LFO_UNIPOLAR, LFO_SYNC = 1, 2, 4
MFE_SYNC_BEATS = [1 / 8, 1 / 6, 1 / 4, 1 / 3, 3 / 8, 1 / 2, 2 / 3, 3 / 4, 1, 3 / 2, 2, 4, 8]   # mfe_sync_beats()

# Massive LFO curve index (0-34) -> nearest MFE wave: (wave, phase in cycles, unipolar, rms error).
# 0-3 measured exactly (sine, saw down, square, triangle starting at its minimum); 4-34 are curves of
# Massive's LFO library, fitted by least squares to one measured cycle (our fit, not NI data).
LFO_FIT = {
    0: (W_SINE, 0.0, False, 0.03), 1: (W_SAW_DOWN, 0.0, False, 0.0), 2: (W_SQUARE, 0.0, False, 0.0),
    3: (W_TRI, 0.75, False, 0.03), 4: (W_SAW_DOWN, 0.0, False, 0.66), 5: (W_SINE, 0.0, False, 0.34),
    6: (W_SAW_DOWN, 0.925, False, 0.24), 7: (W_SAW_DOWN, 0.925, False, 0.44),
    8: (W_SAW_DOWN, 0.925, False, 0.50), 9: (W_SAW_UP, 0.3, False, 0.71), 10: (W_SAW_UP, 0.075, False, 0.79),
    11: (W_SAW_DOWN, 0.875, False, 0.59), 12: (W_SAW_UP, 0.4, False, 0.80), 13: (W_SINE, 0.75, False, 0.28),
    14: (W_TRI, 0.675, False, 0.43), 15: (W_TRI, 0.8, False, 0.45), 16: (W_SAW_UP, 0.05, False, 0.45),
    17: (W_SINE, 0.175, False, 0.39), 18: (W_SAW_UP, 0.65, False, 0.57), 19: (W_SAW_UP, 0.45, False, 0.56),
    20: (W_TRI, 0.8, False, 0.34), 21: (W_SAW_DOWN, 0.75, False, 0.63), 22: (W_SAW_UP, 0.725, False, 0.61),
    23: (W_SAW_DOWN, 0.0, False, 0.64), 24: (W_SAW_DOWN, 0.0, False, 0.88), 25: (W_SAW_DOWN, 0.0, False, 0.59),
    26: (W_SAW_DOWN, 0.0, False, 0.66), 27: (W_SQUARE, 0.95, True, 0.14), 28: (W_TRI, 0.05, False, 0.31),
    29: (W_SAW_UP, 0.25, True, 0.51), 30: (W_SQUARE, 0.0, True, 0.57), 31: (W_SAW_UP, 0.075, True, 0.10),
    32: (W_SINE, 0.75, True, 0.02), 33: (W_TRI, 0.75, True, 0.02), 34: (W_TRI, 0.75, True, 0.24),
}
LFO_CURVES = 35                   # indices 35 and above play as a sine in Massive 1.7 (measured)
PERF_SHAPES = 16                  # Performer curve 16 and above play as curve 1 (ramp down) (measured)
# Performer curve index -> MFE step_shape. Massive's index order is our 0-15 order (measured);
# differences: Massive 3 is an exponential fall to 0 (MFE 3 "VALLEY" falls and rises again) and
# Massive 8 holds 1.0 for the whole step (MFE 8 "HOLD DROP" drops at 85 %). Kept 1:1 until dsp/ changes.
PERF_TO_MFE = list(range(16))

# Stepper glide knob -> time constant (s, time to 63 %), measured; independent of the step rate.
GLIDE_TAU = [(0.0, 0.0), (0.25, 0.002), (0.5, 0.055), (0.625, 0.12), (0.75, 0.235), (0.875, 0.30), (1.0, 0.31)]
# LFO 5 rate knob -> Hz (docs/PROGRESS.md, measured); one Performer/Stepper step per cycle (measured)
RATE_HZ = [(0.3, 0.2), (0.5, 1.0), (0.7, 5.4), (0.9, 28.0), (1.0, 41.0)]
HOST_BPM = 120.0                  # headless pedalboard host tempo (measured: 1/4 = 0.5 s)


class ModSlotError(ValueError):
    pass


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def find_knob_record(state: bytes, key: int, start: int = 4) -> int:
    """Offset of the data that follows knob `key`'s record:
    u32 type | f32 lo | f32 hi | u32 n | n x (f32 depth, i32 source, u16) | u32 key | u8 0 | data."""
    pat = struct.pack("<IB", key, 0)
    i = state.find(pat, start)
    while i >= 0:
        for n in range(1, 9):
            c = i - 10 * n - 4
            if c < 12 or _u32(state, c) != n:
                continue
            ok = True
            for k in range(n):
                d, s, u = struct.unpack_from("<fiH", state, c + 4 + 10 * k)
                if not (-1.0001 <= d <= 1.0001 and -1 <= s <= 64 and u in (0, 256, 512)):
                    ok = False
                    break
            if ok:
                return i + 5
        i = state.find(pat, i + 1)
    raise ModSlotError(f"no record for knob {key}")


def _interp(table, x):
    if x <= table[0][0]:
        return table[0][1]
    for (x0, y0), (x1, y1) in zip(table, table[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return table[-1][1]


def rate_hz(knob):
    """Free-running LFO rate (Hz) from the knob, log-interpolated in the measured LFO 5 curve."""
    pts = [(k, math.log(h)) for k, h in RATE_HZ]
    if knob < pts[0][0]:     # below 0.3: extend the 0.3-0.5 slope
        (k0, l0), (k1, l1) = pts[0], pts[1]
        return math.exp(l0 + (l1 - l0) * (knob - k0) / (k1 - k0))
    return math.exp(_interp(pts, knob))


def nearest_sync_div(beats):
    """Index into mfe_sync_beats() closest (in log time) to `beats` per cycle; and the exact flag."""
    best = min(range(len(MFE_SYNC_BEATS)), key=lambda d: abs(math.log(MFE_SYNC_BEATS[d] / beats)))
    return best, abs(MFE_SYNC_BEATS[best] - beats) < 1e-9


def _parse_lfo(b, o):
    cnt, ph1, ph2, c1, c2 = struct.unpack_from("<IffII", b, o + 4)
    return {"curve_count": cnt, "phase_1": ph1, "phase_2": ph2, "curve_1": c1, "curve_2": c2}


def _flags(b, o):
    return [tuple(b[o + 3 * k:o + 3 * k + 3]) for k in range(STEPS)]


def _parse_performer(b, o):
    hdr = _u32(b, o + 4)
    steps = []
    for k in range(STEPS):
        la, sa, lb, sb, use_b = struct.unpack_from("<fIfIB", b, o + 8 + 17 * k)
        steps.append({"level_1": la, "curve_1": sa, "level_2": lb, "curve_2": sb, "own_row_2": use_b})
    q = o + 8 + 17 * STEPS
    return {"header": hdr, "steps": steps, "row_count": _u32(b, q), "step_flags": _flags(b, q + 4),
            "after_flags": _u32(b, q + 4 + 48)}


def _parse_stepper(b, o):
    hdr = _u32(b, o + 4)
    vals = list(struct.unpack_from("<16f", b, o + 8))
    glide = list(b[o + 72:o + 88])
    q = o + 88
    return {"header": hdr, "values": vals, "glide_steps": glide, "row_count": _u32(b, q),
            "step_flags": _flags(b, q + 4), "after_flags": _u32(b, q + 4 + 48)}


def parse_raw(state: bytes):
    """The four slot records as Massive stores them (no conversion) plus the sync bytes."""
    if len(state) < 4 + 4 * 137:
        raise ModSlotError("state too short")
    ver = _u32(state, 0)
    if ver not in (22, 23, 24):
        raise ModSlotError(f"state version {ver} not supported (22-24 decoded)")
    p = find_knob_record(state, KEY_GLIDE_TIME, 4 + 4 * 137) + PRE_BYTES
    raw = []
    for i in range(4):
        t = _u32(state, p)
        if t not in KIND:
            raise ModSlotError(f"slot {i + 5}: type {t} at offset {p} (expected 0, 1 or 2)")
        rec = {TYPE_LFO: _parse_lfo, TYPE_PERFORMER: _parse_performer, TYPE_STEPPER: _parse_stepper}[t](state, p)
        rec.update({"type": t, "kind": KIND[t], "offset": p})
        raw.append(rec)
        p += SIZE[t]
    if _u32(state, p) != 3:
        raise ModSlotError(f"no end marker (u32 3) after slot 8 at offset {p}")
    # slot i's rate-knob record data: v23/24 u8 a | u8 pos_sync | u8 b | u32 num | u32 den index;
    # v22 has one byte less before num (u8 a | u8 pos_sync? | u32 num | u32 den index)
    n_pre = 2 if ver == 22 else 3
    for i in range(4):
        d = find_knob_record(state, KEY_RATE + i, 4 + 4 * 137)
        raw[i].update({"pos_sync": state[d + 1], "sync_num": _u32(state, d + n_pre),
                       "sync_den_index": _u32(state, d + n_pre + 4), "sync_bytes_offset": d,
                       "rate_record_bytes": state[d:d + n_pre].hex()})
    return raw


def _q15(x):
    return int(max(-32768, min(32767, round(x * 32767))))


def _knobs(state, i):
    f = lambda idx: _f32(state, FLOAT_BASE + 4 * (idx + i))   # noqa: E731
    return {"rate": f(F_RATE), "amp": f(F_AMP), "xfade_glide": f(F_XFADE_GLIDE), "ind_lev": f(F_IND_LEV),
            "sync": f(F_SYNC), "restart": f(F_RESTART)}


def _step_gain(flag, ind_lev):
    """Per-step flags (x, on, z): the step plays only if on != 0 and z == 0; x = 1 scales it by Ind Lev."""
    x, on, z = flag
    if not on or z:
        return 0.0
    return ind_lev if x else 1.0


def decode_mod_slots(state: bytes):
    """Inner synth state -> 4 dicts (Massive slots 5-8 = engine LFO 1-4 = mod sources 4-7), each:
      slot, source, kind ('lfo' | 'performer' | 'stepper'),
      mfe:    fields of mfe_lfo_t (wave, flags, rate_knob, phase (Q15), sync_div, loop_start, loop_end,
              glide (Q15 of a step), step_shape[16], step_level[16] (-127..127)),
      output_gain: multiply the slot's modulation amounts by this (Massive's Amp knob, measured amp^2),
      massive: the raw decoded fields, notes: list of approximations,
      confidence: {field: 'verified' | 'measured' | 'approximate' | 'hypothesis' | 'unknown'}."""
    raw = parse_raw(state)
    out = []
    for i, r in enumerate(raw):
        kn = _knobs(state, i)
        notes = []
        conf = {"kind": "verified", "output_gain": "verified", "rate_knob": "verified"}
        num = r["sync_num"]
        di = r["sync_den_index"]
        den = DEN[di] if di < len(DEN) else None
        m = {"wave": W_SINE, "flags": 0, "rate_knob": kn["rate"], "rate_hz": rate_hz(kn["rate"]), "phase": 0,
             "sync_div": 8, "loop_start": 0, "loop_end": STEPS - 1, "glide": 0,
             "step_shape": [0] * STEPS, "step_level": [127] * STEPS}
        if kn["sync"] >= 0.5:
            m["flags"] |= LFO_SYNC
            if den:
                beats = 4.0 * num / den
                m["sync_div"], exact = nearest_sync_div(beats)
                m["sync_beats"] = beats
                conf["sync_div"] = "verified" if exact else "approximate"
                if not exact:
                    notes.append(f"sync {num}/{den} ({beats:g} beats) -> nearest engine division "
                                 f"{MFE_SYNC_BEATS[m['sync_div']]:g} beats")
            else:
                conf["sync_div"] = "unknown"
                notes.append(f"sync denominator index {di} unknown")
        if kn["restart"] >= 0.5:
            m["flags"] |= LFO_KEYSYNC
        conf["flags.keysync"] = "verified"             # Restart on: phase 0 at each note (measured)
        # Restart off: free-running; Pos Sync changes that free-running phase (measured), most likely by
        # locking it to the host song position (no transport in the headless host: not verified)
        conf["pos_sync"] = "hypothesis"
        if kn["restart"] < 0.5 and r["pos_sync"]:
            notes.append("Restart off with Pos Sync on: phase follows the host position in Massive (assumed); "
                         "the engine free-runs")
        xf = kn["xfade_glide"]
        if r["type"] == TYPE_LFO:
            row1 = xf >= 0.5
            curve = r["curve_1"] if row1 else r["curve_2"]
            ph = r["phase_1"] if row1 else r["phase_2"]
            if curve >= LFO_CURVES:
                curve = 0
            wave, fph, uni, err = LFO_FIT[curve]
            m["wave"] = wave
            m["phase"] = _q15(math.fmod(ph + fph, 1.0))
            if uni:
                m["flags"] |= LFO_UNIPOLAR
            conf["wave"] = "verified" if curve <= 3 else ("approximate" if err < 0.35 else "poor")
            conf["phase"] = "verified"
            conf["xfade"] = "verified"                 # output = xf * curve 1 + (1 - xf) * curve 2 (measured)
            if 0.0 < xf < 1.0 and r["curve_1"] != r["curve_2"]:
                notes.append(f"XFade {xf:.2f} blends curves {r['curve_1']} and {r['curve_2']}; "
                             f"the engine plays curve {curve} only")
                conf["wave"] = "approximate"
            if curve > 3:
                notes.append(f"Massive LFO library curve {curve}: nearest engine wave {WAVE_NAME[wave]} "
                             f"(rms error {err:.2f} of full scale)")
        elif r["type"] == TYPE_PERFORMER:
            m["wave"] = W_PERFORMER
            m["flags"] |= LFO_UNIPOLAR
            for k, s in enumerate(r["steps"]):
                a = (s["level_1"], s["curve_1"])
                b = (s["level_2"], s["curve_2"]) if s["own_row_2"] else a
                lev, cv = (a if xf >= 0.5 else b)
                if a[1] == b[1]:
                    lev = xf * a[0] + (1 - xf) * b[0]          # measured: output = xf*row1 + (1-xf)*row2
                elif 0.0 < xf < 1.0:
                    notes.append(f"step {k + 1}: XFade {xf:.2f} between curves {a[1]} and {b[1]}; "
                                 f"curve {cv} kept")
                if cv >= PERF_SHAPES:
                    cv = 1
                g = _step_gain(r["step_flags"][k], kn["ind_lev"])
                m["step_shape"][k] = PERF_TO_MFE[cv]
                m["step_level"][k] = int(round(127 * max(-1.0, min(1.0, lev * g))))
            conf.update({"step_shape": "verified", "step_level": "verified", "loop": "verified"})
            if any(x in (3, 8) for x in (s["curve_1"] for s in r["steps"])):
                notes.append("curves 3/8 used: engine shapes 3 (VALLEY) / 8 (HOLD DROP) differ from Massive's "
                             "(exponential fall / full hold)")
                conf["step_shape"] = "approximate"
        else:
            m["wave"] = W_STEPPER
            m["flags"] |= LFO_UNIPOLAR
            for k, v in enumerate(r["values"]):
                g = _step_gain(r["step_flags"][k], kn["ind_lev"])
                m["step_level"][k] = int(round(127 * max(-1.0, min(1.0, v * g))))
            on = [k for k, gflag in enumerate(r["glide_steps"]) if gflag]
            tau = _interp(GLIDE_TAU, xf)
            m["glide_seconds"] = tau if on else 0.0
            step_s = (4.0 * num / den * 60.0 / HOST_BPM) if (m["flags"] & LFO_SYNC and den) else 1.0 / m["rate_hz"]
            m["glide"] = _q15(min(1.0, tau / step_s)) if on else 0
            conf.update({"step_level": "verified", "loop": "verified", "glide_steps": "verified",
                         "glide": "approximate"})
            if on and len(on) < STEPS:
                notes.append(f"glide only into steps {[k + 1 for k in on]} in Massive; the engine glides every step")
        out.append({"slot": i + 5, "source": 4 + i, "kind": r["kind"], "mfe": m,
                    "output_gain": kn["amp"] ** 2, "knobs": kn, "massive": r,
                    "confidence": conf, "notes": notes})
    return out


def main(argv=None):
    import base64
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import massive
    argv = argv if argv is not None else sys.argv[1:]
    d = massive.decode_file(argv[0])
    slots = decode_mod_slots(base64.b64decode(d["state"]["raw_b64"]))
    print(json.dumps(slots, indent=1, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
