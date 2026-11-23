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
    with open(manifest) as manifest_file:
        for name in manifest_file:
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
    absolute_path = os.path.normpath(os.path.join(cwd, path))
    return os.path.relpath(absolute_path, ROOT)


def firmware_source(relative_path):
    return (relative_path.startswith("components/") or relative_path.startswith("main/")) and not relative_path.startswith("components/lua/")


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
        with gzip.open(path) as gcov_file:
            data = json.load(gcov_file)
        cwd = data.get("current_working_directory", HERE)
        for f in data["files"]:
            source_path = rel(f["file"], cwd)
            if not firmware_source(source_path):
                continue
            for ln in f["lines"]:
                line_number = ln["line_number"]
                lines[source_path][line_number] = lines[source_path].get(line_number, 0) + ln["count"]
                for i, b in enumerate(ln.get("branches", [])):
                    branches[source_path][(line_number, i)] = branches[source_path].get((line_number, i), 0) + b["count"]
            for fn in f.get("functions", []):
                name = fn.get("demangled_name", fn["name"])
                funcs[source_path][name] = funcs[source_path].get(name, 0) + fn["execution_count"]
    res = {}
    for r in lines:
        line_counts, branch_counts, function_counts = lines[r], branches[r], funcs[r]
        res[r] = {
            "lines": [sum(v > 0 for v in line_counts.values()), len(line_counts)],
            "branches": [sum(v > 0 for v in branch_counts.values()), len(branch_counts)],
            "functions": [sum(v > 0 for v in function_counts.values()), len(function_counts)],
            "uncovered_lines": sorted(n for n, v in line_counts.items() if v == 0),
            "uncalled": sorted(n for n, v in function_counts.items() if v == 0),
        }
    return res


def lua_coverage(luac):
    hits_file = os.path.join(OUT, "lua.hits")
    hits = defaultdict(set)
    if os.path.exists(hits_file):
        with open(hits_file) as hits_rows:
            for row in hits_rows:
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
        for path in glob.glob(os.path.join(ROOT, pat)):
            relative_path = os.path.relpath(path, ROOT)
            if firmware_source(relative_path) and relative_path not in tested:
                found.append(relative_path)
    return sorted(found)


def pct(covered, total):
    return 100.0 * covered / total if total else 100.0


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
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--luac", default="luac5.3")
    parser.add_argument("--fail-under", type=float, help="minimum total line coverage (%%)")
    parser.add_argument("-v", "--verbose", action="store_true", help="list uncovered lines")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)

    c_results = c_coverage()
    lua = lua_coverage(args.luac)
    untested = untested_sources(c_results)

    tot = {"c_lines": [0, 0], "c_branches": [0, 0], "lua_lines": [0, 0]}
    rows = []
    for r in sorted(c_results):
        file_coverage = c_results[r]
        tot["c_lines"] = [a + b for a, b in zip(tot["c_lines"], file_coverage["lines"])]
        tot["c_branches"] = [a + b for a, b in zip(tot["c_branches"], file_coverage["branches"])]
        rows.append((r, file_coverage["lines"], file_coverage["branches"], file_coverage["functions"]))
    for r in LUA_SOURCES:
        file_coverage = lua[r]
        tot["lua_lines"] = [a + b for a, b in zip(tot["lua_lines"], file_coverage["lines"])]
        rows.append((r, file_coverage["lines"], None, None))
    all_lines = [tot["c_lines"][0] + tot["lua_lines"][0], tot["c_lines"][1] + tot["lua_lines"][1]]

    def cell(counts):
        return f"{pct(*counts):.1f}% ({counts[0]}/{counts[1]})" if counts else "-"

    name_width = max(len(r[0]) for r in rows)
    print(f"{'file':{name_width}}  {'lines':>18}  {'branches':>18}  {'functions':>18}")
    for r, ln, br, fn in rows:
        print(f"{r:{name_width}}  {cell(ln):>18}  {cell(br):>18}  {cell(fn):>18}")
        if args.verbose:
            file_coverage = c_results.get(r) or lua.get(r)
            if file_coverage["uncovered_lines"]:
                print(f"    uncovered: {ranges(file_coverage['uncovered_lines'])}")
            if file_coverage.get("uncalled"):
                print(f"    uncalled:  {', '.join(file_coverage['uncalled'])}")
    print(f"{'C total':{name_width}}  {cell(tot['c_lines']):>18}  {cell(tot['c_branches']):>18}")
    print(f"{'Lua total':{name_width}}  {cell(tot['lua_lines']):>18}")
    print(f"{'TOTAL':{name_width}}  {cell(all_lines):>18}")
    if untested:
        print("\nNo host test compiles: " + ", ".join(untested))

    markdown_rows = ["| File | Lines | Branches | Functions |", "|---|---:|---:|---:|"]
    for r, ln, br, fn in rows:
        markdown_rows.append(f"| `{r}` | {cell(ln)} | {cell(br)} | {cell(fn)} |")
    markdown_rows.append(f"| **C total** | **{cell(tot['c_lines'])}** | **{cell(tot['c_branches'])}** | |")
    markdown_rows.append(f"| **Lua total** | **{cell(tot['lua_lines'])}** | | |")
    markdown_rows.append(f"| **Total** | **{cell(all_lines)}** | | |")
    if untested:
        markdown_rows.append("\nNot compiled by any host test: " + ", ".join(f"`{u}`" for u in untested))
    with open(os.path.join(OUT, "summary.md"), "w") as output_file:
        output_file.write("\n".join(markdown_rows) + "\n")
    with open(os.path.join(OUT, "coverage.json"), "w") as output_file:
        json.dump({"c": c_results, "lua": lua, "untested": untested, "totals": {**tot, "lines": all_lines}},
                  output_file, indent=1)

    if args.fail_under is not None and pct(*all_lines) < args.fail_under:
        print(f"\nFAIL: total line coverage {pct(*all_lines):.1f}% < {args.fail_under}%")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
