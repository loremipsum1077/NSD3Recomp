#!/usr/bin/env python3
"""Remove stand-in lines once the real functions are written.

    python3 tools/unstub.py 'nn::fs::OpenFile(' 'nn::fs::CloseFile('

Each argument is text to look for in the readable name of a D3_STUB line under
src/. Every matching line is deleted; the count is printed per argument.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
files = list((ROOT / "src").rglob("unimplemented.c"))
for needle in sys.argv[1:]:
    hits = 0
    for f in files:
        lines = f.read_text().splitlines(keepends=True)
        keep = [l for l in lines if not (l.startswith("D3_STUB(") and needle in l.split(",", 1)[1])]
        if len(keep) != len(lines):
            hits += len(lines) - len(keep)
            f.write_text("".join(keep))
    print(f"{hits:3} removed  {needle}")
