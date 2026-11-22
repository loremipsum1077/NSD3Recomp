#!/usr/bin/env python3
"""Add a stand-in for every Switch SDK, graphics-driver and FMOD function that is
still undefined at link time.

Reads the last link attempt (run tools/link_probe.py first) and appends D3_STUB
lines to src/nn/<area>/unimplemented.c, src/gfx/unimplemented.c and
src/audio/unimplemented.c. Existing lines are kept; a symbol that is already
defined, by a stub or by a real implementation, is not undefined and so is never
added again. Variables and everything outside those three groups are left alone.
"""
import collections, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C_AREAS = [("nnos", "os"), ("nnsocket", "socket"), ("nnssl", "ssl"), ("nnResult", "result"), ("nndetail", "diag")]


def target(mangled, pretty):
    """Source file a stub belongs in, or None if this tool does not handle the symbol."""
    m = re.match(r"nn::(\w+)::.*\(", pretty)
    if m:
        return f"src/nn/{m.group(1)}/unimplemented.c"
    if re.match(r"nv::.*\(", pretty) or mangled.startswith("nvn"):
        return "src/gfx/unimplemented.c"
    if mangled.startswith("FMOD_"):
        return "src/audio/unimplemented.c"
    for prefix, area in C_AREAS:
        if re.match(prefix + r"[A-Z]", mangled):
            return f"src/nn/{area}/unimplemented.c"
    # Everything else that is a function: called by our build of the decomp but defined
    # nowhere. (Missing data is handled by tools/extract_data.py, which runs first.)
    if mangled != "main" and not pretty.startswith(("vtable for", "typeinfo")) and ("(" in pretty or "::" not in pretty):
        return "src/replace/unimplemented.c"
    return None


def main():
    tsv = ROOT / "build/probe/undefined_symbols.tsv"
    if not tsv.is_file():
        sys.exit("run tools/link_probe.py first")
    files = collections.defaultdict(list)
    for line in tsv.read_text().splitlines():
        s, p = line.split("\t", 1)
        t = target(s, p)
        if t:
            files[t].append((p, s))
    for t, entries in sorted(files.items()):
        path = ROOT / t
        path.parent.mkdir(parents=True, exist_ok=True)
        text = path.read_text() if path.is_file() else (
            "// Functions not implemented yet. Each stops the program and prints its name.\n"
            "// Added by tools/gen_stubs.py; delete a line when the real function is written.\n"
            '#include "host/unimplemented.h"\n\n')
        new = [f'D3_STUB({s}, "{p}")\n' for p, s in sorted(entries) if f"D3_STUB({s}," not in text]
        path.write_text(text + "".join(new))
        print(f"{len(new):4} added  {t}")
    print(sum(len(v) for v in files.values()), "symbols handled")


if __name__ == "__main__":
    main()
