#!/usr/bin/env python3
"""Write C definitions for data that exists in the original program but that the
decomp does not define yet.

Inputs, all from the player's own dump (nothing here is stored in the repository):
    game/exefs/main                 the program (NSO)
    game/romfs/DiabloIIINX64r.nss   its debug file: symbol names, addresses, sizes
    config/extract_symbols.txt      which symbols to extract (names only). A symbol the
                                    decomp has since defined is skipped automatically.

Output: one C file. Each symbol becomes a block of bytes; every pointer inside it
(found through the program's relocation table) becomes a reference to a string
literal or to another named symbol. Variables that start as zero become
zero-filled definitions. Anything that cannot be expressed is listed on stderr and
left undefined, so the link probe still reports it.

    python3 tools/extract_data.py --out build/macos/generated/extracted_data.c
"""
import argparse, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lz4_block(src, size):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    if len(out) != size:
        sys.exit(f"NSO segment decompressed to {len(out)} bytes, expected {size}")
    return bytes(out)


class Program:
    """The NSO's three segments laid out at their memory addresses."""

    def __init__(self, path):
        d = path.read_bytes()
        if d[:4] != b"NSO0":
            sys.exit(f"{path} is not an NSO file")
        flags = struct.unpack_from("<I", d, 0x0C)[0]
        comp = struct.unpack_from("<3I", d, 0x60)
        self.seg = []
        for n, hdr in enumerate((0x10, 0x20, 0x30)):
            foff, moff, size = struct.unpack_from("<3I", d, hdr)
            raw = d[foff:foff + comp[n]]
            self.seg.append((moff, lz4_block(raw, size) if flags & (1 << n) else raw[:size]))
        self.bss_size = struct.unpack_from("<I", d, 0x3C)[0]
        self.text, self.ro, self.data = self.seg
        self.bss_start = None
        self._relocs()

    def read(self, addr, size):
        for moff, b in self.seg:
            if moff <= addr and addr + size <= moff + len(b):
                return b[addr - moff:addr - moff + size]
        return None

    def kind(self, addr):
        for name, (moff, b) in zip(("text", "rodata", "data"), self.seg):
            if moff <= addr < moff + len(b):
                return name
        return "bss"

    def _relocs(self):
        mod = struct.unpack_from("<I", self.text[1], 4)[0]
        magic, dyn = struct.unpack_from("<4si", self.read(mod, 8))
        if magic != b"MOD0":
            sys.exit("MOD0 header not found")
        a, tags = mod + dyn, {}
        while True:
            tag, val = struct.unpack_from("<qQ", self.read(a, 16)); a += 16
            if tag == 0:
                break
            tags[tag] = val
        self.rel = {}                                   # address -> (type, symbol index, addend)
        rela, relasz = tags.get(7), tags.get(8, 0)
        for i in range(relasz // 24):
            off, info, addend = struct.unpack_from("<QQq", self.read(rela + i * 24, 24))
            self.rel[off] = (info & 0xffffffff, info >> 32, addend)
        self.dynsym, self.dynstr = tags.get(6), tags.get(5)

    def dynsym_name(self, index):
        name_off, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", self.read(self.dynsym + index * 24, 24))
        s = self.read(self.dynstr + name_off, 1)
        a, out = self.dynstr + name_off, b""
        while (c := self.read(a, 1)) and c != b"\0":
            out += c; a += 1
        return out.decode(), value

    def cstring(self, addr, limit=4096):
        out = bytearray()
        while len(out) < limit:
            c = self.read(addr + len(out), 1)
            if not c:
                return None
            if c == b"\0":
                return bytes(out)
            out += c
        return None


def elf_symbols(path):
    """name -> (address, size, is_function) for every symbol in the debug file, and address -> name."""
    with path.open("rb") as f:
        hdr = f.read(64)
        shoff, = struct.unpack_from("<Q", hdr, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", hdr, 0x3A)
        f.seek(shoff); sh = f.read(shentsize * shnum)
        secs = [struct.unpack_from("<IIQQQQIIQQ", sh, i * shentsize) for i in range(shnum)]
        symtab = next(s for s in secs if s[1] == 2)
        strtab = secs[symtab[6]]
        f.seek(symtab[4]); sym = f.read(symtab[5])
        f.seek(strtab[4]); strs = f.read(strtab[5])
    by_name, by_addr = {}, {}
    for i in range(0, len(sym), 24):
        name_off, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", sym, i)
        typ, bind = info & 15, info >> 4
        if shndx == 0 or typ not in (0, 1, 2):
            continue
        name = strs[name_off:strs.index(b"\0", name_off)].decode()
        if not name or name.startswith("$"):
            continue
        by_name.setdefault(name, (value, size, typ == 2, bind != 0))
        if bind != 0 or value not in by_addr:            # prefer a linkable (non-local) name
            by_addr[value] = (name, bind != 0)
    return by_name, by_addr


def csv_symbols(data_dir, size_hints=None, base=0x7100000000):
    """The same tables as elf_symbols, for a game version without a debug file: names and
    addresses from a decomp checkout's data/<version>/functions.csv and data_symbols.csv.
    Data sizes come from another version's debug file when it has the same name, else from
    the distance to the next known symbol."""
    import csv
    hints = elf_symbols(size_hints)[0] if size_hints else {}
    rows = []                                           # (address, size or None, name, is_function)
    for r in csv.DictReader(open(data_dir / "functions.csv")):
        rows.append((int(r["Address"], 16) - base, int(r["Size"]), r["Name"], True))
    for r in csv.DictReader(open(data_dir / "data_symbols.csv")):
        rows.append((int(r["Address"], 16) - base, None, r["Name"], False))
    rows.sort(key=lambda r: (r[0], r[3], r[2]))
    by_name, by_addr = {}, {}
    for i, (addr, size, name, is_func) in enumerate(rows):
        if size is None:
            nxt = next((a for a, *_ in rows[i + 1:] if a > addr), addr + 8)
            gap = min(nxt - addr, 0x40000)
            hint = hints.get(name)
            size = min(hint[1], gap) if hint and hint[1] and not hint[2] else gap
        local = name.startswith(("_ZL", "_ZZ", "_ZGVZ", "_ZGVL")) or ".L" in name
        by_name.setdefault(name, (addr, size, is_func, not local))
        if not local or addr not in by_addr:
            by_addr[addr] = (name, not local)
    return by_name, by_addr


def c_string(b):
    out = []
    for c in b:
        if c in (0x22, 0x5c):
            out.append("\\" + chr(c))
        elif 32 <= c < 127:
            out.append(chr(c))
        else:
            out.append("\\%03o" % c)
    return '"' + "".join(out) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=str(ROOT / "game"))
    ap.add_argument("--symbols", default=str(ROOT / "config/extract_symbols.txt"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-defined-in", metavar="FILE",
                    help="file listing the decomp's compiled objects: symbols they define are skipped")
    ap.add_argument("--symbols-csv", metavar="DIR",
                    help="for a version without a debug file: a decomp's data/<version> folder")
    ap.add_argument("--size-hints", metavar="NSS", help="another version's debug file, for data sizes")
    ap.add_argument("--add-undefined", action="store_true",
                    help="first add still-undefined data symbols from the last link probe to the list")
    a = ap.parse_args()
    game = Path(a.game)
    nso, nss = game / "exefs/main", game / "romfs/DiabloIIINX64r.nss"
    for p in (nso,) if a.symbols_csv else (nso, nss):
        if not p.is_file():
            sys.exit(f"{p} not found: the data extraction needs your own game dump (see README.md)")
    prog = Program(nso)
    if a.symbols_csv:
        by_name, by_addr = csv_symbols(Path(a.symbols_csv), Path(a.size_hints) if a.size_hints else None)
    else:
        by_name, by_addr = elf_symbols(nss)
    if a.add_undefined:
        # Append every still-undefined symbol that is data in the original. Run after
        # tools/link_probe.py; repeat until nothing is added, because extracted data
        # can point at further data the decomp does not define.
        listed = Path(a.symbols).read_text()
        have = set(listed.split())
        new = []
        for line in (ROOT / "build/probe/undefined_symbols.tsv").read_text().splitlines():
            name = line.split("\t")[0]
            if name in by_name and not by_name[name][2] and by_name[name][1] and name not in have:
                new.append(name)
        Path(a.symbols).write_text(listed + "".join(n + "\n" for n in new))
        print(f"{len(new)} symbols added to {a.symbols}", file=sys.stderr)
    wanted = [l.strip() for l in Path(a.symbols).read_text().splitlines() if l.strip() and not l.startswith("#")]
    if a.skip_defined_in:
        # The decomp gains definitions over time. Whatever it defines today wins; the
        # list keeps the name in case that changes back.
        objs = [l for l in Path(a.skip_defined_in).read_text().splitlines() if l and Path(l).is_file()]
        defined = set()
        for i in range(0, len(objs), 200):
            nm = subprocess.run(["nm", "-g", "-U", "-j"] + objs[i:i + 200], capture_output=True, text=True).stdout
            defined.update(l[1:] if l.startswith("_") else l for l in nm.splitlines())
        skipped = [w for w in wanted if w in defined]
        wanted = [w for w in wanted if w not in defined]
        print(f"{len(skipped)} symbols skipped: the decomp defines them now", file=sys.stderr)

    L = ["// Generated by tools/extract_data.py from the player's own copy of the game.",
         "// Game content: never commit this file.",
         "#include <stdint.h>",
         "#ifdef __APPLE__", '#define SYM(name) __asm__("_" name)', "#else", '#define SYM(name) __asm__(name)', "#endif",
         "#pragma pack(push, 1)", ""]
    externs, problems, locals_, counts = {}, [], {}, {"content": 0, "zero": 0}
    body = []

    def ref(target, owner):
        """C expression for a pointer to `target`, or None."""
        hit = by_addr.get(target)
        if hit and hit[1]:                                # a name the linker can see
            externs.setdefault(hit[0], len(externs))
            return f"&x{externs[hit[0]]}"
        if hit and by_name[hit[0]][1] and not by_name[hit[0]][2]:
            # File-local data in the original: define a private copy and point at it.
            if target not in locals_:
                locals_[target] = f"l{len(locals_)}"
                if not define(target, by_name[hit[0]][1], locals_[target], f"/* {hit[0]} */", hit[0]):
                    locals_[target] = None
            return f"&{locals_[target]}" if locals_[target] else None
        if prog.kind(target) == "rodata" and not hit:
            s = prog.cstring(target)
            if s is not None:
                return c_string(s)
        problems.append(f"{owner}: pointer to {target:#x} ({prog.kind(target)}"
                        + (f", local symbol {hit[0]}" if hit else ", no symbol") + ")")
        return None

    def define(addr, size, cname, attr, owner):
        """Append the definition of one object. False if it cannot be expressed."""
        align = min(16, addr & -addr) or 16
        attr += f" __attribute__((aligned({align})))"
        raw = prog.read(addr, size)
        if raw is None:                                   # starts as zero
            body.append(f"unsigned char {cname}[{size}] {attr};")
            counts["zero"] += 1
            return True
        fields, values, pos, k = [], [], 0, 0
        ptrs = sorted(o for o in range(addr, addr + size) if o in prog.rel)
        for p in ptrs + [addr + size]:
            if p - addr > pos:
                chunk = raw[pos:p - addr]
                fields.append(f"unsigned char b{k}[{len(chunk)}];")
                values.append("{" + ",".join(str(c) for c in chunk) + "}")
                k += 1
            if p == addr + size:
                break
            typ, symidx, addend = prog.rel[p]
            if typ == 1027:                               # R_AARCH64_RELATIVE
                e = ref(addend, owner)
            else:                                         # against a dynamic symbol
                dn, dv = prog.dynsym_name(symidx)
                e = ref(dv + addend, owner) if dv else None
                if not dv:
                    problems.append(f"{owner}: pointer to imported symbol {dn}")
            if e is None:
                return False
            fields.append(f"const void * p{k};")
            values.append(e)
            k += 1
            pos = p - addr + 8
        body.append(f"struct t_{cname} {{ {' '.join(fields)} }};")
        body.append(f"struct t_{cname} {cname} {attr} = {{ {', '.join(values)} }};")
        counts["content"] += 1
        return True

    failed = 0
    for n, name in enumerate(wanted):
        if name not in by_name:
            problems.append(f"{name}: not in the debug file's symbol table")
            failed += 1
            continue
        addr, size, is_func, _ = by_name[name]
        if is_func or size == 0:
            problems.append(f"{name}: {'a function' if is_func else 'size 0'}, not data")
            failed += 1
            continue
        if not define(addr, size, f"d{n}", f'SYM("{name}")', name):
            failed += 1
    for sym, i in externs.items():
        L.append(f'extern char x{i} SYM("{sym}");')
    L += [""] + body + ["", "#pragma pack(pop)"]
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L) + "\n")
    print(f"{len(wanted) - failed} of {len(wanted)} symbols defined ({counts['zero']} zero-filled, "
          f"{len(locals_)} private copies of file-local data); {failed} not extracted -> {out}", file=sys.stderr)
    for p in problems:
        print("  " + p, file=sys.stderr)


if __name__ == "__main__":
    main()
