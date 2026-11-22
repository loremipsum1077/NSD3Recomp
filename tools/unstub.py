#!/usr/bin/env python3
"""Remove stand-in lines once the real functions are written.

    python3 tools/unstub.py 'nn::fs::OpenFile(' 'nn::fs::CloseFile('

Each argument is the start of the readable name of a D3_STUB line under src/
(so 'nn::fs::' removes that whole area, and never a function that merely takes an
nn::fs type as a parameter). Every matching line is deleted; the count is printed per argument.
"""
import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
files = list((ROOT / "src").rglob("unimplemented.c"))

if sys.argv[1:] == ["--defined-by-decomp"]:
    # The decomp gains functions over time. Drop every stand-in whose symbol its
    # compiled objects now define (a link would report these as defined twice).
    objs = [str(p) for p in (ROOT / "build/macos/CMakeFiles/d3_decomp.dir").rglob("*.o")]
    defined = set()
    for i in range(0, len(objs), 200):
        nm = subprocess.run(["nm", "-g", "-U", "-j"] + objs[i:i + 200], capture_output=True, text=True).stdout
        defined.update(l[1:] if l.startswith("_") else l for l in nm.splitlines())
    for f in files:
        lines = f.read_text().splitlines(keepends=True)
        keep = [l for l in lines if not ((m := re.match(r"D3_STUB\((\w+),", l)) and m.group(1) in defined)]
        if len(keep) != len(lines):
            print(f"{len(lines) - len(keep):3} removed  {f.relative_to(ROOT)}")
            f.write_text("".join(keep))
    sys.exit(0)
for needle in sys.argv[1:]:
    hits = 0
    for f in files:
        lines = f.read_text().splitlines(keepends=True)
        keep = [l for l in lines if not (l.startswith("D3_STUB(") and l.split(",", 1)[1].strip().lstrip('"').lstrip("_").startswith(needle))]
        if len(keep) != len(lines):
            hits += len(lines) - len(keep)
            f.write_text("".join(keep))
    print(f"{hits:3} removed  {needle}")
