#!/usr/bin/env python3
"""Try to link every compiled decomp object and report what is missing.

This is the milestone-2 progress meter: the number of undefined symbols, grouped
by where they belong (Switch SDK area, graphics, C library, other), plus any
symbols defined more than once. Build the objects first:

    cmake --build build/macos --target d3_decomp -- -k 0
    python3 tools/link_probe.py
    python3 tools/gen_stubs.py                    # stand-ins for missing SDK functions
    python3 tools/link_probe.py --list nn::fs     # every missing symbol in a group

Results go to build/probe/ (undefined_symbols.tsv, undefined.txt, link_summary.json).
"""
import argparse, collections, json, re, shutil, subprocess, sys
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
    # The platform layer is small: rebuild it from nothing so that objects of deleted
    # source files cannot linger and hide a missing symbol.
    shutil.rmtree(Path(a.build) / "CMakeFiles/d3_platform.dir", ignore_errors=True)
    subprocess.run(["cmake", a.build], capture_output=True)
    targets = ["d3_decomp", "d3_platform", "d3_replace"] + (["d3_extracted"] if (Path(a.build) / "CMakeFiles/d3_extracted.dir").parent.is_dir()
                                 and "d3_extracted" in (Path(a.build) / "build.ninja").read_text() else [])
    b = subprocess.run(["cmake", "--build", a.build, "--target"] + targets, capture_output=True, text=True)
    if b.returncode:
        sys.exit("platform layer failed to build:\n" + b.stdout[-3000:])
    # Use the program's own link command (objects, libraries and frameworks), so that
    # nothing the real link resolves is reported as missing here.
    cmd = subprocess.run(["ninja", "-C", a.build, "-t", "commands", "d3"], capture_output=True, text=True).stdout.strip().splitlines()
    if not cmd:
        sys.exit("no link command for d3: is the game dump present and the build configured?")
    link = cmd[-1].replace("-o d3 ", "-o " + str(ROOT / "build/probe/linktest") + " ")
    out = ROOT / "build/probe"
    out.mkdir(parents=True, exist_ok=True)
    objs = [w for w in link.split() if w.endswith(".o")]
    objs = [w if w.startswith("/") else str(Path(a.build) / w) for w in objs]
    r = subprocess.run(link, shell=True, cwd=a.build, capture_output=True, text=True)
    undefined, duplicate = [], []
    for line in r.stderr.splitlines():
        m = re.match(r'\s+"(.+)", referenced from:', line)
        if m:
            undefined.append(m.group(1))
        m = re.match(r"duplicate symbol '(.+)' in:", line)
        if m:
            duplicate.append(m.group(1))
    # The linker prints readable names. Recover the linker names from the objects:
    # every symbol some object needs, kept if the linker reported it missing.
    need = set()
    for i in range(0, len(objs), 200):
        nm = subprocess.run(["nm", "-u", "-j"] + objs[i:i + 200], capture_output=True, text=True).stdout
        need.update(l for l in nm.splitlines() if l.startswith("_"))
    need = sorted(need)
    pretty = subprocess.run(["c++filt"], input="\n".join(need), capture_output=True, text=True).stdout.splitlines()
    reported = set(undefined)
    missing = [(m[1:], p) for m, p in zip(need, pretty) if p in reported or m in reported]
    (out / "undefined_symbols.tsv").write_text("".join(f"{m}\t{p}\n" for m, p in missing))
    dem = [p for _, p in missing]
    groups = collections.defaultdict(list)
    for n in dem:
        groups[group(n)].append(n)
    (out / "undefined.txt").write_text("\n".join(sorted(dem)) + "\n")
    (out / "link_stderr.txt").write_text(r.stderr)
    (out / "duplicate_symbols.txt").write_text("".join(d + "\n" for d in duplicate))
    print(f"{len(objs)} objects; link {'succeeded' if r.returncode == 0 else 'failed'}")
    print(f"{len(missing)} undefined symbols, {len(duplicate)} defined more than once")
    for g, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):5}  {g}")
        if a.list and a.list in g:
            for n in sorted(v):
                print("           ", n[:150])
    for d in duplicate[:20]:
        print("  duplicate:", d)
    json.dump({"objects": len(objs), "undefined": len(missing), "duplicate": duplicate,
               "groups": {g: len(v) for g, v in groups.items()}}, (out / "link_summary.json").open("w"), indent=1)


if __name__ == "__main__":
    main()
