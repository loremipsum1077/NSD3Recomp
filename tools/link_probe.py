#!/usr/bin/env python3
"""Try to link every compiled decomp object and report what is missing.

This is the milestone-2 progress meter: the number of undefined symbols, grouped
by where they belong (Switch SDK area, graphics, C library, other), plus any
symbols defined more than once. Build the objects first:

    cmake --build build/macos --target d3_game d3_middleware -- -k 0
    python3 tools/link_probe.py
    python3 tools/link_probe.py --list nn::fs     # every missing symbol in a group

Results go to build/probe/ (undefined.txt, link_summary.json).
"""
import argparse, collections, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def group(name):
    m = re.match(r"(?:non-virtual thunk to |vtable for |typeinfo for |guard variable for )?(nn::\w+|nvn\w*|nn\w+)", name)
    if m:
        g = m.group(1)
        return "nvn (graphics)" if g.startswith("nvn") else (g if "::" in g else "nn (C entry points)")
    if "::" not in name.split("(")[0]:
        return "plain C names"
    return name.split("(")[0].split("<")[0].rsplit("::", 1)[0].split(" ")[-1].split("::")[0] + ":: (game or middleware)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(ROOT / "build/macos"))
    ap.add_argument("--list", help="print every undefined symbol whose group contains this text")
    a = ap.parse_args()
    objs = sorted(str(p) for t in ("d3_game", "d3_middleware")
                  for p in (Path(a.build) / "CMakeFiles" / f"{t}.dir").rglob("*.o"))
    if not objs:
        sys.exit("no objects: build d3_game and d3_middleware first")
    out = ROOT / "build/probe"
    out.mkdir(parents=True, exist_ok=True)
    (out / "objects.txt").write_text("\n".join(objs) + "\n")
    r = subprocess.run(["c++", "-Wl,-filelist," + str(out / "objects.txt"), "-o", str(out / "linktest")],
                       capture_output=True, text=True)
    undefined, duplicate = [], []
    for line in r.stderr.splitlines():
        m = re.match(r'\s+"(.+)", referenced from:', line)
        if m:
            undefined.append(m.group(1))
        m = re.match(r"duplicate symbol '(.+)' in:", line)
        if m:
            duplicate.append(m.group(1))
    dem = subprocess.run(["c++filt"], input="\n".join(undefined), capture_output=True, text=True).stdout.splitlines()
    groups = collections.defaultdict(list)
    for n in dem:
        groups[group(n)].append(n)
    (out / "undefined.txt").write_text("\n".join(sorted(dem)) + "\n")
    (out / "link_stderr.txt").write_text(r.stderr)
    print(f"{len(objs)} objects; link {'succeeded' if r.returncode == 0 else 'failed'}")
    print(f"{len(undefined)} undefined symbols, {len(duplicate)} defined more than once")
    for g, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):5}  {g}")
        if a.list and a.list in g:
            for n in sorted(v):
                print("           ", n[:150])
    for d in duplicate[:20]:
        print("  duplicate:", d)
    json.dump({"objects": len(objs), "undefined": len(undefined), "duplicate": duplicate,
               "groups": {g: len(v) for g, v in groups.items()}}, (out / "link_summary.json").open("w"), indent=1)


if __name__ == "__main__":
    main()
