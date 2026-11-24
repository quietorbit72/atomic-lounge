# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: decode a Native Instruments Massive (original, not Massive X)
# .nmsv preset into the versioned JSON described in docs/MASSIVE_PRESET_FORMAT.md.
#
# What is VERIFIED (from the bytes of a public init preset, 000-new.nmsv):
#   container tree, FastLZ payload, metadata strings, authoring app id 7,
#   PresetChunkItem header (u32 x3, u64 state size), state starts with u32 23,
#   followed by a run of normalised float32 values, then structured sections
#   that include the eight macro names.
# What is NOT verified: which float is which Massive parameter, where the float
#   block really ends, and the meaning of the structured tail. Every byte is
#   kept (raw_b64) so later mapping work needs no re-conversion.
import base64
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nis  # noqa: E402

SCHEMA = "vintage-chonk/decoded-preset"
SCHEMA_VERSION = 1
MASSIVE_APP_ID = 7


def decode_bytes(buf: bytes, filename: str = "") -> dict:
    root = nis.parse(buf)
    warnings: list[str] = []
    out = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "source": {"file": filename, "size": len(buf), "sha256": hashlib.sha256(buf).hexdigest()},
        "container": {"root_version": root.version, "tree": _tree(root)},
        "metadata": {},
        "state": None,
        "warnings": warnings,
    }
    preset = _first_frame(root, 0x65)
    if preset is not None:
        out["metadata"].update(_preset_frame(preset.data, warnings))
    else:
        warnings.append("no Preset item: not a single-preset file")
    info = _first_frame(root, 0x6C)
    if info is not None:
        out["metadata"].update(_sound_info(info.data, warnings))
    if out["metadata"].get("app_id") not in (None, MASSIVE_APP_ID):
        warnings.append(f"authoring app id {out['metadata'].get('app_id')} is not Massive (7)")
    chunk = _first_frame(root, 0x6D)
    if chunk is None:
        warnings.append("no PresetChunkItem: no synth state found")
    else:
        out["state"] = decode_state_chunk(chunk.data, warnings)
    return out


def decode_file(path: str) -> dict:
    with open(path, "rb") as f:
        buf = f.read()
    return decode_bytes(buf, path.replace("\\", "/").split("/")[-1])


# ------------------------------------------------------------------ parts ---

def _first_frame(root, item_id):
    for _, it in root.walk():
        f = it.frame(item_id)
        if f is not None:
            return f
    return None


def _tree(item):
    node = {"type": item.type_name, "frames": [
        {"name": f.name, "id": f.item_id, "version": f.version, "bytes": len(f.data)} for f in item.frames]}
    if item.subtree is not None:
        node["subtree"] = _tree(item.subtree)
    if item.children:
        node["children"] = [_tree(c) for c in item.children]
    return node


def _preset_frame(data, warnings):
    # u32 version | u8 flag | u32 app id | u32 ? | wstring app version
    md = {}
    try:
        md["preset_item_version"] = struct.unpack_from("<I", data, 0)[0]
        md["app_id"] = struct.unpack_from("<I", data, 5)[0]
        md["app"] = nis.APP_IDS.get(md["app_id"], "unknown")
        md["app_version"], _ = nis.read_wstring(data, 13)
    except (struct.error, nis.NISError) as e:
        warnings.append(f"Preset item: {e}")
    return md


def _sound_info(data, warnings):
    # u32 1 | u32 2 | u32 1 | u32 0 | then length-prefixed UTF-16 strings and
    # fields. Name is verified; the remaining strings are read in order and
    # reported as found (field names unverified).
    md = {}
    try:
        md["name"], p = nis.read_wstring(data, 16)
    except nis.NISError as e:
        warnings.append(f"SoundInfoItem name: {e}")
        return md
    strings = []
    q = p
    while q + 4 <= len(data):
        n = struct.unpack_from("<I", data, q)[0]
        if 0 < n < 256 and q + 4 + 2 * n <= len(data):
            raw = data[q + 4:q + 4 + 2 * n]
            try:
                s = raw.decode("utf-16-le")
                if s.isprintable():
                    strings.append({"offset": q, "text": s})
                    q += 4 + 2 * n
                    continue
            except UnicodeDecodeError:
                pass
        q += 1
    md["sound_info_strings"] = strings
    md["sound_info_raw_b64"] = base64.b64encode(data).decode()
    return md


def decode_state_chunk(data: bytes, warnings: list) -> dict:
    # PresetChunkItem data: u32 | u32 | u32 | u64 state_size | state | trailer
    if len(data) < 20:
        warnings.append("PresetChunkItem too short")
        return {"raw_b64": base64.b64encode(data).decode()}
    h = struct.unpack_from("<III", data, 0)
    size = struct.unpack_from("<Q", data, 12)[0]
    if 20 + size > len(data):
        warnings.append("state size larger than chunk; using what is there")
        size = len(data) - 20
    state = data[20:20 + size]
    trailer = data[20 + size:]
    # Massive 1.7 (seen 2026-11-16 in the plugin state of Massive 1.7.0.0): an outer wrapper
    # u32 5 | u32 size | u32 0 | "tsrp" around the older inner state (u32 version | floats ...)
    wrapper = None
    if len(state) >= 20 and struct.unpack_from("<I", state, 0)[0] == 5 and state[12:16] == b"tsrp":
        wrapper = {"version": 5, "size": struct.unpack_from("<I", state, 4)[0], "tag": "prst"}
        state = state[16:]
    out = {
        "chunk_header": list(h),
        "size": len(state),
        "version": struct.unpack_from("<I", state, 0)[0] if len(state) >= 4 else None,
        "raw_b64": base64.b64encode(state).decode(),
        "chunk_trailer_hex": trailer.hex(),
        "wrapper": wrapper,
    }
    if out["version"] not in (23, 24):
        warnings.append(f"state version {out['version']} (only 23 and 24 have been seen)")
    floats, end = _float_block(state)
    out["float_block"] = {
        "offset": 4, "end": end, "count": len(floats),
        "boundary": "heuristic (first value that is not 0 or a float in [1e-6, 1])",
        "values": floats,
    }
    out["macro_names"] = _macro_names(state)
    out["tail_offset"] = end
    return out


def _float_block(state):
    vals = []
    o = 4
    while o + 4 <= len(state):
        v = struct.unpack_from("<f", state, o)[0]
        if not (v == 0.0 or 1e-6 < abs(v) <= 1.0001):
            break
        vals.append(v)
        o += 4
    return vals, o


def _macro_names(state):
    """The eight macro labels: u32 length + ASCII, '1 ...' to '8 ...' in order."""
    names = []
    for k in range(1, 9):
        found = None
        start = 0
        while True:
            i = state.find(str(k).encode(), start)
            if i < 4:
                if i < 0:
                    break
                start = i + 1
                continue
            n = struct.unpack_from("<I", state, i - 4)[0]
            if 1 <= n <= 64 and i + n <= len(state):
                s = state[i:i + n]
                if all(32 <= c < 127 for c in s):
                    found = {"offset": i - 4, "text": s.decode("ascii")}
                    if not names or found["offset"] > names[-1]["offset"]:
                        break
            start = i + 1
        if found is None:
            break
        names.append(found)
    return names


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Decode a Massive .nmsv preset to JSON")
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    a = ap.parse_args(argv)
    d = decode_file(a.input)
    text = json.dumps(d, indent=1)
    if a.output:
        with open(a.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
