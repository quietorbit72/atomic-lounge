# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: the FM1 binary preset (.mfp), format version 5, Python side.
# Field order and sizes must match dsp/mfe_preset.c (tests/run_py_tests.py checks both ways).
import struct
import zlib

MAGIC = b"MFP1"
VERSION = 5   # v2: envelope stages; v3: Performer/Stepper; v4: modulation oscillator; v5: new master effects

WAVETABLES = ["BASIC", "PWM", "SYNC", "FORMANT", "ORGAN", "SQUASH", "MOOG"]
FILTERS = ["OFF", "LP2", "LP4", "BP", "HP", "NOTCH", "LADDER", "ACID"]
LFO_WAVES = ["SINE", "TRI", "SAW_UP", "SAW_DOWN", "SQUARE", "SH", "PERFORMER", "STEPPER"]
LFO_SYNC = 4
ENS_MODES = ["OFF", "I", "II", "I+II", "VIBRATO"]     # MFE_ENS_*
REV_TYPES = ["TWIN TANK", "SPRING"]                  # MFE_REV_*
PERF_STEPS = 16
VOICE_MODES = ["POLY", "MONO", "LEGATO"]
SOURCES = ["NONE", "ENV1", "ENV2", "ENV3", "ENV4", "LFO1", "LFO2", "LFO3", "LFO4",
           "VEL", "KEY", "MODWHEEL", "BEND", "AFTERTOUCH",
           "MACRO1", "MACRO2", "MACRO3", "MACRO4", "MACRO5", "MACRO6", "MACRO7", "MACRO8",
           "RANDOM", "ONE", "OSC3"]
DESTINATIONS = ["NONE", "PITCH", "OSC1_PITCH", "OSC2_PITCH", "OSC3_PITCH",
                "OSC1_POS", "OSC2_POS", "OSC3_POS", "OSC1_WARP", "OSC2_WARP", "OSC3_WARP",
                "OSC1_AMP", "OSC2_AMP", "OSC3_AMP", "NOISE_AMP",
                "F1_CUTOFF", "F2_CUTOFF", "F1_RES", "F2_RES",
                "SERPAR", "DRIVE", "AMP", "PAN", "LFO1_RATE", "LFO2_RATE"]
OSC_ON, OSC_RESET, OSC_LO = 1, 2, 4
LFO_KEYSYNC, LFO_UNIPOLAR = 1, 2
ENV_LOOP = 1
NOSC, NFILT, NENV, NLFO, NMOD, NMACRO = 3, 2, 4, 4, 16, 8

# mfe_fx_defaults() in dsp/effects/mfe_fx.c: every effect off, the other knobs at their usual settings
FX_DEFAULTS = dict(fuzz_amount=0, fuzz_octave=32767, fuzz_tone=16384, fuzz_level=16384,
                   tape_drive=0, tape_tone=16384, tape_level=16384, tape_wow=0,
                   ens_mode=0, ens_mix=16384, ens_rate=16384, ens_depth=16384,
                   bbd_mix=0, bbd_time=9830, bbd_feedback=13107, bbd_mod=6553, bbd_tone=16384,
                   oil_mix=0, oil_time=16384, oil_feedback=22938, oil_warble=4915, oil_smear=3277,
                   rev_type=0, rev_hold=0, rev_amount=0, fx_sync=0)
SYNC_DIVS = [(1, 8), (1, 6), (1, 4), (1, 3), (3, 8), (1, 2), (2, 3), (3, 4), (1, 1), (3, 2), (2, 1), (4, 1), (8, 1)]  # beats


def fx_sync(bbd_div=None, oil_div=None):
    """mfe_fx_t.fx_sync: high nibble BBD ECHO division + 1, low nibble OIL CAN division + 1 (0 = free time)."""
    return ((0 if bbd_div is None else bbd_div + 1) << 4) | (0 if oil_div is None else oil_div + 1)


def nearest_delay_div(num, den):
    """The SYNC_DIVS index nearest num/den beats (ratio distance, exact integer maths; ties to the shorter)."""
    best, bn, bd = 0, None, None
    for i, (a, b) in enumerate(SYNC_DIVS):
        hi, lo = max(num * b, a * den), min(num * b, a * den)      # ratio hi/lo >= 1
        if bn is None or hi * bd < bn * lo:
            best, bn, bd = i, hi, lo
    return best

BODY = ("<24s4B10h" + "BBbb5h" * NOSC + "3h" + "BB4h" * NFILT + "3h" + "5hBB3h" * NENV
        + "BBhhBBBBh16B16b" * NLFO + "BBh" * NMOD + "8hBB13hBBh" + "bBBBBB4h")
SIZE = 8 + struct.calcsize(BODY) + 4


def default_patch():
    """Mirror of mfe_patch_init() in dsp/voices/mfe_engine.c."""
    return {
        "name": "INIT", "voice_mode": 0, "max_voices": 8, "bend_range": 2, "vel_sens": 64,
        "glide": 0, "master": 8192, "macro": [0] * NMACRO,
        "osc": [dict(table=6, flags=OSC_ON if i == 0 else 0, semis=0, drift=0, fine=0,
                     position=14043, warp=0, amp=26000, route=0) for i in range(NOSC)],
        "noise": dict(amp=0, color=0, route=0),
        "filt": [dict(type=1 if i == 0 else 0, cutoff=30000 if i == 0 else 0, res=0, drive=0, keytrack=0)
                 for i in range(NFILT)],
        "serpar": 0, "f1_mix": 32767, "f2_mix": 32767,
        "env": [dict(delay=0, attack=2000, decay=16000, sustain=32767, release=12000, flags=0,
                     att_level=32767, dec_level=-1, sust_time=0) for _ in range(NENV)],
        "lfo": [dict(wave=0, flags=0, rate=16000, phase=0, sync_div=8, loop_start=0, loop_end=15, glide=0,
                     step_shape=[0] * 16, step_level=[127] * 16) for _ in range(NLFO)],
        "mod": [dict(src=0, dst=0, amount=0) for _ in range(NMOD)],
        "modosc": dict(semis=0, rm_osc=0, pm_osc=0, pos_osc=0, ffm_filter=0, rm=0, pm=0, pos=0, ffm=0),
        "fx": dict(FX_DEFAULTS),
    }


# mfe_fx_t (dsp/include/mfe.h) in file order ("fx_sync" was the spare byte "ens_reserved": older files read 0 = free)
FX_ORDER = ["fuzz_amount", "fuzz_octave", "fuzz_tone", "fuzz_level",
            "tape_drive", "tape_tone", "tape_level", "tape_wow",
            "ens_mode", "fx_sync", "ens_mix", "ens_rate", "ens_depth",
            "bbd_mix", "bbd_time", "bbd_feedback", "bbd_mod", "bbd_tone",
            "oil_mix", "oil_time", "oil_feedback", "oil_warble", "oil_smear",
            "rev_type", "rev_hold", "rev_amount"]


def write(p) -> bytes:
    v = [p["name"].encode("ascii", "replace")[:23].ljust(24, b"\0"),
         p["voice_mode"], p["max_voices"], p["bend_range"], p["vel_sens"], p["glide"], p["master"], *p["macro"]]
    for o in p["osc"]:
        v += [o["table"], o["flags"], o["semis"], o.get("drift", 0), o["fine"], o["position"], o["warp"], o["amp"], o["route"]]
    v += [p["noise"]["amp"], p["noise"]["color"], p["noise"]["route"]]
    for f in p["filt"]:
        v += [f["type"], 0, f["cutoff"], f["res"], f["drive"], f["keytrack"]]
    v += [p["serpar"], p["f1_mix"], p["f2_mix"]]
    for e in p["env"]:
        v += [e["delay"], e["attack"], e["decay"], e["sustain"], e["release"], e["flags"], 0,
              e["att_level"], e["dec_level"], e["sust_time"]]
    for l in p["lfo"]:
        v += [l["wave"], l["flags"], l["rate"], l["phase"], l["sync_div"], l["loop_start"], l["loop_end"], 0,
              l["glide"], *l["step_shape"], *l["step_level"]]
    for m in p["mod"]:
        v += [m["src"], m["dst"], m["amount"]]
    v += [p["fx"].get(k, 0) if k == "fx_sync" else p["fx"][k] for k in FX_ORDER]
    mo = p["modosc"]
    v += [mo["semis"], mo["rm_osc"], mo["pm_osc"], mo["pos_osc"], mo["ffm_filter"], 0, mo["rm"], mo["pm"], mo["pos"], mo["ffm"]]
    body = struct.pack(BODY, *v)
    head = MAGIC + struct.pack("<HH", VERSION, SIZE - 12)
    data = head + body
    return data + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)


def read(buf: bytes):
    if len(buf) < SIZE:
        raise ValueError("short preset")
    if buf[:4] != MAGIC:
        raise ValueError("bad magic")
    ver, plen = struct.unpack_from("<HH", buf, 4)
    if ver != VERSION or plen != SIZE - 12:
        raise ValueError("unsupported preset version")
    if zlib.crc32(buf[:SIZE - 4]) & 0xFFFFFFFF != struct.unpack_from("<I", buf, SIZE - 4)[0]:
        raise ValueError("CRC mismatch")
    v = list(struct.unpack_from(BODY, buf, 8))
    p = default_patch()
    p["name"] = v.pop(0).split(b"\0")[0].decode("ascii", "replace")
    for k in ("voice_mode", "max_voices", "bend_range", "vel_sens", "glide", "master"):
        p[k] = v.pop(0)
    p["macro"] = [v.pop(0) for _ in range(NMACRO)]
    for o in p["osc"]:
        o["table"], o["flags"], o["semis"], o["drift"], o["fine"], o["position"], o["warp"], o["amp"], o["route"] = \
            [v.pop(0) for _ in range(9)]
    p["noise"] = dict(amp=v.pop(0), color=v.pop(0), route=v.pop(0))
    for f in p["filt"]:
        f["type"], _, f["cutoff"], f["res"], f["drive"], f["keytrack"] = [v.pop(0) for _ in range(6)]
    p["serpar"], p["f1_mix"], p["f2_mix"] = v.pop(0), v.pop(0), v.pop(0)
    for e in p["env"]:
        (e["delay"], e["attack"], e["decay"], e["sustain"], e["release"], e["flags"], _,
         e["att_level"], e["dec_level"], e["sust_time"]) = [v.pop(0) for _ in range(10)]
    for l in p["lfo"]:
        l["wave"], l["flags"], l["rate"], l["phase"] = [v.pop(0) for _ in range(4)]
        l["sync_div"], l["loop_start"], l["loop_end"], _, l["glide"] = [v.pop(0) for _ in range(5)]
        l["step_shape"] = [v.pop(0) for _ in range(16)]
        l["step_level"] = [v.pop(0) for _ in range(16)]
    for m in p["mod"]:
        m["src"], m["dst"], m["amount"] = [v.pop(0) for _ in range(3)]
    p["fx"] = {k: v.pop(0) for k in FX_ORDER}
    mo = p["modosc"]
    mo["semis"], mo["rm_osc"], mo["pm_osc"], mo["pos_osc"], mo["ffm_filter"], _ = [v.pop(0) for _ in range(6)]
    mo["rm"], mo["pm"], mo["pos"], mo["ffm"] = [v.pop(0) for _ in range(4)]
    return p
