# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: the differential workflow for mapping Massive parameters.
#
#   python nmsv_diff.py diff BASE.nmsv CHANGED.nmsv
#       which floats and which raw state bytes differ
#   python nmsv_diff.py record BASE.nmsv CHANGED.nmsv --param "OSC1 Wt-position" \
#          --base-value 0 --changed-value 100 [--db observations.jsonl]
#       the same, appended as one observation (one parameter changed in Massive between the saves)
#   python nmsv_diff.py summarize [--db observations.jsonl]
#       for each parameter, the float index that changed in every observation of it, with the
#       fitted line (serialized = a * displayed + b): candidate entries for maps/*.json
#
# Workflow (docs/MASSIVE_PRESET_FORMAT.md): save a baseline in Massive, change exactly one
# control, save again, record; repeat with two or more known values per control.
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import massive  # noqa: E402
import base64  # noqa: E402

DEFAULT_DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "observations.jsonl")


def diff(a, b):
    sa, sb = a["state"] or {}, b["state"] or {}
    fa, fb = sa.get("float_block", {}).get("values", []), sb.get("float_block", {}).get("values", [])
    floats = [{"index": i, "base": fa[i] if i < len(fa) else None, "changed": fb[i] if i < len(fb) else None}
              for i in range(max(len(fa), len(fb)))
              if (fa[i] if i < len(fa) else None) != (fb[i] if i < len(fb) else None)]
    ra = base64.b64decode(sa.get("raw_b64", "")) if sa.get("raw_b64") else b""
    rb = base64.b64decode(sb.get("raw_b64", "")) if sb.get("raw_b64") else b""
    ranges, start = [], None
    for i in range(max(len(ra), len(rb))):
        d = (ra[i] if i < len(ra) else None) != (rb[i] if i < len(rb) else None)
        if d and start is None:
            start = i
        if not d and start is not None:
            ranges.append([start, i])
            start = None
    if start is not None:
        ranges.append([start, max(len(ra), len(rb))])
    float_end = max(sa.get("float_block", {}).get("end", 0), sb.get("float_block", {}).get("end", 0))
    tail = [r for r in ranges if r[1] > float_end]
    return {"floats": floats, "byte_ranges": ranges, "tail_ranges": tail,
            "size": [len(ra), len(rb)], "macro_names": [sa.get("macro_names"), sb.get("macro_names")]}


def summarize(db):
    by = {}
    with open(db, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                o = json.loads(line)
                by.setdefault(o["param"], []).append(o)
    out = []
    for param, obs in by.items():
        sets = [set(x["index"] for x in o["diff"]["floats"]) for o in obs]
        common = set.intersection(*sets) if sets else set()
        ent = {"param": param, "observations": len(obs), "indices": sorted(common)}
        if len(common) == 1:
            i = common.pop()
            pts = []
            for o in obs:
                fl = next(x for x in o["diff"]["floats"] if x["index"] == i)
                pts += [(o["base_value"], fl["base"]), (o["changed_value"], fl["changed"])]
            pts = sorted(set(p for p in pts if None not in p))
            if len(pts) >= 2:
                n = len(pts)
                mx, my = sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n
                sxx = sum((p[0] - mx) ** 2 for p in pts)
                a = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx if sxx else 0
                b = my - a * mx
                err = max(abs(a * p[0] + b - p[1]) for p in pts)
                ent.update(fit={"a": a, "b": b, "max_error": err, "points": pts},
                           status="verified-linear" if len(pts) >= 3 and err < 1e-3 else "needs-more-points")
        else:
            ent["status"] = "ambiguous" if common else "no-common-index (tail data?)"
        out.append(ent)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("diff"); d.add_argument("base"); d.add_argument("changed")
    r = sub.add_parser("record"); r.add_argument("base"); r.add_argument("changed")
    r.add_argument("--param", required=True); r.add_argument("--base-value", type=float, required=True)
    r.add_argument("--changed-value", type=float, required=True); r.add_argument("--db", default=DEFAULT_DB)
    r.add_argument("--quiet", action="store_true")
    s = sub.add_parser("summarize"); s.add_argument("--db", default=DEFAULT_DB)
    a = ap.parse_args(argv)
    if a.cmd == "summarize":
        print(json.dumps(summarize(a.db), indent=1))
        return 0
    da, db_ = massive.decode_file(a.base), massive.decode_file(a.changed)
    res = diff(da, db_)
    if a.cmd == "record":
        rec = {"param": a.param, "base_value": a.base_value, "changed_value": a.changed_value,
               "base_sha256": da["source"]["sha256"], "changed_sha256": db_["source"]["sha256"], "diff": res}
        with open(a.db, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    if not getattr(a, "quiet", False):
        print(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
