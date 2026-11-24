# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: tests for tools/preset_decoder/massive_mods.py (Massive modulator slots 5-8).
# States are built here from the documented layout (docs/MASSIVE_MODULATORS.md); no NI data.
# the developer's own GUI saves in captures/ are used when present (skipped otherwise).
#   python -I tests/test_massive_mods.py
import base64
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "preset_decoder"))
import massive_mods as mm  # noqa: E402

CAPTURES = os.path.join(ROOT, "captures")


def knob_record(key, data=b"", n=1):
    """u32 type 4 | f32 lo | f32 hi | u32 n | n slots | u32 key | u8 0 | data"""
    slots = b"".join(struct.pack("<fiH", 0.0, -1, 0) for _ in range(n))
    return struct.pack("<IffI", 4, 0.0, 1.0, n) + slots + struct.pack("<IB", key, 0) + data


def lfo(curve1=0, curve2=1, ph1=0.0, ph2=0.0):
    return struct.pack("<IIffII", mm.TYPE_LFO, 2, ph1, ph2, curve1, curve2)


def performer(steps, flags=None):
    """steps: 16 x (level1, curve1, level2, curve2, own_row_2); flags: 16 x (x, on, z)"""
    b = struct.pack("<II", mm.TYPE_PERFORMER, 1)
    for s in steps:
        b += struct.pack("<fIfIB", *s)
    flags = flags or [(0, 1, 0)] * 16
    return b + struct.pack("<I", 2) + b"".join(bytes(f) for f in flags) + struct.pack("<I", 0)


def stepper(values, glide=None, flags=None):
    b = struct.pack("<II", mm.TYPE_STEPPER, 2) + struct.pack("<16f", *values)
    b += bytes(glide or [0] * 16)
    flags = flags or [(0, 1, 0)] * 16
    return b + struct.pack("<I", 2) + b"".join(bytes(f) for f in flags) + struct.pack("<I", 0)


def build_state(slots, knobs=None, sync=None, version=24):
    """slots: 4 slot records; knobs: {float index: value}; sync: 4 x (pos_sync, num, den_index)."""
    fl = [0.0] * 137
    for i in range(4):
        fl[mm.F_RATE + i], fl[mm.F_AMP + i], fl[mm.F_XFADE_GLIDE + i] = 0.5, 1.0, 1.0
        fl[mm.F_RESTART + i] = 1.0
    for k, v in (knobs or {}).items():
        fl[k] = v
    b = struct.pack("<I", version) + struct.pack("<137f", *fl) + bytes(64)
    b += knob_record(mm.KEY_GLIDE_TIME, bytes(mm.PRE_BYTES) + b"".join(slots) + struct.pack("<I", 3))
    sync = sync or [(1, 1, 5)] * 4
    for i, (ps, num, den) in enumerate(sync):
        pre = bytes([0, ps]) if version == 22 else bytes([0, ps, 1])
        b += knob_record(mm.KEY_RATE + i, pre + struct.pack("<II", num, den) + struct.pack("<I", 4))
    return b + bytes(32)


FLAT = [(1.0, 1, 1.0, 5, 1)] * 16


class ModSlotTests(unittest.TestCase):
    def test_types_and_sources(self):
        st = build_state([lfo(), lfo(), performer(FLAT), stepper([0.0] * 16)])
        out = mm.decode_mod_slots(st)
        self.assertEqual([s["kind"] for s in out], ["lfo", "lfo", "performer", "stepper"])
        self.assertEqual([s["source"] for s in out], [4, 5, 6, 7])
        self.assertEqual(out[2]["mfe"]["wave"], mm.W_PERFORMER)
        self.assertEqual(out[3]["mfe"]["wave"], mm.W_STEPPER)
        self.assertEqual(out[0]["mfe"]["flags"] & mm.LFO_KEYSYNC, mm.LFO_KEYSYNC)

    def test_lfo_curve_phase_and_xfade(self):
        st = build_state([lfo(3, 0), lfo(0, 2, ph2=0.5), lfo(1, 1, ph1=0.25), lfo(40, 0)],
                         knobs={mm.F_XFADE_GLIDE + 1: 0.0})
        a, b, c, d = mm.decode_mod_slots(st)
        self.assertEqual(a["mfe"]["wave"], mm.W_TRI)                 # Massive triangle starts at its minimum
        self.assertEqual(a["mfe"]["phase"], round(0.75 * 32767))
        self.assertEqual(b["mfe"]["wave"], mm.W_SQUARE)              # XFade 0 plays curve 2 with phase 2
        self.assertEqual(b["mfe"]["phase"], round(0.5 * 32767))
        self.assertEqual(c["mfe"]["wave"], mm.W_SAW_DOWN)
        self.assertEqual(c["mfe"]["phase"], round(0.25 * 32767))
        self.assertEqual(d["mfe"]["wave"], mm.W_SINE)                # curve 35+ plays as a sine
        self.assertEqual(a["confidence"]["wave"], "verified")

    def test_amp_is_squared(self):
        st = build_state([lfo()] * 4, knobs={mm.F_AMP: 0.5})
        self.assertAlmostEqual(mm.decode_mod_slots(st)[0]["output_gain"], 0.25)

    def test_sync_divisions(self):
        st = build_state([lfo()] * 4, knobs={mm.F_SYNC + i: 1.0 for i in range(4)},
                         sync=[(1, 1, 2), (1, 3, 3), (1, 1, 5), (1, 5, 7)])
        out = mm.decode_mod_slots(st)
        self.assertTrue(all(s["mfe"]["flags"] & mm.LFO_SYNC for s in out))
        self.assertEqual(out[0]["mfe"]["sync_beats"], 1.0)            # 1/4 = one beat
        self.assertEqual(mm.MFE_SYNC_BEATS[out[0]["mfe"]["sync_div"]], 1)
        self.assertEqual(mm.MFE_SYNC_BEATS[out[1]["mfe"]["sync_div"]], 1.5)   # 3/8
        self.assertEqual(mm.MFE_SYNC_BEATS[out[2]["mfe"]["sync_div"]], 0.25)  # 1/16
        self.assertEqual(out[3]["confidence"]["sync_div"], "approximate")     # 5/32 has no engine division

    def test_v22_sync_bytes(self):
        st = build_state([lfo()] * 4, knobs={mm.F_SYNC: 1.0}, sync=[(1, 3, 3)] * 4, version=22)
        self.assertEqual(mm.decode_mod_slots(st)[0]["massive"]["sync_num"], 3)
        self.assertEqual(mm.decode_mod_slots(st)[0]["mfe"]["sync_beats"], 1.5)

    def test_performer_steps(self):
        steps = [((k + 1) / 16, k % 16, 0.5, 9, 1) for k in range(16)]
        flags = [(0, 1, 0)] * 16
        flags[2], flags[3], flags[4] = (1, 1, 0), (0, 0, 0), (0, 1, 1)
        st = build_state([lfo(), lfo(), performer(steps, flags), lfo()], knobs={mm.F_IND_LEV + 2: 0.5})
        m = mm.decode_mod_slots(st)[2]["mfe"]
        self.assertEqual(m["step_shape"], list(range(16)))
        self.assertEqual(m["step_level"][0], round(127 / 16))
        self.assertEqual(m["step_level"][2], round(127 * 3 / 16 * 0.5))   # x flag: times Ind Lev
        self.assertEqual(m["step_level"][3], 0)                            # step off
        self.assertEqual(m["step_level"][4], 0)                            # z flag: silent (measured)
        self.assertEqual((m["loop_start"], m["loop_end"]), (0, 15))

    def test_performer_row_2_and_blend(self):
        steps = [(1.0, 1, 0.5, 1, 1)] * 8 + [(1.0, 1, 0.5, 9, 0)] * 8
        st = build_state([lfo(), lfo(), performer(steps), lfo()], knobs={mm.F_XFADE_GLIDE + 2: 0.0})
        m = mm.decode_mod_slots(st)[2]["mfe"]
        self.assertEqual(m["step_level"][0], round(127 * 0.5))       # row 2 at XFade 0
        self.assertEqual(m["step_level"][8], 127)                    # own_row_2 = 0: row 2 = row 1
        self.assertEqual(m["step_shape"][8], 1)
        st = build_state([lfo(), lfo(), performer(steps), lfo()], knobs={mm.F_XFADE_GLIDE + 2: 0.5})
        self.assertEqual(mm.decode_mod_slots(st)[2]["mfe"]["step_level"][0], round(127 * 0.75))

    def test_stepper_values_and_glide(self):
        vals = [k / 15 for k in range(16)]
        st = build_state([lfo(), lfo(), lfo(), stepper(vals)])
        m = mm.decode_mod_slots(st)[3]["mfe"]
        self.assertEqual(m["step_level"][15], 127)
        self.assertEqual(m["step_level"][0], 0)
        self.assertEqual(m["glide"], 0)
        st = build_state([lfo(), lfo(), lfo(), stepper(vals, glide=[1] * 16)], knobs={mm.F_XFADE_GLIDE + 3: 0.75})
        m = mm.decode_mod_slots(st)[3]["mfe"]
        self.assertAlmostEqual(m["glide_seconds"], 0.235)
        self.assertEqual(m["glide"], round(0.235 / mm.rate_hz(0.5) ** -1 * 32767))

    def test_rate_curve(self):
        self.assertAlmostEqual(mm.rate_hz(0.5), 1.0)
        self.assertAlmostEqual(mm.rate_hz(0.7), 5.4)
        self.assertTrue(mm.rate_hz(0.1) < 0.2)

    def test_bad_type_rejected(self):
        st = bytearray(build_state([lfo()] * 4))
        i = mm.find_knob_record(bytes(st), mm.KEY_GLIDE_TIME) + mm.PRE_BYTES
        st[i] = 7
        with self.assertRaises(mm.ModSlotError):
            mm.decode_mod_slots(bytes(st))
        with self.assertRaises(mm.ModSlotError):
            mm.decode_mod_slots(struct.pack("<I", 14) + bytes(2000))


@unittest.skipUnless(os.path.exists(os.path.join(CAPTURES, "modbase.nmsv")), "captures/ not present")
class CaptureTests(unittest.TestCase):
    def test_gui_saves(self):
        import massive
        for name in ("base.nmsv", "modbase.nmsv", "mod_lfo5_osc1wtpos__100.nmsv"):
            p = os.path.join(CAPTURES, name)
            if not os.path.exists(p):
                continue
            st = base64.b64decode(massive.decode_file(p)["state"]["raw_b64"])
            out = mm.decode_mod_slots(st)
            self.assertEqual([s["kind"] for s in out], ["lfo"] * 4, name)
            for s in out:
                self.assertEqual(s["massive"]["curve_count"], 2)
                self.assertTrue(1 <= s["massive"]["sync_num"] <= 32)
                self.assertTrue(s["massive"]["sync_den_index"] < 8)


if __name__ == "__main__":
    unittest.main(verbosity=1)
