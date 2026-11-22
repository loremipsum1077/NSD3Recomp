#!/usr/bin/env python3
"""Syntax-check every decomp source file with the host compiler.

This is the milestone-1 progress meter: it says how many of the decomp's
translation units the host toolchain accepts, and groups the failures by their
first error so the most common blocker is fixed first. Nothing is linked.

The compile commands come from the CMake build (compile_commands.json), so the
probe and the real build always use the same flags. Configure first:

    cmake --preset macos
    python3 tools/compile_probe.py                # whole tree
    python3 tools/compile_probe.py X/Common       # one area
    python3 tools/compile_probe.py --show 'unknown type name'

Results go to build/probe/ (summary.json, errors/<file>.txt).
"""
import argparse, collections, concurrent.futures, json, os, re, shlex, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def commands(build):
    db = build / "compile_commands.json"
    if not db.is_file():
        sys.exit(f"{db} not found: run `cmake --preset macos` first")
    out = {}
    for e in json.loads(db.read_text()):
        args = shlex.split(e["command"]) if "command" in e else e["arguments"]
        keep, skip = [], 0
        for a in args:
            if skip:
                skip -= 1
            elif a in ("-o", "-MF", "-MT", "-MQ"):
                skip = 1
            elif a not in ("-c", "-MD", "-MMD"):
                keep.append(a)
        out[Path(e["file"])] = (keep + ["-fsyntax-only", "-ferror-limit=5"], e["directory"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("area", nargs="?", default="")
    ap.add_argument("--build", default=str(ROOT / "build/macos"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--show", help="list files whose first error contains this text")
    a = ap.parse_args()
    cmds = {p: c for p, c in commands(Path(a.build)).items() if "/src/" in str(p) and ROOT not in p.parents}
    if not cmds:
        sys.exit("no decomp sources in compile_commands.json")
    src = Path(os.path.commonpath([str(p) for p in cmds]))
    while src.name != "src":
        src = src.parent
    files = sorted(p for p in cmds if str(p.relative_to(src)).startswith(a.area))
    out = ROOT / "build/probe"
    (out / "errors").mkdir(parents=True, exist_ok=True)
    ok, first, by_area = 0, collections.defaultdict(list), collections.defaultdict(lambda: [0, 0])

    def check(path):
        args, cwd = cmds[path]
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
        return path, r.returncode, r.stderr

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for path, rc, err in ex.map(check, files):
            rel = path.relative_to(src)
            area = "/".join(rel.parts[:2])
            by_area[area][1] += 1
            log = out / "errors" / (str(rel).replace("/", "__") + ".txt")
            if rc == 0:
                ok += 1
                by_area[area][0] += 1
                log.unlink(missing_ok=True)
                continue
            log.write_text(err)
            m = re.search(r"(?:fatal )?error: (.*)", err)
            key = re.sub(r"'[^']*'", "'…'", m.group(1)) if m else "(no diagnostic)"
            first[key].append(str(rel))
    print(f"{ok}/{len(files)} files accepted by the host compiler")
    for area, (n, t) in sorted(by_area.items()):
        print(f"  {area:32} {n:5}/{t}")
    print("\nFirst error per failing file, most common first:")
    for k, v in sorted(first.items(), key=lambda kv: -len(kv[1]))[:25]:
        print(f"  {len(v):5}  {k[:110]}")
        if a.show and a.show in k:
            for f in v:
                print("           ", f)
    if not a.area:
        json.dump({"ok": ok, "total": len(files), "areas": by_area, "first_error": first},
                  (out / "summary.json").open("w"), indent=1)


if __name__ == "__main__":
    main()
