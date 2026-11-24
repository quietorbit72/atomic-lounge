# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: Massive menu/switch choices -> IR (profile rules "massive_v24").
# The byte positions come from the profile (measured); this file only says how each Massive choice
# is approximated by the MFE engine.
import mfp  # tools/preset_converter (convert.py puts it on the path)

FILTERS = {"None": "OFF", "Lowpass 4": "LP4", "Lowpass 2": "LP2", "Highpass 4": "HP", "Highpass 2": "HP",
           "Bandpass": "BP", "Bandreject": "NOTCH", "Double Notch": "NOTCH"}
FILTER_APPROX = {"Highpass 4": "HP4 played as a 2-pole HP", "Allpass": "no allpass: LP2",
                 "Scream": "no Scream model: LP2", "Daft": "no Daft model: LP2", "Comb": "no comb filter: BP",
                 "Acid": "Massive Acid -> MFE ACID (303-style, not measured against Massive)", "Double Notch": "Double Notch is not provided : one notch"}
FILTER_FALLBACK = {"Allpass": "LP2", "Scream": "LP2", "Daft": "LP2", "Comb": "BP", "Acid": "ACID"}

# Massive master FX -> Vintage Chonk master effects (17 Nov 2026). The JS twin is editor/src/mf_core.js.
FX = {"Reverb": "reverb", "Reverb Sm": "reverb", "Chorus": "ensemble", "Chorus 1": "ensemble", "Chorus E": "ensemble",
      "Dim Exp": "ensemble", "Flanger +": "ensemble", "Flanger +1": "ensemble", "Flanger -": "ensemble",
      "Flanger -1": "ensemble", "Phaser": "ensemble", "Phaser 1": "ensemble", "Delay": "bbd", "Delay S": "bbd",
      "C Tube": "tape", "T Tube": "tape", "B Tube": "tape"}
FX_SCREEN = {"reverb": "TWIN TANK", "ensemble": "ENSEMBLE", "bbd": "BBD ECHO", "tape": "TAPE"}
FX_NOTE = {"Flanger +": "no flanger: ENSEMBLE II", "Flanger +1": "no flanger: ENSEMBLE II",
           "Flanger -": "no flanger: ENSEMBLE II", "Flanger -1": "no flanger: ENSEMBLE II",
           "Phaser": "no phaser: ENSEMBLE II", "Phaser 1": "no phaser: ENSEMBLE II",
           "C Tube": "tube as TAPE drive", "T Tube": "tube as TAPE drive", "B Tube": "tube as TAPE drive"}


MOOG_WAVES = ["triangle", "shark", "reverse saw", "saw", "square", "wide pulse", "narrow pulse", "narrowest pulse"]


def moog_wave_for(name):
    """16 Nov 2026: oscillators are classic-Moog-style, not Massive's wavetables. A Massive
    wavetable picks the nearest Model D waveform (frame of MFE_WT_MOOG)."""
    n = name.lower()
    if any(k in n for k in ("saw", "-sw", "sw ")):
        return 3
    if "pwm" in n or "pulse" in n:
        return 5
    if "squ" in n or "sqr" in n or "square" in n:
        return 4
    if "tri" in n:
        return 0
    if "sin" in n:
        return 0
    return 3


def choice(raw, ent):
    """The Massive choice name stored at ent's byte, or None."""
    off = ent["offset"]
    if off >= len(raw):
        return None
    if "values" in ent:
        return ent["values"].get(str(raw[off]))
    if "bit" in ent:
        on = (raw[off] >> ent["bit"]) & 1
        other = [c for c in ent["choices"] if c != ent["set_means"]]
        return ent["set_means"] if on else (other[0] if other else None)
    return None


def apply_slots(ir, raw):
    """Modulator slots 5-8 (LFO / Performer / Stepper) from tools/preset_decoder/massive_mods.py
    (decoded and verified against headless Massive 1.7, docs/MASSIVE_MODULATORS.md)."""
    import math
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "preset_decoder"))
    import massive_mods
    comp = ir["compatibility"]
    try:
        slots = massive_mods.decode_mod_slots(raw)
    except Exception as e:                     # older or unexpected layouts: keep the defaults
        comp["unsupported"].append(f"modulator slots 5-8 not decoded ({e})")
        return [1.0] * 4
    gains = []
    kinds = ["SINE", "TRI", "SAW_UP", "SAW_DOWN", "SQUARE", "SH", "PERFORMER", "STEPPER"]
    for i, s in enumerate(slots[:4]):
        m = s["mfe"]
        l = ir["lfos"][i]
        l["wave"] = kinds[m["wave"]] if m["wave"] < len(kinds) else "SINE"
        l["keysync"] = bool(m["flags"] & 1)
        l["unipolar"] = bool(m["flags"] & 2)
        l["sync"] = bool(m["flags"] & 4)
        l["sync_div"] = m["sync_div"]
        l["phase"] = m["phase"] / 32767
        l["loop_start"], l["loop_end"] = m["loop_start"], m["loop_end"]
        l["glide"] = m["glide"] / 32767
        l["steps"] = [{"shape": sh, "level": lv / 127} for sh, lv in zip(m["step_shape"], m["step_level"])]
        if m.get("rate_hz"):
            l["rate"] = max(0.0, min(1.0, math.log(max(m["rate_hz"], 0.02) / 0.02) / math.log(2000.0)))
        gains.append(s.get("output_gain", 1.0))
        conf = s.get("confidence", {})
        approx = [k for k, v in conf.items() if v != "verified"]
        comp["approximated" if approx else "supported"].append(
            f"slot {5 + i}: {l['wave']}" + (f" ({', '.join(approx)} not exact)" if approx else ""))
    return gains


def apply(ir, raw, profile, floats_by_name):
    comp = ir["compatibility"]
    gains = apply_slots(ir, raw)
    apply_modulation(ir, raw, {e["index"]: e["massive"] for e in profile.get("float_names", [])}, gains)
    choices = {}
    for name, ent in profile.get("enum_params", {}).items():
        c = choice(raw, ent)
        if c is not None:
            choices[name] = c
    ir["preserved"]["massive_choices"] = choices
    for n in range(3):
        o = ir["oscillators"][n]
        on = choices.get(f"osc{n + 1}_on_off")
        if on is not None:
            o["enabled"] = on == "on"
            comp["supported"].append(f"osc{n + 1} on/off = {on}")
        wt = choices.get(f"osc{n + 1}_wavetable")
        if wt:
            f = moog_wave_for(wt)
            o["massive_wavetable"] = wt
            o["massive_position"] = o.get("position")
            o["wavetable"] = "MOOG"
            o["position"] = f / 7
            comp["approximated"].append(f"osc{n + 1} wavetable '{wt}' -> Moog-style {MOOG_WAVES[f]} "
                                        f"(Massive wt-position {o['massive_position']:.2f} not used)")
        mode = choices.get(f"osc{n + 1}_mode")
        if mode and mode != "Spectrum":
            comp["approximated"].append(f"osc{n + 1} intensity mode '{mode}' -> phase warp")
        elif mode == "Spectrum":
            comp["approximated"].append(f"osc{n + 1} intensity mode Spectrum -> phase warp (no spectral mode)")
    mo = ir.setdefault("mod_osc", {})
    if choices.get("mod_osc_on_off") == "off":
        mo["enabled"] = False
    for key, field in (("mod_osc_rm_osc", "rm_osc"), ("mod_osc_phase_osc", "pm_osc"),
                       ("mod_osc_position_osc", "pos_osc"), ("mod_osc_filter_fm_osc", "ffm_filter")):
        c = choices.get(key)
        if c is not None:
            mo[field] = 0 if c == "off" else int(c)
    if any(mo.get(f) for f in ("rm_osc", "pm_osc", "pos_osc")) and mo.get("enabled", True):
        comp["approximated"].append(f"mod osc: ring -> osc {mo.get('rm_osc', 0)}, phase -> osc {mo.get('pm_osc', 0)}, "
                                    f"position -> osc {mo.get('pos_osc', 0)} (depth scales not measured)")
    if mo.get("ffm_filter") and mo.get("ffm"):
        comp["unsupported"].append("mod osc filter FM (stored, not rendered yet)")
    if choices.get("noise_on_off") == "off":
        ir["noise"]["amp"] = 0.0
    for n in range(2):
        f = ir["filters"][n]
        t = choices.get(f"filter{n + 1}_type")
        if t is not None:
            f["type"] = FILTERS.get(t) or FILTER_FALLBACK.get(t, "LP2")
            (comp["approximated"] if t in FILTER_APPROX else comp["supported"]).append(
                f"filter{n + 1} type '{t}' -> {f['type']}" + (f" ({FILTER_APPROX[t]})" if t in FILTER_APPROX else ""))
        if choices.get(f"filter{n + 1}_on_off") == "off":
            f["type"] = "OFF"
    vm = choices.get("voicemode")
    if vm:
        ir["voice"]["mode"] = "MONO" if vm == "Monophon" else "POLY"
        (comp["supported"] if vm != "Monorotate" else comp["approximated"]).append(f"voice mode {vm}")
    for slot in (1, 2):
        t = choices.get(f"master_fx{slot}_type")
        if not t or t == "None" or choices.get(f"master_fx{slot}_on_off") == "off":
            continue
        kind = FX.get(t)
        mix = floats_by_name.get(f"master_fx{slot}_dry_wet", 0.0)
        if kind:
            e = ir["effects"]
            if kind == "reverb":
                e["reverb"].update(type="TWIN TANK", amount=mix)
            if kind == "ensemble":
                e["ensemble"].update(mode="II" if t in FX_NOTE else "I", mix=mix)
            if kind == "bbd":
                e["bbd"].update(mix=mix, time=0.3, feedback=0.4)
                # Massive's delay times are note values (left n / d of a whole note): synced BBD ECHO at the nearest
                # Vintage Chonk division (the right channel's value is not used: one mono line)
                ln, ld = choices.get(f"master_fx{slot}_sd_ln"), choices.get(f"master_fx{slot}_sd_ld")
                if ln is not None and ld is not None and str(ln).isdigit() and str(ld).isdigit() and int(ld) > 0:
                    e["bbd"]["sync_div"] = mfp.nearest_delay_div(4 * int(ln), int(ld))
                    comp["approximated"].append(f"master FX{slot} delay {ln}/{ld} -> BBD ECHO synced, division "
                                                f"{e['bbd']['sync_div']}")
            if kind == "tape":
                e["tape"]["drive"] = max(e["tape"]["drive"], mix)
            note = f" ({FX_NOTE[t]})" if t in FX_NOTE else ""
            comp["approximated"].append(f"master FX{slot} '{t}' -> {FX_SCREEN[kind]}, dry/wet {mix:.2f}{note}")
        else:
            comp["unsupported"].append(f"master FX{slot} '{t}' (no equivalent)")
    for slot in (1, 2):
        t = choices.get(f"insert{slot}_type")
        if t and t != "None" and choices.get(f"insert{slot}_on_off") != "off":
            if t in ("Clip", "S Shaper", "P Shaper"):
                tp = ir["effects"]["tape"]
                tp["drive"] = max(tp["drive"], floats_by_name.get(f"insert{slot}_dw_prm_1", 0.0))
                comp["approximated"].append(f"insert{slot} '{t}' -> TAPE drive")
            else:
                comp["unsupported"].append(f"insert{slot} '{t}'")
    return ir


# ------------------------------------------------------------------ modulation (measured 2026-11-16) ---
# Source numbers: 0 = ENV 1 and 4 = LFO 5 and 13 = Macro 1 from the developer's GUI saves; the rest from
# injecting each number into filter 1 cutoff and rendering Massive (tools/compare/mod_source_probe.py):
# 0-2 decay like envelopes, 3 = the amp envelope, 4-6 periodic, 13-20 a fixed offset per macro,
# 22 follows velocity.
SOURCES = {0: "ENV1", 1: "ENV2", 2: "ENV3", 3: "ENV4", 4: "LFO1", 5: "LFO2", 6: "LFO3", 7: "LFO4",
           13: "MACRO1", 14: "MACRO2", 15: "MACRO3", 16: "MACRO4", 17: "MACRO5", 18: "MACRO6",
           19: "MACRO7", 20: "MACRO8", 21: "KEY", 22: "VEL", 23: "RANDOM", 24: "AFTERTOUCH", 25: "KEY"}
# 21/25: grow with the note (no shift at C2, large at C6): key tracking (KTr); 23: new value each
# note: trigger random (TrR); 24: silent headless, aftertouch by the manual's list (inferred).
# 8, 9, 12: unidentified (strong rise-then-fall in the first second; performer/stepper outputs?).
SOURCE_NOTES = {21: "Massive KTr is unipolar across the keyboard; MFE KEY is bipolar around C4",
                25: "second key-tracking source, same behaviour as 21", 24: "aftertouch inferred, not measured",
                4: "Massive LFO 5 -> MFE LFO 1", 5: "Massive LFO 6 -> MFE LFO 2", 6: "Massive LFO 7 -> MFE LFO 3",
                7: "Massive LFO 8 -> MFE LFO 4"}
# Massive knob (float index name) -> MFE destination
DESTS = {"osc1_pitch": "OSC1_PITCH", "osc2_pitch": "OSC2_PITCH", "osc3_pitch": "OSC3_PITCH",
         "osc1_position": "OSC1_POS", "osc2_position": "OSC2_POS", "osc3_position": "OSC3_POS",
         "osc1_param2": "OSC1_WARP", "osc2_param2": "OSC2_WARP", "osc3_param2": "OSC3_WARP",
         "osc1_amp": "OSC1_AMP", "osc2_amp": "OSC2_AMP", "osc3_amp": "OSC3_AMP", "noise_amp": "NOISE_AMP",
         "filter1_cut_prm_1": "F1_CUTOFF", "filter2_cut_prm_1": "F2_CUTOFF",
         "filter1_res_prm_3": "F1_RES", "filter2_res_prm_3": "F2_RES",
         "seriell_parallel": "SERPAR", "pan_fltr_path": "PAN", "insert1_dw_prm_1": "DRIVE",
         "mod5_rate": "LFO1_RATE", "mod6_rate": "LFO2_RATE"}


# Depth: Massive's depth d shifts the target by d x the knob's full range (measured with ENV 4 on
# filter 1 cutoff: +0.1 -> +0.100 knob, +0.25 -> +0.250; on OSC 1 pitch: +0.1 -> +12.7 semitones,
# i.e. 127-128 st per 1.0). MFE amount = d x (Massive range / MFE full-scale range).
CUTOFF_SCALE = 123.0 / 136.0          # Massive cutoff knob ~123 st over its range; MFE +-1 = 136 st
PITCH_SCALE = 128.0 / 64.0            # Massive pitch knob 128 st; MFE_PITCH_MOD_RANGE 64
DEPTH_SCALE = {"F1_CUTOFF": CUTOFF_SCALE, "F2_CUTOFF": CUTOFF_SCALE, "PITCH": PITCH_SCALE,
               "OSC1_PITCH": PITCH_SCALE, "OSC2_PITCH": PITCH_SCALE, "OSC3_PITCH": PITCH_SCALE}
DEPTH_MEASURED = {"F1_CUTOFF", "F2_CUTOFF", "OSC1_PITCH", "OSC2_PITCH", "OSC3_PITCH"}


def mod_slots(raw, index):
    """Slots of knob `index`: the block right before the record keyed `index` (u32 count, then
    count x (f32 depth, i32 source, u16)). Returns [(depth, source)] for filled slots, or None."""
    import struct
    for t in range(0, 10):
        key = struct.pack("<IBI", index, 0, t)
        k = raw.find(key)
        if k < 0:
            continue
        for n in (3, 2, 1, 4):
            c = k - 10 * n - 4
            if c >= 0 and struct.unpack_from("<I", raw, c)[0] == n:
                out = []
                for s in range(n):
                    depth, src = struct.unpack_from("<fi", raw, c + 4 + 10 * s)
                    if src >= 0:
                        out.append((depth, src))
                return out
        return None
    return None


def apply_modulation(ir, raw, names, slot_gains=None):
    comp = ir["compatibility"]
    for idx, name in sorted(names.items()):
        slots = mod_slots(raw, idx)
        if not slots:
            continue
        for depth, src in slots:
            s, d = SOURCES.get(src), DESTS.get(name)
            label = f"modulation source {src} -> {name} depth {depth:+.2f}"
            if s and d:
                amt = depth * DEPTH_SCALE.get(d, 1.0)
                if 4 <= src <= 7 and slot_gains:
                    amt *= slot_gains[src - 4]          # the slot's Amp knob (measured amp^2)
                clipped = abs(amt) > 1.0
                amt = max(-1.0, min(1.0, amt))
                ir["modulation"].append({"source": s, "destination": d, "amount": round(amt, 5)})
                why = ["depth scale measured" if d in DEPTH_MEASURED else "depth taken as knob fraction"]
                if clipped:
                    why.append("depth beyond the MFE range, clipped")
                if src in SOURCE_NOTES:
                    why.append(SOURCE_NOTES[src])
                comp["approximated"].append(f"{label} -> {s} -> {d} amount {amt:+.3f} ({'; '.join(why)})")
            else:
                comp["unsupported"].append(label + (" (source not identified)" if not s else " (no MFE destination)"))
