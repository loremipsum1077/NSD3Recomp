# Walks a 2.7.7 SNO file the way the loader does and reports arrays that point outside it
# or whose size is not a whole number of elements.
# Usage: check.py <tables folder> <root type> <file>
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert as C


def check(root, src):
    """Problems found in the 2.7.7 file src (bytes) of root type, as strings."""
    T = C.T277
    bad = []

    def walk(t, s, path):
        if t in C.BASIC or t not in T:
            return
        for f in T[t]['fields']:
            o = s + f['offset']
            if f['type'] in ('DT_VARIABLEARRAY', 'DT_TAGMAP'):
                off, size = struct.unpack_from('<2I', src, o + f['ser'])
                if not size:
                    continue
                if 0x10 + off + size > len(src):
                    bad.append('%s.%s@%#x: %s at %#x size %#x is outside the file' % (path, t, f['offset'], f['aux'], off, size))
                    continue
                if f['type'] == 'DT_VARIABLEARRAY':
                    try:
                        es = C.elem_size(f['aux'], T)
                    except KeyError:
                        continue
                    if size % es:
                        bad.append('%s.%s: %s size %d is not a multiple of %d' % (path, t, f['aux'], size, es))
                    for i in range(size // es):
                        walk(f['aux'], 0x10 + off + i * es, '%s.%s[%d]' % (path, t, i))
            elif f['type'] == 'DT_FIXEDARRAY':
                es = C.elem_size(f['aux'], T)
                for i in range(f['count']):
                    walk(f['aux'], o + i * es, path + '.' + t)
            elif f['type'] in T:
                walk(f['type'], o, path + '.' + t)

    walk(root, 0x10, '')
    return bad


if __name__ == '__main__':
    C.load(sys.argv[1])
    problems = check(sys.argv[2], open(sys.argv[3], 'rb').read())
    for p in problems:
        print(p)
    print('%d problems' % len(problems))
