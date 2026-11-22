#!/usr/bin/env python3
"""Decode dumped Switch shaders and report which GPU instructions they use.

    D3_SHADER_DUMP=build/shaders build/macos/d3      # run for a while, then quit
    python3 tools/shader_stats.py build/shaders

Decoding is by tools/gm107.py. Each dumped .code file is a 0x30-byte wrapper, an
0x50-byte shader program header, then 64-bit instructions in groups of four where
the first word of each group is a scheduling word, not an instruction.
"""
import collections, hashlib, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gm107


def main():
    folder = Path(sys.argv[1])
    seen, ops, per_op_shaders = {}, collections.Counter(), collections.Counter()
    unknown = collections.Counter()
    for f in sorted(folder.glob("*.code")):
        b = f.read_bytes()
        seen.setdefault(hashlib.md5(b).hexdigest(), b)
    total = 0
    for b in seen.values():
        used = set()
        for pc, word, tokens in gm107.instructions(b):
            if word == 0:
                continue
            total += 1
            if tokens is None:
                unknown[word >> 48] += 1
                continue
            name = gm107.mnemonic(tokens)
            ops[name] += 1
            used.add(name)
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
