#!/usr/bin/env python3
"""Print compiled Switch shaders as readable text.

    python3 tools/shader_disasm.py FILE.glslc            # one shader, full listing
    python3 tools/shader_disasm.py --summary FOLDER       # statistics over a folder

A .glslc file is a whole compiled-shader file as the game loads it: GPU code plus
the reflection table. Names from the reflection table are shown beside the
instructions that touch them: inputs (a[...]), constants (c[block][offset]) and
samplers.

Conventions used for naming that do NOT come from the instruction reference, and
are checked against the reflection table by --summary:
  - an input at location L is a[0x80 + 16*L], four bytes per component; position is
    a[0x70];
  - the constant block with binding B is c[B + 3];
  - a texture instruction's handle H refers to the sampler with binding (H - 8) / 2.
"""
import collections, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gm107

TYPES = {4: ("int", 1), 5: ("ivec2", 2), 6: ("ivec3", 3), 7: ("ivec4", 4), 20: ("uint", 1), 36: ("float", 1),
         37: ("vec2", 2), 38: ("vec3", 3), 39: ("vec4", 4)}


class Shader:
    def __init__(self, blob):
        self.blob = blob
        n, = struct.unpack_from("<I", blob, 0x4c)
        self.stage = None
        self.inputs, self.uniforms, self.blocks, self.samplers = [], [], [], []
        for i in range(n):
            h = 0x90 + 0x90 * i
            size, data_off, typ = struct.unpack_from("<III", blob, h)
            if typ == 0:
                self.stage, ctrl_off, code_off, code_size = struct.unpack_from("<IIII", blob, h + 0x2c)
                self.code = blob[data_off + code_off:data_off + code_off + code_size]
            elif typ == 3:
                self._reflection(h, data_off)

    def _name(self, base, rec):
        off, length = struct.unpack_from("<II", self.blob, rec)
        return self.blob[base + off:base + off + length].split(b"\0")[0].decode()

    def _reflection(self, h, data):
        b = self.blob
        (nblk, blk_off, nuni, uni_off, nin, in_off) = struct.unpack_from("<6I", b, h + 0x2c)
        pool = data + struct.unpack_from("<I", b, h + 0x68)[0]
        s = self.stage if self.stage is not None else 0
        for i in range(nblk):
            r = data + blk_off + 0x6c * i
            size, = struct.unpack_from("<I", b, r + 0x28)
            bindings = struct.unpack_from("<6i", b, r + 0x34)
            self.blocks.append({"name": self._name(pool, r), "size": size, "binding": bindings[s]})
        for i in range(nuni):
            r = data + uni_off + 0xa4 * i
            typ, blk, off, count = struct.unpack_from("<iiiI", b, r + 0x28)
            bindings = struct.unpack_from("<6i", b, r + 0x48)
            u = {"name": self._name(pool, r), "type": typ, "block": blk, "offset": off, "count": count, "binding": bindings[s]}
            (self.uniforms if b[r + 0x64] else self.samplers).append(u)
        for i in range(nin):
            r = data + in_off + 0x5c * i
            typ, count, loc = struct.unpack_from("<IIi", b, r + 0x28)
            self.inputs.append({"name": self._name(pool, r), "type": typ, "location": loc})

    # ---- naming ----

    def constant(self, block_binding, offset):
        for bi, blk in enumerate(self.blocks):
            if blk["binding"] + 3 != block_binding:
                continue
            best = None
            for u in self.uniforms:
                if u["block"] == bi and u["offset"] <= offset and (best is None or u["offset"] > best["offset"]):
                    best = u
            if best:
                d = offset - best["offset"]
                return f"{blk['name']}.{best['name']}" + (f"+{d}" if d else "")
            return blk["name"]
        return None

    def sampler(self, handle):
        for s in self.samplers:
            if s["binding"] * 2 + 8 == handle:
                return s["name"]
        return None

    def attribute(self, offset):
        if 0x70 <= offset < 0x80:
            return "position." + "xyzw"[(offset - 0x70) // 4]
        if 0x80 <= offset < 0x280:
            loc, comp = (offset - 0x80) // 16, "xyzw"[(offset - 0x80) % 16 // 4]
            for i in self.inputs:
                if i["location"] == loc:
                    return f"{i['name']}.{comp}"
            return f"attr{loc}.{comp}"
        return None


def text(sh, tokens):
    parts, note = [], []
    if gm107.mnemonic(tokens) in ("tex", "texs", "tlds", "tld4s", "txq"):
        handle = [t[1] for t in tokens if t[0] == "imm"]
        if handle and sh.sampler(handle[0]):
            note.append(sh.sampler(handle[0]))
    for t in tokens:
        if t[0] == "exec":
            neg = any(x == ("flag", "not") for x in t[1])
            reg = next((x[2] for x in t[1] if x[0] == "reg"), 7)
            if any(x == ("name", "never") for x in t[1]):
                parts.insert(0, "(never)")
            elif reg != 7 or neg:
                parts.insert(0, f"({'!' if neg else ''}p{reg})")
        elif t[0] in ("name", "flag"):
            parts.append(t[1])
        elif t[0] == "reg":
            parts.append("0" if t[1] == "r" and t[2] == 255 else "true" if t[1] == "p" and t[2] == 7 else f"{t[1]}{t[2]}")
        elif t[0] == "imm":
            parts.append(hex(t[1]) if abs(t[1]) > 9 else str(t[1]))
        elif t[0] == "float":
            parts.append(f"{t[1]:g}f")
        elif t[0] == "target":
            parts.append(f"L{t[1]:04x}")
        elif t[0] == "mem":
            _, name, idx, reg, off = t
            inner = (f"r{reg}+" if reg not in (None, 255) else "") + hex(off)
            parts.append(f"{name}[{idx}][{inner}]" if idx is not None else f"{name}[{inner}]")
            if reg in (None, 255):
                n = sh.constant(idx, off) if name == "c" else sh.attribute(off) if name == "a" else None
                if n:
                    note.append(n)
    return " ".join(parts), ", ".join(note)


def listing(path):
    sh = Shader(Path(path).read_bytes())
    print(f"; {path}: {'vertex' if sh.stage == 0 else 'fragment'} shader, {len(sh.code)} bytes of code")
    for i in sh.inputs:
        print(f";   input    location {i['location']:2}  {TYPES.get(i['type'], ('type%d' % i['type'],))[0]:6} {i['name']}")
    for b in sh.blocks:
        print(f";   block    binding {b['binding']:2}   {b['size']} bytes  {b['name']}")
    for s in sh.samplers:
        print(f";   sampler  binding {s['binding']:2}   {s['name']}")
    targets = {t[1] for _, _, tok in gm107.instructions(sh.code) if tok for t in tok if t[0] == "target"}
    for pc, word, tok in gm107.instructions(sh.code):
        if word == 0:
            continue
        if pc in targets:
            print(f"L{pc:04x}:")
        if tok is None:
            print(f"    {pc:04x}  .word {word:#018x}")
            continue
        line, note = text(sh, tok)
        print(f"    {pc:04x}  {line}" + (f"    ; {note}" if note else ""))


def summary(folder):
    files = sorted(Path(folder).glob("*.glslc"))
    stats = collections.Counter()
    for f in files:
        sh = Shader(f.read_bytes())
        ins = [(pc, w, t) for pc, w, t in gm107.instructions(sh.code) if w]
        stats["shaders"] += 1
        stats["vertex" if sh.stage == 0 else "fragment"] += 1
        back = cond = fwd = 0
        attr_read, named = set(), 0
        for pc, w, tok in ins:
            if tok is None:
                stats["words not decoded"] += 1
                continue
            op = gm107.mnemonic(tok)
            if op == "bra":
                tgt = next((t[1] for t in tok if t[0] == "target"), None)
                is_cond = any(t[0] == "exec" for t in tok if t[0] == "exec" and
                              (any(x[0] == "reg" and x[2] != 7 for x in t[1]) or ("flag", "not") in t[1]))
                has_cc = any(t[0] == "name" and t[1] not in ("bra", "t") for t in tok)
                if tgt is None:
                    stats["branches: indirect"] += 1
                elif tgt == pc:
                    stats["branches: the stop-here loop after the end of each shader"] += 1
                elif tgt <= pc:
                    back += 1
                else:
                    fwd += 1
                cond += 1 if (is_cond or has_cc) else 0
            if op == "ld" and sh.stage == 0:
                for t in tok:
                    if t[0] == "mem" and t[1] == "a" and t[3] in (None, 255) and 0x80 <= t[4] < 0x280:
                        attr_read.add((t[4] - 0x80) // 16)
            if op in ("tex", "texs", "tlds", "tld4s"):
                handle = [t[1] for t in tok if t[0] == "imm"]
                stats["texture reads"] += 1
                stats["texture reads named"] += 1 if handle and sh.sampler(handle[0]) else 0
            for t in tok:
                if t[0] == "mem" and t[1] == "c" and t[3] is not None and t[3] != 255:
                    stats["constant reads with a computed offset (arrays)"] += 1
                if t[0] == "mem" and t[1] == "c" and t[3] in (None, 255):
                    stats["constant reads"] += 1
                    stats["constant reads named"] += 1 if sh.constant(t[2], t[4]) else 0
                    if not sh.constant(t[2], t[4]):
                        stats[f"  unnamed: block c[{t[2]}]"] += 1
        stats["branches: forward"] += fwd
        stats["branches: backward (loops)"] += back
        stats["shaders with a loop"] += 1 if back else 0
        stats["shaders with no branch at all"] += 1 if fwd + back == 0 else 0
        stats["conditional branches"] += cond
        if sh.stage == 0:
            declared = {i["location"] for i in sh.inputs}
            stats["vertex shaders whose attribute reads match the declared inputs"] += 1 if attr_read <= declared else 0
            stats["vertex shaders reading an undeclared attribute"] += 0 if attr_read <= declared else 1
    for k, v in stats.items():
        print(f"{v:8}  {k}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--summary":
        summary(sys.argv[2])
    elif len(sys.argv) == 2:
        listing(sys.argv[1])
    else:
        sys.exit(__doc__)
