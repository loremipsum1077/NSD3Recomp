#!/usr/bin/env python3
"""Syntax-check every decomp source file with the host compiler.

This is the milestone-1 progress meter: it says how many of the decomp's
translation units the host toolchain accepts, and groups the failures by their
first error so the most common blocker is fixed first. Nothing is linked.

    python3 tools/compile_probe.py                # whole tree
    python3 tools/compile_probe.py X/Common       # one area
    python3 tools/compile_probe.py --show 'unknown type name'

Results go to build/probe/ (summary.json, errors/<file>.txt).
"""
import argparse, collections, concurrent.futures, json, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def decomp_dir():
    d = os.environ.get("D3_DECOMP_DIR")
    for c in ([Path(d)] if d else []) + [ROOT / "extern/NSD3Decomp", ROOT.parent / "NSD3Decomp"]:
        if (c / "src/X").is_dir():
            return c.resolve()
    sys.exit("decomp source not found: set D3_DECOMP_DIR (see README.md)")


def flags(dec, path):
    cxx = path.suffix in (".cpp", ".cc")
    f = ["-fsyntax-only", "-w", "-ferror-limit=5", "-fsigned-char", "-fno-strict-aliasing",
         "-DNDEBUG", "-DNON_MATCHING", "-I", str(ROOT / "src/compat"),
         "-I", str(dec / "include"), "-I", str(dec / "src")]
    if cxx:
        f += ["-std=c++11", "-fno-exceptions", "-fno-rtti"]
    else:
        f += ["-std=gnu89" if "/lua/" in str(path) else "-std=gnu99"]
    return (["c++"] if cxx else ["cc"]) + f + [str(path)]


def check(dec, path):
    r = subprocess.run(flags(dec, path), capture_output=True, text=True)
    return path, r.returncode, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("area", nargs="?", default="")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--show", help="list files whose first error contains this text")
    a = ap.parse_args()
    dec = decomp_dir()
    base = dec / "src" / a.area
    files = sorted(p for p in base.rglob("*") if p.suffix in (".c", ".cpp", ".cc"))
    out = ROOT / "build/probe"
    (out / "errors").mkdir(parents=True, exist_ok=True)
    ok, first, by_area = 0, collections.defaultdict(list), collections.defaultdict(lambda: [0, 0])
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for path, rc, err in ex.map(lambda p: check(dec, p), files):
            rel = path.relative_to(dec / "src")
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
    json.dump({"ok": ok, "total": len(files), "areas": by_area, "first_error": first},
              (out / "summary.json").open("w"), indent=1)


if __name__ == "__main__":
    main()
