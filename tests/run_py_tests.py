# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: parser, converter and integration tests.  python -I tests/run_py_tests.py
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "preset_decoder"))
sys.path.insert(0, os.path.join(ROOT, "tools", "preset_converter"))
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))
import fastlz  # noqa: E402
import nis  # noqa: E402
import massive  # noqa: E402
import nmsv_diff  # noqa: E402
import mfp  # noqa: E402
import convert  # noqa: E402
import make_fixtures  # noqa: E402

# A public init preset from the ni-file repository (no licence file: read in place, never copied)
NI_INIT = os.path.join(ROOT, "third_party", "ni-file", "tests", "data", "Containers", "NIS", "files",
                       "massive", "1.0.0.0", "000-new.nmsv")
NI_INIT_STATE = os.path.join(ROOT, "third_party", "ni-file", "tests", "data", "Presets", "Massive",
                             "1.0.0.0", "000-new")
RENDER = os.path.join(ROOT, "build", "mfe_render.exe")


class FastLZTests(unittest.TestCase):
    def test_literal_round_trip(self):
        for n in (0, 1, 31, 32, 33, 1000):
            data = bytes((i * 37) & 255 for i in range(n))
            self.assertEqual(fastlz.decompress(fastlz.compress_literal(data), n), data)

    def test_match(self):
        # literal "abc", then a match of length 6 at distance 3 -> "abcabcabc"
        stream = bytes([2]) + b"abc" + bytes([(6 - 2) << 5 | 0, 2])
        self.assertEqual(fastlz.decompress(stream), b"abcabcabc")

    def test_long_match_level1(self):
        stream = bytes([0]) + b"z" + bytes([7 << 5, 10, 0])     # 3 + 6 + 10 = 19 copies of z
        self.assertEqual(fastlz.decompress(stream), b"z" * 20)

    def test_errors(self):
        with self.assertRaises(fastlz.FastLZError):
            fastlz.decompress(bytes([5]) + b"ab")                   # literal past end
        with self.assertRaises(fastlz.FastLZError):
            fastlz.decompress(bytes([0]) + b"a" + bytes([0x20, 5]))   # reference before start
        with self.assertRaises(fastlz.FastLZError):
            fastlz.decompress(fastlz.compress_literal(b"x" * 50), 10)  # more than declared
        with self.assertRaises(fastlz.FastLZError):
            fastlz.decompress(bytes([0xE0]))                       # unknown level


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.fx = make_fixtures.nmsv_bytes("T", [0.5] * 21, ["1 A"] + [str(i) for i in range(2, 9)])

    def test_fixture_tree(self):
        root = nis.parse(self.fx)
        names = [it.type_name for _, it in root.walk()]
        self.assertEqual(names, ["RepositoryRoot", "Preset", "SoundInfoItem", "EncryptionItem", "Item",
                                 "PresetChunkItem"])

    def test_uncompressed_subtree(self):
        b = make_fixtures.nmsv_bytes("T", [0.25] * 21, ["1"], compress=False)
        d = massive.decode_bytes(b)
        self.assertEqual(d["state"]["float_block"]["values"][:3], [0.25] * 3)

    def test_truncated_and_corrupt(self):
        for cut in (10, 40, 100, len(self.fx) - 1):
            with self.assertRaises(nis.NISError):
                nis.parse(self.fx[:cut])
        bad = bytearray(self.fx); bad[12:16] = b"xxxx"
        with self.assertRaises(nis.NISError):
            nis.parse(bytes(bad))
        forged = bytearray(self.fx); struct.pack_into("<Q", forged, 0, 10 ** 12)
        with self.assertRaises(nis.NISError):
            nis.parse(bytes(forged))

    def test_fuzz_never_crashes(self):
        import random
        rnd = random.Random(1)
        for _ in range(300):
            b = bytearray(self.fx)
            for _ in range(rnd.randint(1, 8)):
                b[rnd.randrange(len(b))] = rnd.randrange(256)
            try:
                massive.decode_bytes(bytes(b))
            except (nis.NISError, fastlz.FastLZError, struct.error, UnicodeDecodeError, ValueError):
                pass


class MassiveTests(unittest.TestCase):
    def test_fixture_decode(self):
        floats = [i / 20 for i in range(21)]
        d = massive.decode_bytes(make_fixtures.nmsv_bytes("NAME X", floats, ["1 Cut", "2", "3", "4", "5", "6", "7", "8"]))
        self.assertEqual(d["metadata"]["name"], "NAME X")
        self.assertEqual(d["metadata"]["app"], "Massive")
        self.assertEqual(d["state"]["version"], 9001)
        self.assertEqual(len(d["state"]["float_block"]["values"]), 21)
        for a, b in zip(d["state"]["float_block"]["values"], floats):
            self.assertAlmostEqual(a, b, places=6)
        self.assertEqual([m["text"] for m in d["state"]["macro_names"]][:2], ["1 Cut", "2"])

    @unittest.skipUnless(os.path.exists(NI_INIT), "third_party/ni-file not cloned")
    def test_real_init_preset(self):
        d = massive.decode_file(NI_INIT)
        self.assertEqual(d["metadata"]["name"], "New Sound")
        self.assertEqual(d["metadata"]["app_id"], 7)
        self.assertEqual(d["metadata"]["app_version"], "1.0.0.0")
        self.assertEqual(d["state"]["version"], 23)
        self.assertEqual(d["state"]["macro_names"][0]["text"], "1 Vibrato")
        self.assertEqual(len(d["state"]["macro_names"]), 8)
        self.assertEqual(d["warnings"], [])
        with open(NI_INIT_STATE, "rb") as f:   # ni-file's own extraction of the same state
            self.assertEqual(base64.b64decode(d["state"]["raw_b64"]), f.read())


CAPTURES = os.path.join(ROOT, "captures")


class MassiveModulationTests(unittest.TestCase):
    """the developer's GUI saves (captures/, not in git): skipped when absent."""

    def conv(self, name):
        with tempfile.TemporaryDirectory() as t:
            return convert.convert(os.path.join(CAPTURES, name), t)[1]

    @unittest.skipUnless(os.path.exists(os.path.join(CAPTURES, "mod_env1_f1cutoff__100.nmsv")), "no captures")
    def test_gui_saves_decode(self):
        self.assertEqual(self.conv("modbase.nmsv")["modulation"], [])
        self.assertEqual(self.conv("mod_env1_f1cutoff__100.nmsv")["modulation"],
                         [{"source": "ENV1", "destination": "F1_CUTOFF", "amount": round(123 / 136, 5)}])
        self.assertEqual(self.conv("mod_lfo5_osc1wtpos__100.nmsv")["modulation"],
                         [{"source": "LFO1", "destination": "OSC1_POS", "amount": 1.0}])
        self.assertEqual(self.conv("mod_macro1_f1res__100.nmsv")["modulation"][0]["source"], "MACRO1")


class DiffTests(unittest.TestCase):
    def test_diff_and_summarize(self):
        base = [0.5] * 21
        with tempfile.TemporaryDirectory() as t:
            def save(name, vals):
                p = os.path.join(t, name)
                with open(p, "wb") as f:
                    f.write(make_fixtures.nmsv_bytes("D", vals, ["1"]))
                return p
            b = save("b.nmsv", base)
            db = os.path.join(t, "obs.jsonl")
            for k, (disp, ser) in enumerate([(25, 0.25), (75, 0.75), (100, 1.0)]):
                v = list(base); v[7] = ser
                c = save(f"c{k}.nmsv", v)
                nmsv_diff.main(["record", b, c, "--param", "OSC2 pitch", "--base-value", "50",
                                "--changed-value", str(disp), "--db", db, "--quiet"])
            s = nmsv_diff.summarize(db)
            self.assertEqual(s[0]["indices"], [7])
            self.assertEqual(s[0]["status"], "verified-linear")
            self.assertAlmostEqual(s[0]["fit"]["a"], 0.01, places=6)


class ConverterTests(unittest.TestCase):
    def test_mfp_python_layout_size(self):
        self.assertEqual(mfp.SIZE, 508)   # v5 (17 Nov 2026: new master effects)
        p = mfp.default_patch()
        self.assertEqual(mfp.read(mfp.write(p)), p)

    def test_unmapped_state_is_preserved(self):
        d = massive.decode_bytes(make_fixtures.nmsv_bytes("U", [0.3] * 21, ["1"]))
        d["state"]["version"] = 23
        prof = convert.load_profile(os.path.join(ROOT, "tools", "preset_converter", "maps", "massive_state_v23.json"))
        ir = convert.decoded_to_ir(d, prof, "massive_state_v23.json")
        self.assertEqual(len(ir["compatibility"]["unsupported"]), 21)
        self.assertEqual(ir["preserved"]["float_block"], d["state"]["float_block"]["values"])

    def test_fixture_conversion_values(self):
        with tempfile.TemporaryDirectory() as t:
            paths = make_fixtures.main(t)
            out, ir = convert.convert(paths[0], t)
            self.assertEqual(ir["oscillators"][1]["semis"], 7)
            self.assertEqual(ir["filters"][0]["type"], "LP2")
            with open(out["mfp"], "rb") as f:
                p = mfp.read(f.read())
            self.assertEqual(p["name"], "SAW PLUCK")
            self.assertEqual(p["osc"][1]["semis"], 7)
            self.assertEqual(p["mod"][0]["src"], mfp.SOURCES.index("ENV1"))
            self.assertEqual(p["fx"]["rev_amount"], round(0.25 * 32767))
        self.assertEqual(p["fx"]["rev_type"], mfp.REV_TYPES.index("TWIN TANK"))

    @unittest.skipUnless(os.path.exists(RENDER), "build/mfe_render.exe not built")
    def test_c_engine_reads_python_preset_and_renders(self):
        with tempfile.TemporaryDirectory() as t:
            paths = make_fixtures.main(t)
            for p in paths:
                out, _ = convert.convert(p, t)
                wav = os.path.join(t, "o.wav")
                r = subprocess.run([RENDER, out["mfp"], "demo", wav], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                peak = float(r.stdout.split("peak ")[1].split(" ")[0])
                self.assertTrue(-40 < peak <= 0, f"{p}: peak {peak} dBFS")
            with open(out["mfp"], "rb") as f:
                bad = bytearray(f.read())
            bad[50] ^= 0xFF
            bp = os.path.join(t, "bad.mfp")
            with open(bp, "wb") as f:
                f.write(bad)
            r = subprocess.run([RENDER, bp, "demo", os.path.join(t, "x.wav")], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("error -4", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
