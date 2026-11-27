# Reads the 1.0 type descriptors out of the emulated memory (emu.py's mem.pkl).
# Usage: types10.py mem.pkl [binary] [TypeName ...]   (writes types10.json next to mem.pkl;
#   with the binary, its read-only __TEXT replaces the emulated copy)
import json, os, pickle, re, struct, sys

dump = pickle.load(open(sys.argv[1], 'rb'))
# __TEXT is read-only in the real process; take it from the file (the emulation's stubs
# can let stray writes through).
if len(sys.argv) > 2 and os.path.isfile(sys.argv[2]):
    binary = open(sys.argv.pop(2), 'rb').read()
    dump['mem'][0x1000] = binary[:len(dump['mem'][0x1000])]
regions = sorted(dump['mem'].items())

def find(a):
    for base, b in regions:
        if base <= a < base + len(b):
            return b, a - base
    return None, 0

def rd32(a):
    b, o = find(a)
    if b is None or o + 4 > len(b):
        return None
    return struct.unpack_from('<I', b, o)[0]

def rd(a, fmt):
    b, o = find(a)
    if b is None or o + struct.calcsize(fmt) > len(b):
        return None
    return struct.unpack_from(fmt, b, o)

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_:<>]*$')
def cstr(a):
    if not a:
        return None
    b, o = find(a)
    if b is None:
        return None
    e = b.find(b'\0', o, o + 200)
    if e < 0:
        return None
    try:
        return b[o:e].decode('ascii')
    except UnicodeDecodeError:
        return None

FIELD = 0x8c
def fields_at(a):
    out = []
    for i in range(2000):
        f = a + i * FIELD
        name_p, type_p, off = rd(f, '<3I') or (None, None, None)
        if type_p is None:
            return None
        if name_p == 0:
            out.append(None)  # terminator: its offset is the struct size
            out.append(off)
            return out
        n = cstr(name_p)
        if n is None or (n and not IDENT.match(n)):
            return None
        if type_p not in typeptrs:
            return None
        out.append((f, n, type_p, off))
    return None

# Type pointers: anything whose +4 names an identifier (checked lazily).
class TypePtrs:
    def __contains__(self, p):
        q = rd32(p + 4) if p else None
        n = cstr(q) if q else None
        return bool(n) and bool(IDENT.match(n))
typeptrs = TypePtrs()
# Candidates: anything whose +4 is an identifier and +0xc a field array.
descs = {}
for base, b in regions:
    for o in range(0, len(b) - 16, 4):
        name_p, ver, fp = struct.unpack_from('<3I', b, o + 4)
        if fp == 0 or name_p == 0:
            continue
        n = cstr(name_p)
        if not n or not IDENT.match(n):
            continue
        fl = fields_at(fp)
        if not fl:
            continue
        descs[base + o] = (n, ver, fl)

def tname(p):
    if p in descs:
        return descs[p][0]
    n = cstr(rd32(p + 4) or 0) if rd32(p + 4) else None
    return n or ('?%x' % p)

# Basic descriptors have no fields; name them by their +4 too.
out = {}
for a, (n, ver, fl) in descs.items():
    size = fl[-1]
    fs = []
    for f, fname, tp, off in fl[:-2]:
        raw = rd(f, '<35I')
        fs.append({'name': fname, 'type': tname(tp), 'offset': off,
                   'default': raw[3], 'min': raw[4], 'max': raw[5], 'flags': raw[6],
                   'aux': tname(raw[7]) if raw[7] else None, 'raw': list(raw[8:20])})
    key = n if n not in out else '%s@%x' % (n, a)
    out[key] = {'addr': a, 'version': ver, 'size': size, 'fields': fs}
json.dump(out, open(os.path.join(os.path.dirname(sys.argv[1]) or '.', 'types10.json'), 'w'), indent=1)
print(len(out), 'types')
for want in sys.argv[2:]:
    t = out.get(want)
    if not t:
        print('no', want); continue
    print('== %s size %#x version %d' % (want, t['size'], t['version']))
    for f in t['fields']:
        print('   %#6x %-32s %-24s aux=%s flags=%#x raw=%s' % (f['offset'], f['name'], f['type'], f['aux'], f['flags'], f['raw'][:5]))
