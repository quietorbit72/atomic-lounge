# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: build the one-file offline browser editor editor/converter.html from editor/src/
# (template.html, style.css, mf_core.js, mf_protocol.js, app.js) and the mapping profiles in
# tools/preset_converter/maps/*.json (embedded verbatim, sorted by file name like convert.py).
#
#   python -I tools/build_editor.py [-o editor/converter.html]
import argparse
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC = os.path.join(ROOT, "editor", "src")
MAPS = os.path.join(ROOT, "tools", "preset_converter", "maps")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def script_safe(text):
    # nothing inside a <script> may close it early
    return text.replace("</script", "<\\/script").replace("</SCRIPT", "<\\/SCRIPT")


def profiles_json():
    items = []
    for fn in sorted(os.listdir(MAPS)):
        if fn.endswith(".json"):
            items.append({"name": fn, "profile": json.loads(read(os.path.join(MAPS, fn)))})
    # separators keep it compact; floats are written with repr, which round-trips exactly
    text = json.dumps(items, ensure_ascii=True, separators=(",", ":"))
    return text.replace("</", "<\\/"), len(items)


def build(out):
    html = read(os.path.join(SRC, "template.html"))
    prof, n = profiles_json()
    parts = {
        "/*CSS*/": read(os.path.join(SRC, "style.css")),
        "/*PROFILES*/": prof,
        "/*CORE*/": script_safe(read(os.path.join(SRC, "mf_core.js"))),
        "/*PROTOCOL*/": script_safe(read(os.path.join(SRC, "mf_protocol.js"))),
        "/*APP*/": script_safe(read(os.path.join(SRC, "app.js"))),
    }
    for key, value in parts.items():
        if html.count(key) != 1:
            raise SystemExit(f"template must contain {key} exactly once")
        html = html.replace(key, value, 1)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    return n, len(html.encode("utf-8"))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build editor/converter.html")
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "editor", "converter.html"))
    a = ap.parse_args(argv)
    n, size = build(a.output)
    print(f"built {a.output}: {size} bytes, {n} mapping profiles embedded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
