# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: .nmsv -> decoded JSON -> intermediate preset (IR) -> FM1 binary preset (.mfp)
# plus a compatibility report.
#
#   python convert.py PRESET.nmsv [-o OUTDIR] [--profile auto|<file>]
#
# The mapping from Massive's serialized values to IR fields lives in data files
# (maps/*.json), one profile per state version, each entry carrying its own
# verification status. Nothing is guessed in code.
import base64
import copy
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "preset_decoder"))
import mfp  # noqa: E402
import massive  # noqa: E402

IR_SCHEMA = "vintage-chonk/ir"
IR_VERSION = 1


def default_ir():
    return {
        "schema": IR_SCHEMA, "schema_version": IR_VERSION,
        "metadata": {"name": "INIT"},
        "voice": {"mode": "POLY", "max_voices": 8, "bend_range": 2, "glide": 0.0, "vel_sens": 0.5, "master": 0.25},   # master 0..1 = gain 0..4 (0.25 = unity)
        "macros": [{"name": str(i + 1), "value": 0.0} for i in range(8)],
        "oscillators": [{"enabled": i == 0, "wavetable": "MOOG", "position": 3 / 7,
                         "warp": 0.0, "semis": 0, "fine_cents": 0, "amp": 0.8, "route": 0.0,
                         "key_reset": False,
                         # Moog-style character (16 Nov 2026): a little analogue drift by default
                         "drift": 0.25, "lo_range": False} for i in range(3)],
        "noise": {"amp": 0.0, "color": 0.0, "route": 0.0},
        "mod_osc": {"enabled": True, "semis": 0, "rm": 0.0, "pm": 0.0, "pos": 0.0, "ffm": 0.0,
                    "rm_osc": 0, "pm_osc": 0, "pos_osc": 0, "ffm_filter": 0},
        "filters": [{"type": "LP2" if i == 0 else "OFF", "cutoff": 0.92 if i == 0 else 0.0, "resonance": 0.0,
                     "drive": 0.0, "keytrack": 0.0} for i in range(2)],
        "routing": {"serpar": 0.0, "f1_mix": 1.0, "f2_mix": 1.0},
        "envelopes": [{"delay": 0.0, "attack": 0.06, "decay": 0.49, "sustain": 1.0, "release": 0.37, "loop": False,
                        "att_level": 1.0, "dec_level": None, "sust_time": 0.0}
                      for _ in range(4)],
        "lfos": [{"wave": "SINE", "rate": 0.49, "phase": 0.0, "keysync": False, "unipolar": False,
                  "sync": False, "sync_div": 8, "loop_start": 0, "loop_end": 15, "glide": 0.0,
                  "steps": [{"shape": 0, "level": 1.0} for _ in range(16)]} for _ in range(4)],
        "modulation": [],
        "effects": {"fuzz": {"amount": 0.0, "octave": 1.0, "tone": 0.5, "level": 0.5},
                    "tape": {"drive": 0.0, "tone": 0.5, "level": 0.5, "wow": 0.0},
                    "ensemble": {"mode": "OFF", "mix": 0.5, "rate": 0.5, "depth": 0.5},
                    "bbd": {"mix": 0.0, "time": 0.3, "feedback": 0.4, "mod": 0.2, "tone": 0.5, "sync_div": None},
                    "oilcan": {"mix": 0.0, "time": 0.5, "feedback": 0.7, "warble": 0.15, "smear": 0.1, "sync_div": None},
                    "reverb": {"type": "TWIN TANK", "amount": 0.0, "hold": False}},
        "midi": {"macro_ccs": list(range(16, 24)), "modwheel_cc": 1, "sustain_cc": 64},
        "compatibility": {"supported": [], "approximated": [], "unsupported": [], "missing_wavetables": [],
                          "notes": []},
        "preserved": {},
    }


# ------------------------------------------------------------- mapping ---

def load_profile(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def pick_profile(decoded):
    ver = (decoded.get("state") or {}).get("version")
    for fn in sorted(os.listdir(os.path.join(HERE, "maps"))):
        prof = load_profile(os.path.join(HERE, "maps", fn))
        if prof.get("state_version") == ver:
            return prof, fn
    return None, None


def _set(ir, path, value):
    parts = path.split(".")
    node = ir
    for p in parts[:-1]:
        node = node[int(p)] if p.isdigit() else node[p]
    last = parts[-1]
    if last.isdigit():
        node[int(last)] = value
    else:
        node[last] = value


def _transform(v, t):
    kind = t.get("kind", "linear")
    if kind == "linear":
        a, b = t.get("from", [0, 1])
        c, d = t.get("to", [0, 1])
        x = c + (v - a) * (d - c) / (b - a)
        if t.get("round"):
            x = int(round(x))
        return x
    if kind == "enum":
        names = t["values"]
        return names[max(0, min(len(names) - 1, int(round(v * (len(names) - 1)))))]
    if kind == "massive_time":
        # measured Massive curve (knob -> seconds), log-interpolated, then MFE's time setting:
        # t = 0.5 ms * 60000^p (MFE_TIME_RANGE); "ramp90": measured at 90 % of a linear attack
        import math
        pts = sorted((x, y) for x, y in t["points"] if y)
        if v <= pts[0][0]:
            (x0, y0), (x1, y1) = pts[0], pts[1]
        elif v >= pts[-1][0]:
            (x0, y0), (x1, y1) = pts[-2], pts[-1]
        else:
            k = next(i for i in range(1, len(pts)) if pts[i][0] >= v)
            (x0, y0), (x1, y1) = pts[k - 1], pts[k]
        sec = math.exp(math.log(y0) + (v - x0) * (math.log(y1) - math.log(y0)) / (x1 - x0))
        if t.get("curve") == "ramp90":
            sec /= 0.9
        return max(0.0, min(1.0, math.log(max(sec, 0.0005) / 0.0005) / math.log(60000.0)))
    if kind == "massive_lfo_rate":
        # measured LFO frequency (Hz) per knob value, log-interpolated -> MFE rate: 0.02 Hz * 2000^p
        import math
        pts = sorted((x, y) for x, y in t["points"] if y)
        if v <= pts[0][0]:
            (x0, y0), (x1, y1) = pts[0], pts[1]
        elif v >= pts[-1][0]:
            (x0, y0), (x1, y1) = pts[-2], pts[-1]
        else:
            k = next(i for i in range(1, len(pts)) if pts[i][0] >= v)
            (x0, y0), (x1, y1) = pts[k - 1], pts[k]
        hz = math.exp(math.log(y0) + (v - x0) * (math.log(y1) - math.log(y0)) / (x1 - x0))
        return max(0.0, min(1.0, math.log(max(hz, 0.02) / 0.02) / math.log(2000.0)))
    if kind == "table":
        pts = sorted(t["points"])
        if v <= pts[0][0]:
            return pts[0][1]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if v <= x1:
                return y0 + (v - x0) * (y1 - y0) / (x1 - x0)
        return pts[-1][1]
    if kind == "power":
        return max(0.0, min(1.0, v)) ** t.get("exp", 2.0)
    if kind == "massive_master":
        # measured: Massive's output amplitude follows the knob squared; k matches levels at 0.5
        return max(0.0, min(1.0, t["k"] * v * v / 4.0))
    if kind == "massive_cutoff":
        # measured -6 dB frequency (Hz) per knob value, log-interpolated -> MFE cutoff (note / 136)
        import math
        pts = sorted((x, y) for x, y in t["points"] if y)
        if v <= pts[0][0]:
            (x0, y0), (x1, y1) = pts[0], pts[1]
        elif v >= pts[-1][0]:
            (x0, y0), (x1, y1) = pts[-2], pts[-1]
        else:
            k = next(i for i in range(1, len(pts)) if pts[i][0] >= v)
            (x0, y0), (x1, y1) = pts[k - 1], pts[k]
        hz = math.exp(math.log(y0) + (v - x0) * (math.log(y1) - math.log(y0)) / (x1 - x0))
        note = 69 + 12 * math.log2(max(hz, 8.0) / 440.0)
        return max(0.0, min(1.0, note / 136.0))
    if kind == "bool":
        return v >= t.get("threshold", 0.5)
    raise ValueError(f"unknown transform {kind}")


def decoded_to_ir(decoded, profile, profile_name=""):
    ir = default_ir()
    comp = ir["compatibility"]
    md = decoded.get("metadata", {})
    ir["metadata"] = {"name": md.get("name") or "UNNAMED", "source_app": md.get("app"),
                      "source_app_version": md.get("app_version"),
                      "source_sha256": decoded.get("source", {}).get("sha256"),
                      "mapping_profile": profile_name}
    st = decoded.get("state") or {}
    vals = (st.get("float_block") or {}).get("values", [])
    ir["preserved"] = {"state_version": st.get("version"), "float_block": vals,
                       "float_block_boundary": (st.get("float_block") or {}).get("boundary"),
                       "raw_state_b64": st.get("raw_b64"), "decoder_warnings": decoded.get("warnings", [])}
    for i, m in enumerate(st.get("macro_names", [])[:8]):
        ir["macros"][i]["name"] = m["text"]
    if profile is None:
        comp["notes"].append(f"no mapping profile for state version {st.get('version')}: "
                             "every Massive value is preserved, none is applied")
        comp["unsupported"] += [f"float[{i}]" for i in range(len(vals))]
        return ir
    mapped = set()
    for ent in profile.get("float_params", []):
        i = ent["index"]
        if i >= len(vals):
            comp["unsupported"].append(f"float[{i}] ({ent.get('massive', '?')}): missing in this file")
            continue
        try:
            _set(ir, ent["ir"], _transform(vals[i], ent.get("transform", {})))
        except (KeyError, IndexError, ValueError) as e:
            comp["unsupported"].append(f"float[{i}] ({ent.get('massive', '?')}): {e}")
            continue
        mapped.add(i)
        label = f"float[{i}] {ent.get('massive', '?')} -> {ent['ir']}"
        if ent.get("status") == "verified" and not ent.get("approximation"):
            comp["supported"].append(label)
        else:
            why = ent.get("approximation") or f"mapping {ent.get('status', 'unverified')}"
            comp["approximated"].append(f"{label} ({why})")
    for ent in profile.get("modulation", []):
        ir["modulation"].append(dict(ent["slot"]))
        comp["approximated"].append(f"modulation {ent['slot']} ({ent.get('status', 'unverified')})")
    names = {e["index"]: e["massive"] for e in profile.get("float_names", [])}
    for i in range(len(vals)):
        if i not in mapped:
            label = f" {names[i]}" if i in names else ""
            comp["unsupported"].append(f"float[{i}]{label} = {vals[i]:.5g} (unmapped, preserved)")
    if profile.get("rules") == "massive_v24" and st.get("raw_b64"):
        import massive_rules
        by_name = {names[i]: vals[i] for i in names if i < len(vals)}
        massive_rules.apply(ir, base64.b64decode(st["raw_b64"]), profile, by_name)
    for o in ir["oscillators"]:
        if o.get("massive_wavetable"):
            comp["missing_wavetables"].append(o["massive_wavetable"])
    comp["notes"] += profile.get("notes", [])
    comp["notes"] += [
        "wavetables are Vintage Chonk originals, not Massive's factory tables",
        "filters are a generic TPT state-variable design, not measured against Massive",
        "no claim of acoustic identity: no reference recordings compared yet",
    ]
    return ir


# ---------------------------------------------------------- IR -> .mfp ---

def _q(x, lo=0.0, hi=1.0):
    x = max(lo, min(hi, float(x)))
    return max(-32768, min(32767, int(round(x * 32767))))


def ir_to_patch(ir):
    p = mfp.default_patch()
    name = re.sub(r"[^ -~]", "?", ir["metadata"].get("name", "UNNAMED"))[:23]
    p["name"] = name
    v = ir["voice"]
    p["voice_mode"] = mfp.VOICE_MODES.index(v["mode"])
    p["max_voices"] = int(max(1, min(8, v["max_voices"])))
    p["bend_range"] = int(max(0, min(24, v["bend_range"])))
    p["glide"] = _q(v["glide"])
    p["vel_sens"] = int(round(max(0, min(1, v["vel_sens"])) * 127))
    p["master"] = _q(v["master"])
    p["macro"] = [_q(m["value"]) for m in ir["macros"]]
    for o, s in zip(p["osc"], ir["oscillators"]):
        o["table"] = mfp.WAVETABLES.index(s["wavetable"])
        o["flags"] = ((mfp.OSC_ON if s["enabled"] else 0) | (mfp.OSC_RESET if s.get("key_reset") else 0)
                      | (mfp.OSC_LO if s.get("lo_range") else 0))
        o["drift"] = int(round(max(0.0, min(1.0, s.get("drift", 0.0))) * 127))
        o["semis"] = int(max(-64, min(63, s["semis"])))
        o["fine"] = int(max(-100, min(100, s["fine_cents"])))
        o["position"], o["warp"], o["amp"], o["route"] = (_q(s["position"]), _q(s["warp"]), _q(s["amp"]),
                                                          _q(s["route"]))
    n = ir["noise"]
    p["noise"] = dict(amp=_q(n["amp"]), color=_q(n["color"]), route=_q(n["route"]))
    for f, s in zip(p["filt"], ir["filters"]):
        f["type"] = mfp.FILTERS.index(s["type"])
        f["cutoff"], f["res"], f["drive"] = _q(s["cutoff"]), _q(s["resonance"]), _q(s["drive"])
        f["keytrack"] = _q(s["keytrack"], -1, 1)
    r = ir["routing"]
    p["serpar"], p["f1_mix"], p["f2_mix"] = _q(r["serpar"]), _q(r["f1_mix"]), _q(r["f2_mix"])
    for e, s in zip(p["env"], ir["envelopes"]):
        e.update(delay=_q(s["delay"]), attack=_q(s["attack"]), decay=_q(s["decay"]), sustain=_q(s["sustain"]),
                 release=_q(s["release"]), flags=mfp.ENV_LOOP if s["loop"] else 0,
                 att_level=_q(s.get("att_level", 1.0)),
                 dec_level=-1 if s.get("dec_level") is None else _q(s["dec_level"]),
                 sust_time=_q(s.get("sust_time", 0.0)))
    for l, s in zip(p["lfo"], ir["lfos"]):
        steps = (s.get("steps") or [])[:16] + [{"shape": 0, "level": 1.0}] * (16 - len(s.get("steps") or []))
        l.update(wave=mfp.LFO_WAVES.index(s["wave"]), rate=_q(s["rate"]), phase=_q(s["phase"]),
                 flags=((mfp.LFO_KEYSYNC if s["keysync"] else 0) | (mfp.LFO_UNIPOLAR if s["unipolar"] else 0)
                        | (mfp.LFO_SYNC if s.get("sync") else 0)),
                 sync_div=int(max(0, min(12, s.get("sync_div", 8)))),
                 loop_start=int(max(0, min(15, s.get("loop_start", 0)))),
                 loop_end=int(max(0, min(15, s.get("loop_end", 15)))),
                 glide=_q(s.get("glide", 0.0)),
                 step_shape=[int(max(0, min(31, st.get("shape", 0)))) for st in steps],
                 step_level=[int(round(max(-1.0, min(1.0, st.get("level", 1.0))) * 127)) for st in steps])
    if len(ir["modulation"]) > mfp.NMOD:
        ir["compatibility"]["unsupported"].append(
            f"{len(ir['modulation']) - mfp.NMOD} modulation slots beyond the {mfp.NMOD} the engine has")
    for m, s in zip(p["mod"], ir["modulation"][:mfp.NMOD]):
        m.update(src=mfp.SOURCES.index(s["source"]), dst=mfp.DESTINATIONS.index(s["destination"]),
                 amount=_q(s["amount"], -1, 1))
    mo = ir.get("mod_osc") or {}
    on = mo.get("enabled", True)
    p["modosc"] = dict(semis=int(max(-48, min(48, mo.get("semis", 0)))),
                       rm_osc=int(mo.get("rm_osc", 0)) if on else 0, pm_osc=int(mo.get("pm_osc", 0)) if on else 0,
                       pos_osc=int(mo.get("pos_osc", 0)) if on else 0,
                       ffm_filter=int(mo.get("ffm_filter", 0)) if on else 0,
                       rm=_q(mo.get("rm", 0)), pm=_q(mo.get("pm", 0)), pos=_q(mo.get("pos", 0)), ffm=_q(mo.get("ffm", 0)))
    fx = ir["effects"]
    fz, tp, en, bb, oc, rv = (fx["fuzz"], fx["tape"], fx["ensemble"], fx["bbd"], fx["oilcan"], fx["reverb"])
    p["fx"] = dict(fuzz_amount=_q(fz["amount"]), fuzz_octave=_q(fz["octave"]), fuzz_tone=_q(fz["tone"]),
                   fuzz_level=_q(fz["level"]),
                   tape_drive=_q(tp["drive"]), tape_tone=_q(tp["tone"]), tape_level=_q(tp["level"]), tape_wow=_q(tp["wow"]),
                   ens_mode=mfp.ENS_MODES.index(en["mode"]), ens_mix=_q(en["mix"]), ens_rate=_q(en["rate"]),
                   ens_depth=_q(en["depth"]),
                   bbd_mix=_q(bb["mix"]), bbd_time=_q(bb["time"]), bbd_feedback=_q(bb["feedback"]), bbd_mod=_q(bb["mod"]),
                   bbd_tone=_q(bb["tone"]),
                   oil_mix=_q(oc["mix"]), oil_time=_q(oc["time"]), oil_feedback=_q(oc["feedback"]),
                   oil_warble=_q(oc["warble"]), oil_smear=_q(oc["smear"]),
                   rev_type=mfp.REV_TYPES.index(rv["type"]), rev_hold=1 if rv["hold"] else 0, rev_amount=_q(rv["amount"]),
                   fx_sync=mfp.fx_sync(bb.get("sync_div"), oc.get("sync_div")))
    return p


def report_markdown(ir):
    c = ir["compatibility"]
    lines = [f"# Compatibility report: {ir['metadata'].get('name')}", "",
             f"Mapping profile: {ir['metadata'].get('mapping_profile') or 'none'}  ",
             f"Source: {ir['metadata'].get('source_app')} {ir['metadata'].get('source_app_version')}, "
             f"sha256 {ir['metadata'].get('source_sha256')}", "",
             f"| Supported | Approximated | Unsupported (preserved) | Missing wavetables |",
             f"|---|---|---|---|",
             f"| {len(c['supported'])} | {len(c['approximated'])} | {len(c['unsupported'])} | "
             f"{len(c['missing_wavetables'])} |", ""]
    for title, key in (("Supported", "supported"), ("Approximated", "approximated"),
                       ("Unsupported", "unsupported"), ("Missing wavetables", "missing_wavetables"),
                       ("Fidelity notes", "notes")):
        lines.append(f"## {title}")
        lines += [f"- {x}" for x in c[key]] or ["- (none)"]
        lines.append("")
    return "\n".join(lines)


def convert(path, outdir, profile_path="auto"):
    if path.lower().endswith(".json"):
        with open(path, encoding="utf-8") as f:
            decoded = json.load(f)
    else:
        decoded = massive.decode_file(path)
    if profile_path == "auto":
        profile, pname = pick_profile(decoded)
    else:
        profile, pname = load_profile(profile_path), os.path.basename(profile_path)
    ir = decoded_to_ir(decoded, profile, pname)
    patch = ir_to_patch(ir)
    blob = mfp.write(patch)
    base = os.path.splitext(os.path.basename(path))[0]
    os.makedirs(outdir, exist_ok=True)
    out = {"decoded": os.path.join(outdir, base + ".decoded.json"), "ir": os.path.join(outdir, base + ".ir.json"),
           "mfp": os.path.join(outdir, base + ".mfp"), "report": os.path.join(outdir, base + ".report.md")}
    with open(out["decoded"], "w", encoding="utf-8") as f:
        json.dump(decoded, f, indent=1)
    with open(out["ir"], "w", encoding="utf-8") as f:
        json.dump(ir, f, indent=1)
    with open(out["mfp"], "wb") as f:
        f.write(blob)
    with open(out["report"], "w", encoding="utf-8") as f:
        f.write(report_markdown(ir))
    return out, ir


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Convert a Massive .nmsv to an FM1 .mfp preset")
    ap.add_argument("input")
    ap.add_argument("-o", "--outdir", default="build/converted")
    ap.add_argument("--profile", default="auto")
    a = ap.parse_args(argv)
    out, ir = convert(a.input, a.outdir, a.profile)
    c = ir["compatibility"]
    print(f"{ir['metadata']['name']}: {len(c['supported'])} supported, {len(c['approximated'])} approximated, "
          f"{len(c['unsupported'])} unsupported (preserved)")
    for k, v in out.items():
        print(f"  {k}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
