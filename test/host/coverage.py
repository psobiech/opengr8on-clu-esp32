#!/usr/bin/env python3
"""Coverage report for the host tests (run by `make coverage`; stdlib only).

C: gcov JSON for every .gcda in this directory, merged per source file (a source compiled into
several test binaries counts a line as covered if any of them ran it). Lua: line hits written by
lua_cov.lua, measured against the lines `luac -l` reports as executable.

Firmware sources (components/, main/) that no test compiles are listed as untested; the Lua VM
(components/lua) is third-party and excluded.

Outputs: table on stdout, coverage/summary.md, coverage/coverage.json.
"""
import argparse
import glob
import gzip
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "coverage")


def mqtt_parts():
    manifest = os.path.join(ROOT, "components", "clu_mqtt", "lua", "parts.txt")
    parts, offset = [], 0
    with open(manifest) as fh:
        for name in fh:
            name = name.strip()
            if not name:
                continue
            path = os.path.join("components", "clu_mqtt", "lua", name)
            with open(os.path.join(ROOT, path), "rb") as part:
                count = len(part.read().splitlines())
            parts.append((path, offset, count))
            offset += count
    return parts


MQTT_PARTS = mqtt_parts()
LUA_SOURCES = ["components/clu_lua/prelude.lua", "components/clu_lua/objects.lua",
               *(path for path, _, _ in MQTT_PARTS), "components/clu_mysensors/mysensors.lua"]
MQTT_BUNDLE = "components/clu_mqtt/mqtt.lua"


def rel(path, cwd):
    p = os.path.normpath(os.path.join(cwd, path))
    return os.path.relpath(p, ROOT)


def firmware_source(r):
    return (r.startswith("components/") or r.startswith("main/")) and not r.startswith("components/lua/")


def c_coverage():
    lines = defaultdict(dict)
    branches = defaultdict(dict)
    funcs = defaultdict(dict)
    gcdas = sorted(glob.glob(os.path.join(HERE, "*.gcda")))
    if not gcdas:
        return {}
    tmp = os.path.join(OUT, "gcov")
    os.makedirs(tmp, exist_ok=True)
    for f in glob.glob(os.path.join(tmp, "*")):
        os.remove(f)
    subprocess.run(["gcov", "-b", "-j", "-o", HERE] + gcdas, cwd=tmp, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for path in glob.glob(os.path.join(tmp, "*.gcov.json.gz")):
        with gzip.open(path) as fh:
            data = json.load(fh)
        cwd = data.get("current_working_directory", HERE)
        for f in data["files"]:
            r = rel(f["file"], cwd)
            if not firmware_source(r):
                continue
            for ln in f["lines"]:
                n = ln["line_number"]
                lines[r][n] = lines[r].get(n, 0) + ln["count"]
                for i, b in enumerate(ln.get("branches", [])):
                    branches[r][(n, i)] = branches[r].get((n, i), 0) + b["count"]
            for fn in f.get("functions", []):
                name = fn.get("demangled_name", fn["name"])
                funcs[r][name] = funcs[r].get(name, 0) + fn["execution_count"]
    res = {}
    for r in lines:
        L, B, F = lines[r], branches[r], funcs[r]
        res[r] = {
            "lines": [sum(v > 0 for v in L.values()), len(L)],
            "branches": [sum(v > 0 for v in B.values()), len(B)],
            "functions": [sum(v > 0 for v in F.values()), len(F)],
            "uncovered_lines": sorted(n for n, v in L.items() if v == 0),
            "uncalled": sorted(n for n, v in F.items() if v == 0),
        }
    return res


def lua_coverage(luac):
    hits_file = os.path.join(OUT, "lua.hits")
    hits = defaultdict(set)
    if os.path.exists(hits_file):
        with open(hits_file) as fh:
            for row in fh:
                src, ln = row.rstrip("\n").split("\t")
                hits[rel(src, HERE)].add(int(ln))
    res = {}
    mqtt_hits = hits[MQTT_BUNDLE]
    mqtt_offsets = {path: (offset, count) for path, offset, count in MQTT_PARTS}
    for r in LUA_SOURCES:
        out = subprocess.run([luac, "-l", "-l", "-p", os.path.join(ROOT, r)], capture_output=True,
                             text=True, check=True).stdout
        exe = {int(m) for m in re.findall(r"^\s*\d+\s+\[(\d+)\]", out, re.M)}
        if r in mqtt_offsets:
            offset, count = mqtt_offsets[r]
            hit = {line - offset for line in mqtt_hits if offset < line <= offset + count} & exe
        else:
            hit = hits[r] & exe
        res[r] = {"lines": [len(hit), len(exe)], "uncovered_lines": sorted(exe - hit)}
    return res


def untested_sources(tested):
    found = []
    for pat in ("components/*/*.c", "main/*.c"):
        for p in glob.glob(os.path.join(ROOT, pat)):
            r = os.path.relpath(p, ROOT)
            if firmware_source(r) and r not in tested:
                found.append(r)
    return sorted(found)


def pct(a, b):
    return 100.0 * a / b if b else 100.0


def ranges(nums):
    out, start, prev = [], None, None
    for n in nums:
        if start is None:
            start = prev = n
        elif n <= prev + 2:
            prev = n
        else:
            out.append(f"{start}-{prev}" if start != prev else str(start))
            start = prev = n
    if start is not None:
        out.append(f"{start}-{prev}" if start != prev else str(start))
    return ",".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--luac", default="luac5.3")
    ap.add_argument("--fail-under", type=float, help="minimum total line coverage (%%)")
    ap.add_argument("-v", "--verbose", action="store_true", help="list uncovered lines")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    c = c_coverage()
    lua = lua_coverage(args.luac)
    untested = untested_sources(c)

    tot = {"c_lines": [0, 0], "c_branches": [0, 0], "lua_lines": [0, 0]}
    rows = []
    for r in sorted(c):
        d = c[r]
        tot["c_lines"] = [a + b for a, b in zip(tot["c_lines"], d["lines"])]
        tot["c_branches"] = [a + b for a, b in zip(tot["c_branches"], d["branches"])]
        rows.append((r, d["lines"], d["branches"], d["functions"]))
    for r in LUA_SOURCES:
        d = lua[r]
        tot["lua_lines"] = [a + b for a, b in zip(tot["lua_lines"], d["lines"])]
        rows.append((r, d["lines"], None, None))
    all_lines = [tot["c_lines"][0] + tot["lua_lines"][0], tot["c_lines"][1] + tot["lua_lines"][1]]

    def cell(v):
        return f"{pct(*v):.1f}% ({v[0]}/{v[1]})" if v else "-"

    w = max(len(r[0]) for r in rows)
    print(f"{'file':{w}}  {'lines':>18}  {'branches':>18}  {'functions':>18}")
    for r, ln, br, fn in rows:
        print(f"{r:{w}}  {cell(ln):>18}  {cell(br):>18}  {cell(fn):>18}")
        if args.verbose:
            d = c.get(r) or lua.get(r)
            if d["uncovered_lines"]:
                print(f"    uncovered: {ranges(d['uncovered_lines'])}")
            if d.get("uncalled"):
                print(f"    uncalled:  {', '.join(d['uncalled'])}")
    print(f"{'C total':{w}}  {cell(tot['c_lines']):>18}  {cell(tot['c_branches']):>18}")
    print(f"{'Lua total':{w}}  {cell(tot['lua_lines']):>18}")
    print(f"{'TOTAL':{w}}  {cell(all_lines):>18}")
    if untested:
        print("\nNo host test compiles: " + ", ".join(untested))

    md = ["| File | Lines | Branches | Functions |", "|---|---:|---:|---:|"]
    for r, ln, br, fn in rows:
        md.append(f"| `{r}` | {cell(ln)} | {cell(br)} | {cell(fn)} |")
    md.append(f"| **C total** | **{cell(tot['c_lines'])}** | **{cell(tot['c_branches'])}** | |")
    md.append(f"| **Lua total** | **{cell(tot['lua_lines'])}** | | |")
    md.append(f"| **Total** | **{cell(all_lines)}** | | |")
    if untested:
        md.append("\nNot compiled by any host test: " + ", ".join(f"`{u}`" for u in untested))
    with open(os.path.join(OUT, "summary.md"), "w") as fh:
        fh.write("\n".join(md) + "\n")
    with open(os.path.join(OUT, "coverage.json"), "w") as fh:
        json.dump({"c": c, "lua": lua, "untested": untested, "totals": {**tot, "lines": all_lines}},
                  fh, indent=1)

    if args.fail_under is not None and pct(*all_lines) < args.fail_under:
        print(f"\nFAIL: total line coverage {pct(*all_lines):.1f}% < {args.fail_under}%")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
