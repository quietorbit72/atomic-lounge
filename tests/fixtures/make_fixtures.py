# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: build synthetic .nmsv files with the same container structure as a real
# Massive preset (RepositoryRoot > Preset > SoundInfoItem + EncryptionItem/SubtreeItem >
# Item > PresetChunkItem), but a fixture state (version 9001, see
# tools/preset_converter/maps/fixture_state_v9001.json). No NI data is used.
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "preset_decoder"))
import nis  # noqa: E402
import fastlz  # noqa: E402

FIXTURES = {
    # name: (floats in fixture order, macro names)
    "fixture_saw_pluck": ([0.0, 0.667, 0.8, 0.5, 1.0, 0.0, 0.667, 0.5 + 7 / 48, 0.6, 0.2, 0.55, 0.45,
                           0.0, 0.62, 0.0, 0.45, 0.0, 0.5, 0.0, 0.25, 0.6], ["1 Cutoff", "2", "3", "4", "5", "6", "7", "8"]),
    "fixture_sync_pad": ([2.0 / 5, 0.3, 0.7, 0.5, 1.0, 1.0 / 5, 0.5, 0.25 / 48 + 0.5, 0.5, 0.4, 0.7, 0.2,
                          0.55, 0.6, 0.8, 0.7, 0.6, 0.7, 0.3, 0.4, 0.8], ["1 Motion", "2", "3", "4", "5", "6", "7", "8"]),
}


def state_bytes(floats, macros):
    s = struct.pack("<I", 9001) + struct.pack(f"<{len(floats)}f", *floats)
    s += struct.pack("<I", 1)                      # ends the float run (not a float in range)
    for m in macros:
        b = m.encode("ascii")
        s += struct.pack("<I", len(b)) + b
    return s


def nmsv_bytes(name, floats, macros, compress=True):
    state = state_bytes(floats, macros)
    chunk = struct.pack("<IIIQ", 1, 1, 1, len(state)) + state + b"\0" * 8
    inner = nis.build_item([(0x01, 1, struct.pack("<I", 1))],
                           children=[(0x6D, nis.build_item([(0x6D, 1, chunk), (0x01, 1, struct.pack("<I", 1))]))])
    if compress:
        stream = fastlz.compress_literal(inner)
        sub = struct.pack("<IB", 1, 1) + struct.pack("<II", len(inner), len(stream)) + stream
    else:
        sub = struct.pack("<IB", 1, 0) + inner
    enc = nis.build_item([(0x74, 1, struct.pack("<IB", 1, 0)), (0x73, 1, sub), (0x01, 1, struct.pack("<I", 1))])
    info = nis.build_item([(0x6C, 1, struct.pack("<IIII", 1, 2, 1, 0) + nis.wstring(name) + nis.wstring("Vintage Chonk tests")),
                           (0x01, 1, struct.pack("<I", 1))])
    preset_data = struct.pack("<IBII", 1, 0, 7, 1) + nis.wstring("fixture")
    preset = nis.build_item([(0x65, 1, preset_data), (0x01, 1, struct.pack("<I", 1))],
                            children=[(0x6C, info), (0x74, enc)])
    root = nis.build_item([(0x76, 1, struct.pack("<I", 1) + b"\0" * 50), (0x01, 1, struct.pack("<I", 1))],
                          children=[(0x65, preset)])
    return root


def main(outdir=HERE):
    paths = []
    for name, (floats, macros) in FIXTURES.items():
        p = os.path.join(outdir, name + ".nmsv")
        with open(p, "wb") as f:
            f.write(nmsv_bytes(name.replace("fixture_", "").replace("_", " ").upper(), floats, macros))
        paths.append(p)
    return paths


if __name__ == "__main__":
    for p in main():
        print(p)
