#!/usr/bin/env python3
"""Reads the 2.7.7 game's CPK archives (romfs/CPKs), following the decomp's
X/Common/Console/IO/ConsoleIO_ArchiveReader.cpp and ConsoleIO_Decompress.cpp, and the
file list CoreTOC.dat.
Usage: cpk.py list ARCHIVE [substr]   |   cpk.py get ARCHIVE NAME OUTFILE
"""
import struct, sys, zlib

KEY = 0x873ae8e53c11d761  # XOR64_ENCRYPTION_KEY in the decomp's ConsoleIO_Decompress.cpp

def decxor(data):
    k = KEY
    out = bytearray(len(data))
    for i, b in enumerate(data):
        o = b ^ (k & 0xff)
        out[i] = o
        k = ((k >> 8) | (((k ^ o) & 0xff) << 56)) & 0xFFFFFFFFFFFFFFFF
    return bytes(out)

def highest_bit(v):
    n = 0
    while v:
        n += 1
        v >>= 1
    return n

class Bits:
    def __init__(self, b): self.b = b + b'\0' * 16
    def get(self, idx, cnt):
        if cnt == 0: return 0
        s = idx >> 3; e = (idx + cnt + 7) >> 3
        v = int.from_bytes(self.b[s:e], 'big')
        tot = (e - s) * 8
        return (v >> (tot - (idx - s * 8) - cnt)) & ((1 << cnt) - 1)

class CPK:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2); self.size = self.f.tell(); self.f.seek(0)
        h = self.f.read(0x48)
        (magic, ver, dsize, flags, nfiles, nloc, hsec, fsb, flcb, flib, lb, cs2do, ds2cs, crc, rss, css) = struct.unpack('<IIqIiiiiiiiiiIii', h[:68])
        assert magic == 0xA1B2C3D4, hex(magic)
        off = 0x48
        if ver == 6:
            rss, css = 65536, 16384; off -= 8
        self.flags, self.nfiles, self.rss, self.css, self.dsize = flags, nfiles, rss, css, dsize
        self.fsb, self.flcb, self.flib, self.lb, self.cs2do, self.ds2cs = fsb, flcb, flib, lb, cs2do, ds2cs
        if flags & 8:
            self.hsize = hsec; pad = 1
        else:
            self.hsize = hsec * rss; pad = rss
        self.ncomp = (self.size - hsec * pad + css - 1) // css
        ndecomp = (dsize + css - 1) // css
        fib = fsb + flcb + flib + 64
        def rd(n):
            nonlocal off
            self.f.seek(off); d = self.f.read(n); off += n; return d
        self.fi = Bits(rd(((fib * nfiles + 7) & ~7) // 8))
        self.loc = Bits(rd(((nloc * lb + 7) & ~7) // 8))
        self.compressed = bool(flags & 2)
        if self.compressed:
            self.c2d = Bits(rd(((lb * self.ncomp + 7) & ~7) // 8))
            dbits = highest_bit(self.ncomp)
            self.d2c = Bits(rd(((dbits * ndecomp + 7) & ~7) // 8))
        offs = struct.unpack('<%di' % nfiles, rd(nfiles * 4))
        names = rd(hsec * pad - off)
        self.names = []
        for o in offs:
            e = names.index(b'\0', o)
            self.names.append(names[o:e].decode('latin1'))
        self.fib = fib
        self.cache = {}

    def info(self, i):
        b = i * self.fib
        h = self.fi.get(b, 64); b += 64
        sz = self.fi.get(b, self.fsb); b += self.fsb
        lc = self.fi.get(b, self.flcb); b += self.flcb
        li = self.fi.get(b, self.flib)
        return h, sz, lc, li

    def c2off(self, c):
        return self.c2d.get(c * self.cs2do, self.cs2do)

    def comp_sector(self, c):
        if c in self.cache: return self.cache[c]
        self.f.seek(self.hsize + c * self.css)
        raw = self.f.read(self.css * 6)
        start = self.c2off(c)
        end = self.dsize if c == self.ncomp - 1 else self.c2off(c + 1)
        want = end - start
        out = bytearray(); p = 0
        while len(out) < want and p + 6 <= len(raw):
            lo, hi, ec = struct.unpack_from('<HHH', raw, p); p += 6
            dsz = lo | (hi << 16); csz = ec & 0x7fff; enc = ec >> 15
            data = raw[p:p + csz]; p += csz
            if enc: data = decxor(data)
            d = zlib.decompressobj().decompress(data)
            out += d
        if len(self.cache) > 64: self.cache.clear()
        self.cache[c] = bytes(out)
        return self.cache[c]

    def read_at(self, offset, size):
        if not self.compressed:
            self.f.seek(self.hsize + offset); return self.f.read(size)
        c = self.d2c.get((offset // self.css) * highest_bit(self.ncomp), highest_bit(self.ncomp))
        while c < self.ncomp - 1 and self.c2off(c + 1) <= offset: c += 1
        out = bytearray(); cur = offset
        while len(out) < size:
            s = self.comp_sector(c); base = self.c2off(c)
            out += s[cur - base: cur - base + size - len(out)]
            cur = base + len(s); c += 1
        return bytes(out)

    def get(self, i):
        h, sz, lc, li = self.info(i)
        loc = self.loc.get(li * self.lb, self.lb)
        return self.read_at(loc, sz)

    def find(self, name):
        """The file's bytes, or None. Names look like 'Worlds\\PvP_Duel_Small.wrl'."""
        if not hasattr(self, 'index'):
            self.index = {n.lower(): i for i, n in enumerate(self.names)}
        i = self.index.get(name.lower())
        return None if i is None else self.get(i)


def read_toc(d):
    """CoreTOC.dat bytes -> [(group, sno, name)] (X/Common/SNO/SNOTOC.cpp's layout: 70 group
    counts, 70 group offsets, then per group its entries and their names)."""
    cnt = struct.unpack_from('<70i', d, 0)
    off = struct.unpack_from('<70i', d, 280)
    base = None
    for b in (840, 844, 1120, 1124):
        if struct.unpack_from('<i', d, b + off[1])[0] == 1:
            base = b
            break
    rows = []
    for g in range(70):
        s = base + off[g]
        names = s + cnt[g] * 12
        for i in range(cnt[g]):
            grp, sno, no = struct.unpack_from('<iii', d, s + i * 12)
            e = d.index(b'\0', names + no)
            rows.append((grp, sno, d[names + no:e].decode('latin1')))
    return rows


if __name__ == '__main__':
    c = CPK(sys.argv[2])
    if sys.argv[1] == 'list':
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for i, n in enumerate(c.names):
            if sub in n.lower(): print(i, n, c.info(i)[1])
    elif sys.argv[1] == 'get':
        for i, n in enumerate(c.names):
            if n == sys.argv[3]:
                open(sys.argv[4], 'wb').write(c.get(i)); print('ok', len(c.get(i))); break
        else: print('not found')
