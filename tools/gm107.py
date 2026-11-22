"""Decoder for Switch GPU (NVIDIA GM107 family) shader instructions.

Table-driven from src/shader/gm107.json (see tools/gen_gm107_table.py). Used by
tools/shader_disasm.py and tools/shader_stats.py.

Code layout in a compiled shader: a 0x30-byte wrapper, a 0x50-byte program header,
then 64-bit words in groups of four. The first word of each group is a scheduling
word for the three instructions after it and is skipped. Branch targets are byte
offsets from the first word after the program header.
"""
import json, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLES = json.loads((ROOT / "src/shader/gm107.json").read_text())["tables"]
CODE_START = 0x30 + 0x50


def _imm(word, a):
    value, width = 0, 0
    for pos, w in a["parts"]:
        value |= ((word >> pos) & ((1 << w) - 1)) << width
        width += w
    if a.get("signed") and value >> (width - 1):
        value -= 1 << width
    return value << a.get("shl", 0)


def _walk(word, pc, atoms, out):
    for a in atoms:
        k = a["k"]
        if k == "name":
            out.append(("name", a["n"]))
        elif k == "flag":
            if (word >> a["bit"]) & 1 == a["when"]:
                out.append(("flag", a["n"]))
        elif k == "table":
            for row in TABLES[a["t"]]:
                if word & row["m"] == row["v"]:
                    sub = []
                    _walk(word, pc, row["a"], sub)
                    if a["t"] == "pred":                  # the predicate an instruction runs under
                        out.append(("exec", sub))
                    else:
                        out.extend(sub)
                    break
        elif k == "reg":
            n = (word >> a["pos"]) & ((1 << a["w"]) - 1)
            out.append(("reg", a["p"], n ^ a.get("xor", 0)))
        elif k == "imm":
            v = _imm(word, a)
            if a.get("float") == 32:
                out.append(("float", struct.unpack("<f", struct.pack("<I", v & 0xffffffff))[0]))
            else:
                out.append(("imm", v))
        elif k == "target":
            out.append(("target", pc + _imm(word, a) + a.get("addend", 0)))
        elif k == "mem":
            idx = (word >> a["idx"][0]) & ((1 << a["idx"][1]) - 1) if "idx" in a else None
            reg = (word >> a["reg"]) & 0xff if "reg" in a else None
            off = _imm(word, a["imm"]) if "imm" in a else 0
            out.append(("mem", a["n"], idx, reg, off))


def decode(word, pc):
    """Tokens for one instruction word, or None if it matches nothing."""
    for row in TABLES["root"]:
        if word & row["m"] == row["v"]:
            out = []
            _walk(word, pc, row["a"], out)
            return out
    return None


def instructions(code):
    """(pc, word, tokens) for every instruction in a dumped .code blob."""
    body = code[CODE_START:]
    for i in range(0, len(body) - 7, 8):
        if (i // 8) % 4 == 0:
            continue
        word, = struct.unpack_from("<Q", body, i)
        yield i, word, decode(word, i)


def mnemonic(tokens):
    return next((t[1] for t in tokens if t[0] == "name"), "?")
