#!/usr/bin/env python3
"""Decode dumped Switch shaders and report which GPU instructions they use.

    D3_SHADER_DUMP=build/shaders build/macos/d3      # run for a while, then quit
    python3 tools/shader_stats.py build/shaders

Instruction names come from the envytools table (extern/reference/envytools/gm107.c,
see extern/reference/README.md). Each dumped .code file is a 0x30-byte wrapper, an
0x50-byte shader program header, then 64-bit instructions in groups of four where
the first word of each group is a scheduling word, not an instruction.
"""
import collections, glob, hashlib, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def table():
    src = (ROOT / "extern/reference/envytools/gm107.c").read_text()
    body = src[src.index("static struct insn tabroot[]"):]
    body = body[:body.index("\n};")]
    out = []
    for m in re.finditer(r"\{\s*(0x[0-9a-f]+)ull,\s*(0x[0-9a-f]+)ull,\s*OP8B,(.*)", body):
        names = re.findall(r'N\(\s*"(\w+)"\)', m.group(3))
        out.append((int(m.group(1), 16), int(m.group(2), 16), " ".join(names) or "?"))
    return out


def main():
    folder = Path(sys.argv[1])
    tab = table()
    seen, ops, per_op_shaders = {}, collections.Counter(), collections.Counter()
    unknown = collections.Counter()
    for f in sorted(folder.glob("*.code")):
        b = f.read_bytes()
        seen.setdefault(hashlib.md5(b).hexdigest(), b)
    total = 0
    for b in seen.values():
        ins = b[0x30 + 0x50:]
        used = set()
        for i in range(0, len(ins) - 7, 8):
            if (i // 8) % 4 == 0:
                continue
            w, = struct.unpack_from("<Q", ins, i)
            if w == 0:
                continue
            total += 1
            for value, mask, name in tab:
                if w & mask == value:
                    ops[name] += 1
                    used.add(name)
                    break
            else:
                unknown[w >> 48] += 1
        for n in used:
            per_op_shaders[n] += 1
    print(f"{len(seen)} distinct shaders, {total} instructions, {len(ops)} different instructions, "
          f"{sum(unknown.values())} not recognised")
    print(f"{'instruction':16}{'uses':>8}{'shaders':>9}")
    for name, n in ops.most_common():
        print(f"{name:16}{n:8}{per_op_shaders[name]:9}")
    for k, n in unknown.most_common(10):
        print(f"unrecognised leading bits {k:04x}: {n}")


if __name__ == "__main__":
    main()
