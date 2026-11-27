#!/usr/bin/env python3
"""Translate compiled Switch shaders to Metal shading language.

    python3 tools/shader_translate.py FILE.glslc              # print one translation
    python3 tools/shader_translate.py --all FOLDER OUTFOLDER   # translate every .glslc
    python3 tools/shader_translate.py --cache FOLDER           # fill the run-time cache

The run-time cache: the Metal driver looks for <hash>.metal in the cache folder for
each shader the game creates, and writes <hash>.code when it is missing. --cache
translates every .code that has no .metal yet (and writes <hash>.failed with the
reason when it cannot; those are tried again each time, in case the translator has
learned what they need).

How it works. The GPU code is a register machine: 32-bit registers r0..r254 that
hold either a float or an integer, and one-bit predicates p0..p6. The translation
keeps exactly that shape: one Metal `uint` variable per register, holding the bit
pattern, and one statement per instruction. Nothing is reinterpreted or simplified,
so a translation is as right as the per-instruction rules below; the Metal compiler
does the optimising.

Control flow. Almost every branch in the game's shaders goes forward. Those shaders
become straight-line code in numbered blocks, where a taken branch sets `jump` to
the target block and every block in between is skipped. A shader with a backward
branch becomes a loop around a `switch` on the block number instead.

What a translated shader expects from the renderer:
  - vertex inputs as attribute(L) for each input location L;
  - constant block c[N] as buffer(N). The game's blocks start at c[3]. c[1] is the
    shader's own pool of literal constants, stored after its code (docs/SHADERS.md);
  - texture and sampler with binding B as texture(B) and sampler(B);
  - values passed from vertex to fragment shader as user(locnL).

Instructions that are not handled are reported, never guessed: the function
returns the list, and --all counts the shaders affected.
"""
import struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gm107
from shader_disasm import Shader

OPERAND_MODS = {"neg", "abs", "inv", "not", "h1"}
FLOAT_CMP = {"f": "false", "t": "true", "lt": "{a} < {b}", "eq": "{a} == {b}", "le": "{a} <= {b}", "gt": "{a} > {b}",
             "ne": "({a} < {b} || {a} > {b})", "ge": "{a} >= {b}", "num": "!(isnan({a}) || isnan({b}))",
             "nan": "(isnan({a}) || isnan({b}))", "ltu": "!({a} >= {b})", "equ": "!({a} < {b} || {a} > {b})",
             "leu": "!({a} > {b})", "gtu": "!({a} <= {b})", "neu": "{a} != {b}", "geu": "!({a} < {b})"}
INT_CMP = {"f": "false", "t": "true", "lt": "{a} < {b}", "eq": "{a} == {b}", "le": "{a} <= {b}", "gt": "{a} > {b}",
           "ne": "{a} != {b}", "ge": "{a} >= {b}"}
SCALE = {"m2": " * 2.0f", "m4": " * 4.0f", "m8": " * 8.0f", "d2": " * 0.5f", "d4": " * 0.25f", "d8": " * 0.125f"}
MUFU = {"cos": "cos({a})", "sin": "sin({a})", "ex2": "exp2({a})", "lg2": "log2({a})", "rcp": "(1.0f / {a})",
        "rsq": "rsqrt({a})", "sqrt": "sqrt({a})"}
ROUND = {"floor": "floor", "ceil": "ceil", "trunc": "trunc", "round": "rint"}
IGNORED = {"nop", "depbar"}


def flit(x):
    if x != x:
        return "as_type<float>(0x7fc00000u)"
    if x in (float("inf"), float("-inf")):
        return ("-" if x < 0 else "") + "INFINITY"
    return f"as_type<float>(0x{struct.unpack('<I', struct.pack('<f', x))[0]:08x}u)"


class CodeOnly:
    """A shader known only by its GPU code, as the driver receives it. The program
    header says which kind it is: bits 10-13 of its first word are 1 for a vertex
    shader and 5 for a fragment shader."""

    def __init__(self, code):
        self.code = code
        kind = (struct.unpack_from("<I", code, 0x30)[0] >> 10) & 15
        if kind not in (1, 5):
            raise ValueError(f"shader kind {kind} is neither vertex nor fragment")
        self.stage = 0 if kind == 1 else 1


INTEGER_USERS = {"i2f", "i2i", "iadd", "iadd3", "shl", "shr", "xmad", "lop", "lop32i", "lop3", "isetp", "imnmx", "bfi"}


class Translator:
    def __init__(self, sh):
        self.sh = sh
        self.fragment = sh.stage == 1
        self.integer_inputs = set()
        self.regs, self.preds = set(), set()
        self.cbufs, self.textures = set(), {}
        self.inputs, self.outputs = set(), set()
        self.reads_position = self.writes_position = False
        self.vertex_id = self.instance_id = False
        self.unhandled, self.approximated = [], []

    # ---- operands ----

    def split(self, tokens):
        """-> (flags, operands, exec predicate). An operand is (token, modifiers)."""
        flags, ops, pending, ex = [], [], set(), None
        for t in tokens:
            if t[0] == "exec":
                neg = ("flag", "not") in t[1]
                reg = next((x[2] for x in t[1] if x[0] == "reg"), 7)
                ex = "false" if ("name", "never") in t[1] else None if reg == 7 and not neg else self.pred(reg, neg)
            elif t[0] in ("name", "flag"):
                if t[1] in OPERAND_MODS:
                    pending.add(t[1])
                else:
                    flags.append(t[1])
            else:
                ops.append((t, pending))
                pending = set()
        return flags, ops, ex

    def pred(self, n, neg=False):
        if n == 7:
            return "false" if neg else "true"
        self.preds.add(n)
        return f"!p{n}" if neg else f"p{n}"

    def reg(self, n):
        self.regs.add(n)
        return f"r{n}"

    def cbuf(self, t):
        _, _, idx, reg, off = t
        self.cbufs.add(idx)
        if reg in (None, 255):
            return f"c{idx}[{off // 4}]"
        return f"c{idx}[({self.reg(reg)} + {off}u) >> 2]"

    def u(self, op):
        t, mods = op
        if t[0] == "reg":
            e = "0u" if t[2] == 255 else self.reg(t[2])
        elif t[0] == "imm":
            e = f"0x{t[1] & 0xffffffff:08x}u"
        elif t[0] == "float":
            e = f"0x{struct.unpack('<I', struct.pack('<f', t[1]))[0]:08x}u"
        elif t[0] == "mem" and t[1] == "c":
            e = self.cbuf(t)
        else:
            raise KeyError(f"operand {t[0]}")
        if "inv" in mods:
            e = f"~{e}"
        if "abs" in mods:
            e = f"uint(abs(int({e})))"
        if "neg" in mods:
            e = f"(0u - {e})"
        return e

    def f(self, op):
        t, mods = op
        if t[0] == "reg":
            e = "0.0f" if t[2] == 255 else f"as_type<float>({self.reg(t[2])})"
        elif t[0] == "float":
            e = flit(t[1])
        elif t[0] == "imm":
            e = f"as_type<float>(0x{t[1] & 0xffffffff:08x}u)"
        elif t[0] == "mem" and t[1] == "c":
            e = f"as_type<float>({self.cbuf(t)})"
        else:
            raise KeyError(f"operand {t[0]}")
        if "abs" in mods:
            e = f"abs({e})"
        if "neg" in mods:
            e = f"(-{e})"
        return e

    def p(self, op):
        t, mods = op
        return self.pred(t[2], "not" in mods)

    def set_u(self, dst, expr):
        n = dst[0][2]
        return "" if n == 255 else f"{self.reg(n)} = {expr};"

    def set_f(self, dst, expr, flags=()):
        if "sat" in flags:
            expr = f"saturate({expr})"
        return self.set_u(dst, f"as_type<uint>({expr})")

    def set_p(self, dst, expr):
        n = dst[0][2]
        if n == 7:
            return ""
        self.preds.add(n)
        return f"p{n} = {expr};"

    # ---- inputs and outputs ----

    def attribute_in(self, off):
        """Float expression for reading attribute memory at a fixed offset."""
        comp = "xyzw"[off % 16 // 4]
        if off == 0x2fc and not self.fragment:      # the vertex's number, as an integer
            self.vertex_id = True
            return "as_type<float>(vid)"
        if off == 0x2f8 and not self.fragment:
            self.instance_id = True
            return "as_type<float>(iid)"
        if 0x70 <= off < 0x80:
            self.reads_position = True
            return f"in.position.{comp}"
        if 0x80 <= off < 0x280:
            loc = (off - 0x80) // 16
            self.inputs.add(loc)
            if loc in self.integer_inputs:
                return f"as_type<float>(in.a{loc}.{comp})"     # keep the integer's bits
            return f"in.a{loc}.{comp}"
        raise KeyError(f"attribute offset {off:#x}")

    def find_integer_inputs(self, ins):
        """Vertex inputs the shader treats as integers (bone indices, packed normals).
        The code does not declare input types, so they are recognised by use: a
        register loaded from an input and then consumed by an integer instruction."""
        origin = {}
        for pc, w, t in ins:
            if not t:
                continue
            op = gm107.mnemonic(t)
            regs = [x for x in t if x[0] == "reg" and x[1] == "r"]
            if op == "ld":
                mem = next((x for x in t if x[0] == "mem" and x[1] == "a" and x[3] in (None, 255)), None)
                if mem and regs and 0x80 <= mem[4] < 0x280:
                    origin[regs[0][2]] = (mem[4] - 0x80) // 16
                elif regs:
                    origin.pop(regs[0][2], None)
                continue
            if op in INTEGER_USERS:
                for r in regs[1:]:
                    if r[2] in origin:
                        self.integer_inputs.add(origin[r[2]])
            if op == "mov" and len(regs) == 2 and regs[1][2] in origin:
                origin[regs[0][2]] = origin[regs[1][2]]
            elif regs and op not in ("st", "bra", "kil", "exit"):
                origin.pop(regs[0][2], None)

    def attribute_out(self, off):
        comp = "xyzw"[off % 16 // 4]
        if 0x70 <= off < 0x80:
            self.writes_position = True
            return f"out.position.{comp}"
        if 0x80 <= off < 0x280:
            loc = (off - 0x80) // 16
            self.outputs.add(loc)
            return f"out.a{loc}.{comp}"
        raise KeyError(f"attribute offset {off:#x}")

    def texture(self, handle, kind):
        binding = (handle - 8) // 2
        if self.textures.setdefault(binding, kind) != kind:
            raise KeyError("one texture used as two kinds")
        return binding

    # ---- one instruction ----

    def statement(self, tokens):
        op = gm107.mnemonic(tokens)
        flags, ops, ex = self.split(tokens)
        fl = [x for x in flags if x != op]
        s = self.emit(op, fl, ops)
        if s is None:
            return None, None
        return s, ex

    def emit(self, op, fl, o):
        if op in IGNORED:
            return ""
        if op == "fadd" or op == "fadd32i":
            return self.set_f(o[0], f"{self.f(o[1])} + {self.f(o[2])}", fl)
        if op == "fmul" or op == "fmul32i":
            scale = next((SCALE[x] for x in fl if x in SCALE), "")
            return self.set_f(o[0], f"({self.f(o[1])} * {self.f(o[2])}){scale}", fl)
        if op == "ffma":
            return self.set_f(o[0], f"fma({self.f(o[1])}, {self.f(o[2])}, {self.f(o[3])})", fl)
        if op == "fmnmx":
            a, b = self.f(o[1]), self.f(o[2])
            return self.set_f(o[0], f"{self.p(o[3])} ? min({a}, {b}) : max({a}, {b})")
        if op == "mufu":
            fn = next(x for x in fl if x in MUFU)
            return self.set_f(o[0], MUFU[fn].format(a=self.f(o[1])), fl)
        if op == "rro":             # range reduction before sin/cos/exp2: the host functions need none
            return self.set_f(o[0], self.f(o[1]))
        if op == "mov":
            return self.set_u(o[0], self.u(o[1]))
        if op == "mov32i":
            return self.set_u(o[0], self.u(o[1]))
        if op == "sel":
            return self.set_u(o[0], f"{self.p(o[3])} ? {self.u(o[1])} : {self.u(o[2])}")
        if op == "i2f":
            src = f"int({self.u(o[1])})" if "s32" in fl else self.u(o[1])
            return self.set_f(o[0], f"float({src})")
        if op == "f2i":
            rnd = next((ROUND[x] for x in fl if x in ROUND), "rint")
            val = f"{rnd}({self.f(o[1])})"
            return self.set_u(o[0], f"uint(int({val}))" if "s32" in fl else f"uint({val})")
        if op == "f2f":
            rnd = next((ROUND[x] for x in fl if x in ROUND), None)
            a = self.f(o[1])
            return self.set_f(o[0], f"{rnd}({a})" if rnd else a, fl)
        if op == "i2i":
            return self.set_u(o[0], self.u(o[1]))
        if op in ("fsetp", "isetp", "fset"):
            return self.compare(op, fl, o)
        if op == "psetp":
            # Two predicate outputs from three predicate inputs:
            # dst = (a OP1 b) OP2 c, dst2 = !(a OP1 b) OP2 c.
            ops = [x for x in fl if x in ("and", "or", "xor")]
            if len(ops) != 2:
                return None
            join = {"and": "&&", "or": "||", "xor": "!="}
            ab = f"({self.p(o[2])} {join[ops[0]]} {self.p(o[3])})"
            c = self.p(o[4])
            first = self.set_p(o[0], f"{ab} {join[ops[1]]} {c}")
            second = self.set_p(o[1], f"!{ab} {join[ops[1]]} {c}")
            return (first + " " + second).strip()
        if op == "iadd":
            return self.set_u(o[0], f"{self.u(o[1])} + {self.u(o[2])}")
        if op == "shl":
            return self.set_u(o[0], f"{self.u(o[1])} << ({self.u(o[2])} & 31u)")
        if op == "shr":
            a = f"uint(int({self.u(o[1])}) >> ({self.u(o[2])} & 31u))" if "u32" not in fl else f"{self.u(o[1])} >> ({self.u(o[2])} & 31u)"
            return self.set_u(o[0], a)
        if op in ("lop", "lop32i"):
            return self.logic(op, fl, o)
        if op == "xmad":
            return self.xmad(fl, o)
        if op == "ld":
            return self.load(fl, o)
        if op == "st":
            return self.store(fl, o)
        if op == "ipa":
            return self.interpolate(fl, o)
        if op in ("texs", "tlds", "tld4s"):
            return self.sample(op, fl, o)
        if op == "tex":
            return self.sample_vector(fl, o)
        if op == "txq":
            return self.query(fl, o)
        if op == "bfi":
            # Insert the low `len` bits of the source into the base at `pos`.
            pos, length = f"({self.u(o[2])} & 0xffu)", f"(({self.u(o[2])} >> 8) & 0xffu)"
            mask = f"((1u << {length}) - 1u)"
            return self.set_u(o[0], f"({self.u(o[3])} & ~({mask} << {pos})) | (({self.u(o[1])} & {mask}) << {pos})")
        if op == "kil":
            return "discard_fragment();"
        return None

    def compare(self, op, fl, o):
        if op == "fset":
            dst, a, b, pred = o[0], self.f(o[1]), self.f(o[2]), self.p(o[3])
            table = FLOAT_CMP
        elif op == "fsetp":
            dst, dst2, a, b, pred = o[0], o[1], self.f(o[2]), self.f(o[3]), self.p(o[4])
            table = FLOAT_CMP
        else:
            dst, dst2, pred = o[0], o[1], self.p(o[4])
            signed = "u32" not in fl
            a = f"int({self.u(o[2])})" if signed else self.u(o[2])
            b = f"int({self.u(o[3])})" if signed else self.u(o[3])
            table = INT_CMP
        code = next(x for x in fl if x in table)
        combine = next((x for x in fl if x in ("and", "or", "xor")), "and")
        join = {"and": "&&", "or": "||", "xor": "!="}[combine]
        cmp = "(" + table[code].format(a=a, b=b) + ")"
        if op == "fset":
            on = "0x3f800000u" if "bf" in fl else "0xffffffffu"
            return self.set_u(dst, f"({cmp} {join} {pred}) ? {on} : 0u")
        first = self.set_p(dst, f"{cmp} {join} {pred}")
        second = self.set_p(dst2, f"!{cmp} {join} {pred}")
        return (first + " " + second).strip()

    def logic(self, op, fl, o):
        fn = next(x for x in fl if x in ("and", "or", "xor", "passb"))
        if op == "lop32i":
            dst, a, b, pdst = o[0], self.u(o[1]), self.u(o[2]), None
        else:
            pdst, dst, a, b = o[0], o[1], self.u(o[2]), self.u(o[3])
        expr = b if fn == "passb" else f"({a} {'&' if fn == 'and' else '|' if fn == 'or' else '^'} {b})"
        out = [self.set_u(dst, expr)]
        if pdst is not None and pdst[0][2] != 7:
            test = next((x for x in fl if x in ("z", "nz", "t", "f")), None)
            if test in ("z", "nz"):
                out.append(self.set_p(pdst, f"{expr} {'==' if test == 'z' else '!='} 0u"))
            elif test == "t":
                out.append(self.set_p(pdst, "true"))
            else:
                out.append(self.set_p(pdst, "false"))
        return " ".join(x for x in out if x)

    def xmad(self, fl, o):
        # 16-bit multiply-add, the building block of 32-bit integer multiplies.
        if any(x not in ("psl",) for x in fl):
            return None
        a = self.u(o[1])
        a16 = f"({a} >> 16)" if "h1" in o[1][1] else f"({a} & 0xffffu)"
        b = self.u(o[2])
        b16 = f"({b} >> 16)" if "h1" in o[2][1] else f"({b} & 0xffffu)"
        prod = f"({a16} * {b16})"
        if "psl" in fl:
            prod = f"({prod} << 16)"
        return self.set_u(o[0], f"{prod} + {self.u(o[3])}")

    def load(self, fl, o):
        t = o[1][0]
        if t[0] == "mem" and t[1] == "c":
            first = self.set_u(o[0], self.cbuf(t))
            if "b64" in fl:
                n = o[0][0][2]
                nxt = (("reg", "r", n + 1), set())
                second = self.set_u(nxt, self.cbuf((t[0], t[1], t[2], t[3], t[4] + 4)))
                return first + " " + second
            return first if "b32" in fl else None
        if t[0] == "mem" and t[1] == "a" and t[3] in (None, 255) and "b32" in fl and not self.fragment:
            return self.set_f(o[0], self.attribute_in(t[4]))
        return None

    def store(self, fl, o):
        t = o[0][0]
        if t[0] == "mem" and t[1] == "a" and t[3] in (None, 255) and "b32" in fl and not self.fragment:
            return f"{self.attribute_out(t[4])} = {self.f(o[1])};"
        return None

    def interpolate(self, fl, o):
        t = o[1][0]
        if not self.fragment or t[3] not in (None, 255):
            return None
        value = self.attribute_in(t[4])
        position = 0x70 <= t[4] < 0x80
        if "pass" in fl and not position:
            # Asked for without perspective correction; Metal hands over the corrected value.
            self.reads_position = True
            value = f"({value} * in.position.w)"
        elif "pass" not in fl and position:
            return None
        if any(x in ("constant", "sc") for x in fl):
            return None
        return self.set_f(o[0], value, fl)

    def sample_vector(self, fl, o):
        """`tex`: coordinates in consecutive registers, results in consecutive registers."""
        dst, coord, aux, handle, mask = o[0][0][2], o[1][0][2], o[2], o[3][0][1], o[4][0][1]
        if "t2d" not in fl or any(x not in ("t2d", "lz", "aoffi", "nodep") for x in fl):
            return None
        b = self.texture(handle, "2d")
        nxt = lambda n: (("reg", "r", n), set())
        uv = f"float2({self.f(nxt(coord))}, {self.f(nxt(coord + 1))})"
        if "aoffi" in fl:
            # A texel offset packed four bits a side into the next operand.
            a = self.u(aux)
            ox, oy = f"float(int({a} << 28) >> 28)", f"float(int({a} << 24) >> 28)"
            uv = f"({uv} + float2({ox}, {oy}) / float2(t{b}.get_width(), t{b}.get_height()))"
        lod = ", level(0)" if "lz" in fl or not self.fragment else ""
        out, n = [f"float4 t = t{b}.sample(s{b}, {uv}{lod});"], dst
        for bit in range(4):
            if mask & (1 << bit):
                out.append(self.set_u(nxt(n), f"as_type<uint>(t.{'xyzw'[bit]})"))
                n += 1
        return "{ " + " ".join(out) + " }"

    def query(self, fl, o):
        """`txq dimension`: the texture's size at a mip level, as integers."""
        if "dimension" not in fl:
            return None
        dst, lod, handle, mask = o[0][0][2], self.u(o[1]), o[2][0][1], o[3][0][1]
        b = self.texture(handle, "2d")
        vals = [f"t{b}.get_width({lod})", f"t{b}.get_height({lod})", "1u", f"t{b}.get_num_mip_levels()"]
        out, n = [], dst
        for bit in range(4):
            if mask & (1 << bit):
                out.append(self.set_u((("reg", "r", n), set()), f"uint({vals[bit]})"))
                n += 1
        return " ".join(out)

    def sample(self, op, fl, o):
        dst1, dst0, c0, c1, handle = o[0], o[1], o[2], o[3], o[4][0][1]
        if op == "tld4s":
            # Gather: one channel of the four texels around the point.
            comp = next((x for x in fl if x in ("r", "g", "b", "a")), None)
            if comp is None or any(x not in ("r", "g", "b", "a", "nodep") for x in fl):
                return None
            b = self.texture(handle, "2d")
            call = f"t{b}.gather(s{b}, float2({self.f(c0)}, {self.f(c1)}), int2(0), component::{'xyzw'['rgba'.index(comp)]})"
            slots = [dst0[0][2], dst0[0][2] + 1, dst1[0][2], dst1[0][2] + 1]
            out = [f"float4 t = {call};"]
            for i in range(4):
                if (i < 2 and dst0[0][2] != 255) or (i >= 2 and dst1[0][2] != 255):
                    out.append(self.set_u((("reg", "r", slots[i]), set()), f"as_type<uint>(t.{'xyzw'[i]})"))
            return "{ " + " ".join(out) + " }"
        mask = next((x for x in fl if set(x) <= set("rgba") and x), None)
        if mask is None:
            return None
        if op == "tlds":
            if "t2d" not in fl or "lz" not in fl:
                return None
            b = self.texture(handle, "2d")
            call = f"t{b}.read(uint2({self.u(c0)}, {self.u(c1)}), 0)"
        elif "t3d" in fl or "tcube" in fl:
            b = self.texture(handle, "3d" if "t3d" in fl else "cube")
            x = self.f(c0)
            y = self.f((("reg", "r", c0[0][2] + 1), set()))
            call = f"t{b}.sample(s{b}, float3({x}, {y}, {self.f(c1)}){self.level()})"
        elif "t2d" in fl:
            b = self.texture(handle, "2d")
            call = f"t{b}.sample(s{b}, float2({self.f(c0)}, {self.f(c1)}){self.level()})"
        else:
            return None
        # Selected components go, in order, to dst0, dst0+1, dst1, dst1+1.
        slots = [dst0[0][2], dst0[0][2] + 1, dst1[0][2], dst1[0][2] + 1]
        out = [f"float4 t = {call};"]
        for i, comp in enumerate(mask):
            if slots[i] < 255 and not (i >= 2 and dst1[0][2] == 255) and not (i < 2 and dst0[0][2] == 255):
                out.append(self.set_u((("reg", "r", slots[i]), set()), f"as_type<uint>(t.{'xyzw'['rgba'.index(comp)]})"))
        return "{ " + " ".join(out) + " }"

    def level(self):
        return "" if self.fragment else ", level(0)"

    # ---- whole shader ----

    def translate(self):
        ins = [(pc, w, t) for pc, w, t in gm107.instructions(self.sh.code) if w]
        # Everything from the stop-here loop that follows the program is not code.
        for i, (pc, w, t) in enumerate(ins):
            if t and gm107.mnemonic(t) == "bra" and any(x == ("target", pc) for x in t):
                ins = ins[:i]
                break
        # A branch may aim at a scheduling word or padding; execution resumes at the next
        # real instruction, so that is where the block starts.
        pcs = [pc for pc, _, _ in ins]

        def land(target):
            return next((pc for pc in pcs if pc >= target), None)

        if not self.fragment:
            self.find_integer_inputs(ins)
        targets = sorted({land(x[1]) for _, _, t in ins if t for x in t if x[0] == "target"} - {None})
        backward = any(x[1] <= pc for pc, _, t in ins if t for x in t if x[0] == "target")
        starts = sorted(set(targets) | ({ins[0][0]} if ins else set()))
        block_of = {}
        blocks, cur = [], None
        for pc, w, t in ins:
            if pc in starts or cur is None:
                cur = []
                blocks.append(cur)
                block_of[pc] = len(blocks) - 1
            cur.append((pc, w, t))
            if t and gm107.mnemonic(t) in ("bra", "exit"):
                cur = None
        end = len(blocks)
        ret = "return out;" if not self.fragment else None      # the fragment return is built in assemble()
        body = []
        for k, blk in enumerate(blocks):
            lines = []
            for pc, w, t in blk:
                if t is None:
                    self.unhandled.append(f"word {w:#x}")
                    continue
                op = gm107.mnemonic(t)
                flags, ops, ex = self.split(t)
                if op == "bra":
                    tgt = next((x[1] for x in t if x[0] == "target"), None)
                    past_end = tgt is not None and land(tgt) is None
                    tgt = land(tgt) if tgt is not None and not past_end else tgt
                    if tgt is None or any(x != "bra" for x in flags) or (tgt not in block_of and not past_end):
                        self.unhandled.append("bra " + " ".join(x for x in flags if x != "bra"))
                        continue
                    # A branch past the last instruction ends the shader.
                    stmt = f"jump = {block_of.get(tgt, end)}u;" + (" continue;" if backward else "")
                elif op == "exit":
                    stmt = "@RETURN@"
                else:
                    try:
                        stmt = self.emit(op, [x for x in flags if x != op], ops)
                    except (KeyError, StopIteration, IndexError) as e:
                        stmt = None
                    if stmt is None:
                        self.unhandled.append(op + " " + " ".join(x for x in flags if x != op))
                        continue
                if not stmt:
                    continue
                lines.append(f"if ({ex}) {{ {stmt} }}" if ex else stmt)
            body.append(lines)
        return self.assemble(body, backward)

    def assemble(self, body, backward):
        sh, L = self.sh, []
        if self.fragment:
            depth_only = struct.unpack_from("<I", sh.code, 0x30 + 0x48)[0] == 0
            if depth_only:
                # Registers hold the enabled colour components first; with none, depth is r1.
                returns = f"{{ FOut out; out.depth = as_type<float>({self.reg(1)}); return out; }}"
            else:
                c = ", ".join(f"as_type<float>({self.reg(i)})" for i in range(4))
                returns = f"{{ FOut out; out.color = float4({c}); return out; }}"
        else:
            returns = "return out;"
        code = []
        single = len(body) <= 1
        for k, lines in enumerate(body):
            text = [l.replace("@RETURN@", returns) for l in lines]
            if single:
                code += ["    " + l for l in text]
            elif backward:
                code.append(f"        case {k}u:")
                code += ["            " + l for l in text]
                code.append(f"            jump = {k + 1}u; continue;")
            else:
                code.append(f"    if (jump <= {k}u) {{")
                code += ["        " + l for l in text]
                code.append("    }")
        L += ["#include <metal_stdlib>", "using namespace metal;", ""]
        io = lambda locs, name: [f"    {'uint4' if name.startswith('attribute') and l in self.integer_inputs else 'float4'} a{l} [[{name.format(l=l)}]];"
                                 for l in sorted(locs)]
        if self.fragment:
            L += ["struct FIn {", "    float4 position [[position]];"] + io(self.inputs, "user(locn{l})") + ["};", ""]
            L += ["struct FOut {", "    float depth [[depth(any)]];" if depth_only else "    float4 color [[color(0)]];", "};", ""]
        else:
            L += ["struct VIn {"] + io(self.inputs, "attribute({l})") + ["};", ""]
            L += ["struct VOut {", "    float4 position [[position]];"] + io(self.outputs, "user(locn{l})") + ["};", ""]
        params = ["FIn in [[stage_in]]" if self.fragment else "VIn in [[stage_in]]"]
        if not self.fragment and not self.inputs:
            params = []
        if self.vertex_id:
            params.append("uint vid [[vertex_id]]")
        if self.instance_id:
            params.append("uint iid [[instance_id]]")
        params += [f"const device uint * c{i} [[buffer({i})]]" for i in sorted(self.cbufs)]
        for b, kind in sorted(self.textures.items()):
            params += [f"texture{kind}<float> t{b} [[texture({b})]]", f"sampler s{b} [[sampler({b})]]"]
        head = ("fragment FOut d3_fragment(" if self.fragment else "vertex VOut d3_vertex(") + ",\n        ".join(params) + ")"
        L += [head, "{"]
        if not self.fragment:
            L.append("    VOut out = {};")
        if self.regs:
            L.append("    uint " + ", ".join(f"r{n} = 0u" for n in sorted(self.regs)) + ";")
        if self.preds:
            L.append("    bool " + ", ".join(f"p{n} = false" for n in sorted(self.preds)) + ";")
        if single:
            L += code
        elif backward:
            L += ["    uint jump = 0u;", "    for (int guard = 0; guard < 65536; guard++) {", "        switch (jump) {"] + code
            L += [f"        default: {returns}", "        }", "    }"]
        else:
            L += ["    uint jump = 0u;"] + code
        L += ["    " + returns, "}", ""]
        return "\n".join(L)


def translate(blob):
    t = Translator(Shader(blob))
    return t.translate(), t.unhandled


def fill_cache(folder):
    folder = Path(folder)
    done = failed = 0
    for f in sorted(folder.glob("*.code")):
        out = f.with_suffix(".metal")
        if out.exists():
            continue
        try:
            t = Translator(CodeOnly(f.read_bytes()))
            text, unhandled = t.translate(), t.unhandled
        except Exception as e:                      # one bad shader must not stop the rest
            text, unhandled = "", [f"translator error: {e}"]
        if unhandled:
            f.with_suffix(".failed").write_text("\n".join(sorted(set(unhandled))) + "\n")
            failed += 1
        else:
            out.write_text(text)
            f.with_suffix(".failed").unlink(missing_ok=True)    # it failed with an older translator
            done += 1
    print(f"{done} shaders translated, {failed} could not be; cache now holds {len(list(folder.glob('*.metal')))}")


def main():
    if len(sys.argv) == 2:
        text, unhandled = translate(Path(sys.argv[1]).read_bytes())
        print(text)
        for u in unhandled:
            print("// NOT TRANSLATED:", u)
    elif len(sys.argv) == 3 and sys.argv[1] == "--cache":
        fill_cache(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "--all":
        import collections
        out = Path(sys.argv[3])
        out.mkdir(parents=True, exist_ok=True)
        why, ok, paths = collections.Counter(), 0, []
        files = sorted(Path(sys.argv[2]).glob("*.glslc"))
        for f in files:
            text, unhandled = translate(f.read_bytes())
            for u in set(unhandled):
                why[u] += 1
            if not unhandled:
                ok += 1
                p = out / (f.stem + ".metal")
                p.write_text(text)
                paths.append(str(p))
        (out / "list.txt").write_text("\n".join(paths) + "\n")
        print(f"{ok} of {len(files)} shaders translated completely; written to {out}")
        for k, v in why.most_common():
            print(f"  {v:5} shaders use: {k}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
