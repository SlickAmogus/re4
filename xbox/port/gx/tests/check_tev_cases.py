#!/usr/bin/env python3
"""CT-05: validate a TEV catalogue JSON (default xbox/port/gx/tests/tev_cases.json).

Checks: schema (tev_cases.schema.json next to this script), unique case ids, every "GX_"-prefixed
string names an identifier in include/dolphin/gx/GXEnum.h ("A|B" light masks allowed, strings starting
with '?' are runtime-only values and are skipped), then prints totals. Exit 1 on any problem.
"""
import json, os, re, sys

import jsonschema

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "tev_cases.json")
data = json.load(open(path))
jsonschema.validate(data, json.load(open(os.path.join(HERE, "tev_cases.schema.json"))))
enums = set(re.findall(r"\b(GX_[A-Za-z0-9_]+)\b", open(os.path.join(ROOT, "include/dolphin/gx/GXEnum.h")).read()))
problems = []
ids = [c["id"] for c in data["cases"]]
problems += ["duplicate id %s" % i for i in sorted({i for i in ids if ids.count(i) > 1})]


def walk(v, where):
    if isinstance(v, dict):
        for k, x in v.items():
            walk(x, where + "." + k)
    elif isinstance(v, list):
        for i, x in enumerate(v):
            walk(x, "%s[%d]" % (where, i))
    elif isinstance(v, str) and re.fullmatch(r"GX_\w+(\|GX_\w+)*", v):  # enum-looking: the whole string is GX identifiers
        for part in v.split("|"):
            if part not in enums:
                problems.append("%s: unknown enum %r" % (where, part))


for c in data["cases"]:
    walk(c, c["id"])
hist = {}
for c in data["cases"]:
    n = c["num_tev_stages"] if isinstance(c["num_tev_stages"], int) else "runtime"
    hist[n] = hist.get(n, 0) + 1
print("cases: %d" % len(data["cases"]))
print("stage-count histogram: " + ", ".join("%s:%d" % (k, hist[k]) for k in sorted(hist, key=str)))
print("max textures bound in one case: %d" % max(c["textures_bound"] for c in data["cases"]))
for p in problems:
    print("PROBLEM: " + p)
sys.exit(1 if problems else 0)
