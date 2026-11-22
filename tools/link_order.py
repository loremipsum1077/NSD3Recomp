#!/usr/bin/env python3
"""Print the decomp's source files in the order the original program linked them.

    python3 tools/link_order.py <decomp root> < list-of-source-paths

The game's static initialisers depend on running in the original order (its memory
system must exist before the globals that allocate from it). Initialisers run in
link order, and the original link order is the order of each file's code in the
original program, which the decomp records per function
(data/2.6.2/function_files.csv). Files with no recorded function keep their
alphabetical order and go last. Called by cmake/DecompSources.cmake.
"""
import csv, sys
from pathlib import Path

root = Path(sys.argv[1])
first = {}
with (root / "data/2.6.2/function_files.csv").open() as f:
    for row in csv.DictReader(f):
        addr = int(row["Address"], 16)
        if row["File"] not in first or addr < first[row["File"]]:
            first[row["File"]] = addr
src = str(root / "src") + "/"
paths = [p for p in sys.stdin.read().replace(";", "\n").split("\n") if p.strip()]
paths.sort(key=lambda p: (first.get(p.replace(src, ""), 1 << 62), p))
sys.stdout.write(";".join(paths))
