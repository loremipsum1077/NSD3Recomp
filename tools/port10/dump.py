# Prints an SNO file's struct tree from the type tables.
# Usage: dump.py <tables folder> 10|277 <root type> <file> [max elements per array] [max depth]
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert as C

C.load(sys.argv.pop(1))
ver, root, path = sys.argv[1:4]
MAXN = int(sys.argv[4]) if len(sys.argv) > 4 else 3
MAXD = int(sys.argv[5]) if len(sys.argv) > 5 else 8
src = open(path, 'rb').read()
T = C.T10 if ver == '10' else C.T277

def ser(field_off, f):
    if ver == '10':
        rel = f['raw'][0]
        rel = rel - (1 << 32) if rel >= 1 << 31 else rel
    else:
        rel = f['ser']
    return struct.unpack_from('<2I', src, field_off + rel)

def show(t, s, ind, depth):
    if t in C.BASIC:
        v = struct.unpack_from(C.BASIC[t][0], src, s)
        print('%s%s' % (ind, v if len(v) > 1 else v[0]))
        return
    if t not in T:
        print('%s<%s, %d bytes at %#x>' % (ind, t, 0, s)); return
    for f in T[t]['fields']:
        ft, o = f['type'], s + f['offset']
        label = '%s%#x %s %s' % (ind, f['offset'], f['name'] or '', ft)
        if ft == 'SerializeData':
            continue
        if ft == 'DT_CHARARRAY':
            print(label, repr(src[o:o + 64].split(b'\0')[0])); continue
        if ft in C.BASIC:
            v = struct.unpack_from(C.BASIC[ft][0], src, o)
            if ft in ('DT_INT', 'DT_UINT'):
                v = tuple('%d/%#x' % (x, x & 0xffffffff) for x in v)
            print(label, v if len(v) > 1 else v[0]); continue
        if ft in ('DT_VARIABLEARRAY', 'DT_TAGMAP'):
            off, size = ser(o, f)
            aux = f['aux']
            try:
                es = C.elem_size(aux, T)
            except KeyError:
                es = 0
            n = size // es if es else 0
            print(label, '%s x%d (off %#x size %#x)' % (aux, n, off, size))
            if depth < MAXD and ft == 'DT_VARIABLEARRAY':
                for i in range(min(n, MAXN)):
                    print('%s  [%d]' % (ind, i))
                    show(aux, 0x10 + off + i * es, ind + '    ', depth + 1)
            continue
        if ft == 'DT_FIXEDARRAY':
            n = f['raw'][1] if ver == '10' else f['count']
            es = C.elem_size(f['aux'], T)
            print(label, '%s[%d]' % (f['aux'], n))
            for i in range(min(n, MAXN)):
                print('%s  [%d]' % (ind, i))
                show(f['aux'], o + i * es, ind + '    ', depth + 1)
            continue
        print(label)
        if depth < MAXD:
            show(ft, o, ind + '    ', depth + 1)

print('header', src[:0x10].hex(), 'size', len(src))
show(root, 0x10, '', 0)
