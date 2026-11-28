# Converts a Diablo III 1.0 (PC, 32-bit) SNO file to the 2.7.7 (Switch, 64-bit) layout,
# field by field, from the two versions' type descriptor tables:
#   types10.json   from the 1.0 Mac client (emu.py + types10.py)
#   types277.json  from the 2.7.7 build (D3_DUMP_TYPES, src/mods/loose_files.cpp)
# Fields are matched in declaration order by type; where 2.7.7 changed a field's meaning
# (names became hashes, the navmesh became compressed) a rule below does the conversion.
#
# Usage: convert.py <tables folder> <root type> <in 1.0 file> <out file> <2.7.7 sample of the same group>
#   The sample supplies the 2.7.7 file header (version, asset version, XML hash).
# port10.py uses it as a module: load(), then Converter and convert_file().
import json, os, struct, sys

T10 = T277 = None

def load(folder):
    """Reads types10.json and types277.json from folder."""
    global T10, T277
    T10 = json.load(open(os.path.join(folder, 'types10.json')))
    T277 = {}
    for v in json.load(open(os.path.join(folder, 'types277.json'))).values():
        fs = v['fields']
        t = {'version': v['version'], 'size': fs[-1]['offset'], 'fields': [f for f in fs if f['type'] != 'DT_NULL']}
        T277.setdefault(v['name'], t)
        T277.setdefault('%s#%d' % (v['name'], t['size']), t)
    # Some names are used by two types (Material: a colour block, and the Material group's
    # file). Each is also kept as <name>#<size>, for naming a file's root type.
    for k, t in list(T10.items()):
        if 'size' in t:
            T10.setdefault('%s#%d' % (k.split('@')[0], t['size']), t)

# Basic types: (struct format, size). Names differ a little between the versions.
BASIC = {
    'DT_INT': ('<i', 4), 'DT_UINT': ('<I', 4), 'DT_FLOAT': ('<f', 4), 'DT_SNO': ('<i', 4),
    'DT_GBID': ('<i', 4), 'DT_SNO_GROUP': ('<i', 4), 'DT_SNONAME_HANDLE': ('<i', 4),
    'DT_WORD': ('<H', 2), 'DT_BYTE': ('<B', 1), 'DT_INT64': ('<q', 8), 'DT_ENUM': ('<i', 4),
    'DT_FLAG': ('<i', 4), 'DT_TIME': ('<i', 4), 'DT_ANGLE': ('<f', 4), 'DT_PERCENT': ('<f', 4),
    'DT_HIGHPRECISIONPERCENT': ('<d', 8), 'DT_DATAID': ('<i', 4), 'DT_STARTLOC_NAME': ('<i', 4),
    'DT_ACTOR_GROUP': ('<i', 4), 'DT_ACTOR_ATTRIBUTE': ('<i', 4), 'DT_ANIM_TAG': ('<i', 4),
    'DT_VELOCITY': ('<f', 4), 'DT_ACCEL': ('<f', 4), 'DT_ANGULARVELOCITY': ('<f', 4),
    'DT_ANGULARACCEL': ('<f', 4), 'DT_MASS': ('<f', 4), 'DT_IMPULSE': ('<f', 4),
    'DT_TRANSLATEABLE': ('<i', 4),
    'DT_VECTOR3D': ('<3f', 12), 'Vector3D': ('<3f', 12), 'DT_VECTOR2D': ('<2f', 8),
    'Vector2D': ('<2f', 8), 'DT_IVECTOR2D': ('<2i', 8), 'IVector2D': ('<2i', 8),
    'DT_VECTOR4D': ('<4f', 16), 'DT_RGBACOLOR': ('<4B', 4), 'DT_RGBACOLORVALUE': ('<4f', 16),
    'RGBAColor': ('<4B', 4), 'RGBAColorValue': ('<4f', 16),
}
SAME = {'Vector3D': 'DT_VECTOR3D', 'IVector2D': 'DT_IVECTOR2D', 'Vector2D': 'DT_VECTOR2D',
        'RGBAColor': 'DT_RGBACOLOR', 'RGBAColorValue': 'DT_RGBACOLORVALUE'}

# 1.0 fields 2.7.7 dropped, which the order-and-type matching would otherwise pair with
# an unrelated field (by 1.0 offset).
DROP = {
    'Structure': {0x190, 0x290, 0x390, 0x490, 0x590, 0x594, 0x598},  # source file paths
}
# Pairs fixed by hand where the matching cannot tell (1.0 offset -> 2.7.7 offset).
PIN = {
    'SubObject': {0x48: 0x64, 0xc8: 0x68},  # material name, subobject name -> their hashes
    'Marker': {0x80: 0x8},  # eType (0x4 is the hash's padding)
}

def string_hash_i(s):
    h = 0
    for c in s.lower().encode('latin1'):
        h = (h * 33 + (c - 256 if c > 127 else c)) & 0xffffffff
    return h

def elem_size(tname, table):
    if tname in BASIC:
        return BASIC[tname][1]
    t = table.get(tname)
    if t is None:
        raise KeyError('unknown type %s' % tname)
    return t['size']

def kind(f):
    return f['type']

def compatible(a, b):
    ta, tb = a['type'], b['type']
    if ta == tb or SAME.get(ta) == tb:
        if ta in ('DT_VARIABLEARRAY', 'DT_FIXEDARRAY'):
            return (a['aux'] or '') == (b['aux'] or '') or SAME.get(a['aux']) == b['aux'] or a['aux'] in T277 and a['aux'] == b['aux']
        return True
    if ta == 'DT_CHARARRAY' and tb in ('DT_INT', 'HardpointHash'):
        return True  # 2.7.7 keeps a hash of the name
    return False

def align(fs10, fs277, t10=None, t277=None):
    """Longest common subsequence of compatible fields, in declaration order."""
    drop, pin = DROP.get(t10, set()), PIN.get(t277, {})
    # 2.7.7 count fields are filled from their arrays afterwards; matching them would
    # only pull a 1.0 field out of place.
    fs277 = [f for f in fs277 if not f['flags'] & 0x10000]
    pinned = []
    if drop or pin:
        by277 = {f['offset']: f for f in fs277}
        pinned = [(f, by277[pin[f['offset']]]) for f in fs10 if f['offset'] in pin]
        fs10 = [f for f in fs10 if f['offset'] not in drop and f['offset'] not in pin]
        fs277 = [f for f in fs277 if f['offset'] not in pin.values()]
    n, m = len(fs10), len(fs277)
    L = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            if compatible(fs10[i], fs277[j]):
                L[i][j] = 1 + L[i + 1][j + 1]
            else:
                L[i][j] = max(L[i + 1][j], L[i][j + 1])
    pairs, i, j = [], 0, 0
    while i < n and j < m:
        if compatible(fs10[i], fs277[j]) and L[i][j] == 1 + L[i + 1][j + 1]:
            pairs.append((fs10[i], fs277[j])); i += 1; j += 1
        elif L[i + 1][j] >= L[i][j + 1]:
            i += 1
        else:
            j += 1
    return pinned + pairs

class Out:
    def __init__(self, size):
        self.b = bytearray(size)
    def alloc(self, n):
        at = (len(self.b) + 7) & ~7
        self.b.extend(b'\0' * (at - len(self.b) + n))
        return at
    def put(self, off, fmt, *v):
        struct.pack_into(fmt, self.b, off, *v)

class Converter:
    """on_sno(sno) -> sno: sees every SNO number the file names and may renumber it.
    has(group, sno) -> bool: whether 2.7.7 (or this port) has that file; markers naming
    one it lacks are dropped. Both are optional."""
    def __init__(self, src, on_sno=None, has=None):
        self.src = src
        self.on_sno = on_sno
        self.has = has
        self.base = 0x10  # 1.0 serialize offsets count from the struct after the header
        self.log = []
        self.octree_subobjects = None
        self.blobs = {'idx': bytearray(), 'vtx': bytearray()}
        self.subobject_index = 0

    def rd(self, fmt, off):
        return struct.unpack_from(fmt, self.src, off)

    def array_bytes(self, f10, s, f):
        """The 2.7.7 bytes of one 1.0 variable array, without placing it in the block."""
        off, size = self.ser10(s, f10)
        if not size:
            return b''
        e10, e = elem_size(f10['aux'], T10), elem_size(f['aux'], T277)
        n = size // e10
        tmp = Out(n * e)
        for i in range(n):
            self.convert(f10['aux'], self.base + off + i * e10, f['aux'], tmp, i * e)
        return bytes(tmp.b)

    def ser10(self, field_off, f10):
        rel = f10['raw'][0]
        rel = rel - (1 << 32) if rel >= 1 << 31 else rel
        return self.rd('<2I', field_off + rel)  # (offset, size) at the field's nSerOffset

    def convert(self, t10, s, t277, out, d):
        """Converts one t10 struct at src offset s into t277 at out offset d."""
        rule = RULES.get(t277)
        if rule:
            return rule(self, t10, s, t277, out, d)
        if t10 in BASIC or t277 in BASIC:
            v = self.rd(BASIC[t10][0], s)
            if self.on_sno and t277 in ('DT_SNO', 'DT_SNONAME_HANDLE'):
                v = (self.on_sno(v[0]),)
            out.put(d, BASIC[t277][0], *v)
            return
        a, b = T10[t10], T277[t277]
        for f10, f in align(a['fields'], b['fields'], t10, t277):
            blob = BLOBS.get((t277, f['offset']))
            if blob:
                # Converted on their own and appended to the blob 2.7.7 reads after the block.
                self.blobs[blob] += self.array_bytes(f10, s + f10['offset'], f)
                continue
            self.field(f10, s + f10['offset'], f, out, d + f['offset'])
        if t277 in AFTER_STRUCT:
            AFTER_STRUCT[t277](self, s, out, d)
        # 2.7.7 count fields (flag 0x10000) hold the element count of the array whose
        # SerializeData sits at their nSerOffset.
        for f in b['fields']:
            if f['flags'] & 0x10000 and f['aux'] not in ('', 'DT_NULL'):
                off, size = struct.unpack_from('<2I', out.b, d + f['offset'] + f['ser'])
                out.put(d + f['offset'], '<i', size // elem_size(f['aux'], T277))

    def field(self, f10, s, f, out, d):
        t10, t = f10['type'], f['type']
        if t10 == 'SerializeData':
            return  # written by the array it describes
        if t10 == 'DT_CHARARRAY' and t in ('DT_INT', 'HardpointHash'):
            text = self.src[s:s + f10['raw'][1]].split(b'\0')[0].decode('latin1')
            out.put(d, '<I', string_hash_i(text) if text else 0)
            return
        if t10 == 'DT_CHARARRAY':
            n = min(f10['raw'][1], f['count'])
            out.b[d:d + n] = self.src[s:s + n]
            return
        if t10 == 'DT_FIXEDARRAY':
            n = min(f10['raw'][1], f['count'])
            e10, e = elem_size(f10['aux'], T10), elem_size(f['aux'], T277)
            for i in range(n):
                self.convert(f10['aux'], s + i * e10, f['aux'], out, d + i * e)
            return
        if t10 in ('DT_VARIABLEARRAY', 'DT_TAGMAP', 'DT_CSTRING'):  # a CSTRING is an array of DT_BYTE
            off, size = self.ser10(s, f10)
            if not size:
                return
            data = self.base + off
            if t10 == 'DT_TAGMAP':
                at = out.alloc(size)
                out.b[at:at + size] = self.src[data:data + size]
                self.log.append('tag map copied as is (%d bytes)' % size)
                out.put(d + f['ser'], '<2I', at, size)
                return
            e10, e = elem_size(f10['aux'], T10), elem_size(f['aux'], T277)
            n = size // e10
            at = out.alloc(n * e)
            for i in range(n):
                self.convert(f10['aux'], data + i * e10, f['aux'], out, at + i * e)
            out.put(d + f['ser'], '<2I', at, n * e)
            return
        self.convert(t10, s, t, out, d)

# Navmesh: 1.0 stored every square as (float z, uint32 flags); 2.7.7 stores them as
# 16-bit heights between flMinZ and flMaxZ, 16-bit flags, and a 4-bit-per-square table of
# the "special" bits (blocked, level area index) for the cheap lookup.
def rule_navmeshdef(cv, t10, s, t277, out, d):
    sx, sy, typ, num = cv.rd('<4i', s)
    off, size = cv.rd('<2I', s + 0x14)
    squares = [cv.rd('<fI', cv.base + off + i * 8) for i in range(size // 8)]
    out.put(d, '<4i', sx, sy, typ, num)
    out.put(d + 0x10, '<4i', 0, 0, 0, 0)  # no trimmed edges
    zs = [z for z, _ in squares if z < 3e38]
    lo, hi = (min(zs), max(zs)) if zs else (0.0, 0.0)
    out.put(d + 0x20, '<2f', lo, hi)
    out.put(d + 0x28, '<fI', 3.4028234663852886e38, 0)  # tEdge: unused without edges
    if squares:
        rng = hi - lo
        comp = bytearray()
        special = bytearray((len(squares) + 1) // 2)
        lost = 0
        for i, (z, fl) in enumerate(squares):
            q = 0 if rng <= 0 or z >= 3e38 else int(round((z - lo) / rng * 65535))
            if fl > 0xffff:
                lost += 1
            comp += struct.pack('<HH', max(0, min(65535, q)), fl & 0xffff)
            sp = (fl & 1) | (((fl >> 3) & 3) << 1)
            special[i >> 1] |= sp << ((i & 1) * 4)
        at = out.alloc(len(comp)); out.b[at:at + len(comp)] = comp
        out.put(d + 0x40, '<2I', at, len(comp))
        at = out.alloc(len(special)); out.b[at:at + len(special)] = special
        out.put(d + 0x50, '<2I', at, len(special))
        cv.log.append('navmesh %dx%d, %d squares, z %.1f..%.1f%s' % (sx, sy, len(squares), lo, hi,
                      ', %d squares lost high flag bits' % lost if lost else ''))


# Collision: 1.0 kept a different tree (Domino's older node format) and an edge list; 2.7.7
# keeps vertices relative to the mesh centre as float4s, each triangle's "wings" (the far
# vertex of the neighbour across each edge, -1 if none) and a pre-order box tree whose
# boxes are int16s scaled by m_unquantize (child 1 follows its parent, child 2 is
# parent + index; a leaf holds up to 15 consecutive triangles). Both are rebuilt here from
# the 1.0 vertices and triangles (dmTreeMesh.cpp).
LEAF_TRIANGLES = 4

def build_collision_mesh(verts, tris):
    """verts: [(x,y,z)], tris: [(i1,i2,i3,material)] -> dict of 2.7.7 arrays and fields."""
    import math
    lo = [min(v[k] for v in verts) for k in range(3)]
    hi = [max(v[k] for v in verts) for k in range(3)]
    center = [(lo[k] + hi[k]) / 2 for k in range(3)]
    extents = [max((hi[k] - lo[k]) / 2, 1e-3) * 1.001 for k in range(3)]
    unq = [extents[k] / 32767 for k in range(3)]
    local = [tuple(v[k] - center[k] for k in range(3)) for v in verts]
    # Wings: the third vertex of the triangle on the other side of each edge.
    edges = {}
    for t, (a, b, c, _) in enumerate(tris):
        for e0, e1, opp in ((a, b, c), (b, c, a), (c, a, b)):
            edges.setdefault((e0, e1), []).append(opp)
    def wing(e0, e1):
        o = edges.get((e1, e0))
        return o[0] if o else -1
    # The tree, built by splitting the longest axis at the median of triangle centres.
    def tbox(t):
        a, b, c, _ = tris[t]
        ps = (local[a], local[b], local[c])
        return [min(p[k] for p in ps) for k in range(3)], [max(p[k] for p in ps) for k in range(3)]
    boxes = [tbox(t) for t in range(len(tris))]
    cent = [[(b[0][k] + b[1][k]) / 2 for k in range(3)] for b in boxes]
    order, nodes = [], []
    height = [0]
    def q(v, k, up):
        x = (math.ceil if up else math.floor)(v / unq[k])
        return max(-32767, min(32767, x))
    def build(ts, depth):
        height[0] = max(height[0], depth + 1)
        blo = [min(boxes[t][0][k] for t in ts) for k in range(3)]
        bhi = [max(boxes[t][1][k] for t in ts) for k in range(3)]
        me = len(nodes)
        nodes.append(None)
        box = [q(blo[k], k, False) for k in range(3)] + [q(bhi[k], k, True) for k in range(3)]
        if len(ts) <= LEAF_TRIANGLES:
            nodes[me] = box + [(len(order) << 6) | (len(ts) << 2)]
            order.extend(ts)
            return
        clo = [min(cent[t][k] for t in ts) for k in range(3)]
        chi = [max(cent[t][k] for t in ts) for k in range(3)]
        axis = max(range(3), key=lambda k: chi[k] - clo[k])
        ts = sorted(ts, key=lambda t: cent[t][axis])
        mid = len(ts) // 2
        build(ts[:mid], depth + 1)
        child2 = len(nodes)
        build(ts[mid:], depth + 1)
        nodes[me] = box + [((child2 - me) << 6) | axis]
    build(list(range(len(tris))), 0)
    tri_out = []
    for t in order:
        a, b, c, mat = tris[t]
        tri_out.append((a, b, c, wing(a, b), wing(b, c), wing(c, a), mat, 0xffff))
    return dict(center=center, extents=extents, unq=unq, local=local, tris=tri_out,
                nodes=nodes, height=height[0])

def rule_physmesh(cv, t10, s, t277, out, d):
    flags, count = cv.rd('<iI', s + 0xc)
    machine = cv.rd('<I', s + 0x28)[0]
    f10 = [f for f in T10['PhysMesh']['fields'] if f['type'] == 'DT_VARIABLEARRAY'][0]
    off, size = cv.ser10(s + f10['offset'], f10)
    meshes = []
    for m in range(size // 0x88):
        ms = cv.base + off + m * 0x88
        vf = {f['offset']: f for f in T10['CollisionMesh']['fields']}
        vo, vs = cv.ser10(ms + 0x30, vf[0x30])
        to, ts = cv.ser10(ms + 0x38, vf[0x38])
        c10 = cv.rd('<3f', ms)  # 1.0 m_center: its vertices are relative to it too
        verts = [tuple(a + b for a, b in zip(cv.rd('<3f', cv.base + vo + i * 12), c10)) for i in range(vs // 12)]
        tris = [cv.rd('<3i', cv.base + to + i * 0x1c) + (cv.rd('<H', cv.base + to + i * 0x1c + 0x18)[0],) for i in range(ts // 0x1c)]
        meshes.append(build_collision_mesh(verts, tris))
    out.put(d + 0xc, '<i', flags)
    out.put(d + 0x28, '<I', machine)
    mat = out.alloc(0x70 * len(meshes))
    out.put(d + 0x10, '<i', len(meshes))
    out.put(d + 0x14, '<2I', mat, 0x70 * len(meshes))
    def alloc16(n):
        while len(out.b) % 16:
            out.b.append(0)
        return out.alloc(n)
    for m, mesh in enumerate(meshes):
        md = mat + m * 0x70
        nb = b''.join(struct.pack('<6hI', *n) for n in mesh['nodes'])
        vb = b''.join(struct.pack('<4f', *v, 0.0) for v in mesh['local'])
        tb = b''.join(struct.pack('<6i2H', *t) for t in mesh['tris'])
        for data, ser in ((nb, 0x60), (vb, 0x50), (tb, 0x58)):
            at = alloc16(len(data))
            out.b[at:at + len(data)] = data
            out.put(md + ser, '<2I', at, len(data))
        out.put(md + 0x18, '<3f', *mesh['center'])
        out.put(md + 0x24, '<3f', *mesh['extents'])
        out.put(md + 0x30, '<3f', *mesh['unq'])
        out.put(md + 0x3c, '<4i', len(mesh['nodes']), len(mesh['local']), len(mesh['tris']), mesh['height'])
        cv.log.append('collision mesh %d: %d vertices, %d triangles, %d nodes, height %d, %d open edges'
                      % (m, len(mesh['local']), len(mesh['tris']), len(mesh['nodes']), mesh['height'],
                         sum(1 for t in mesh['tris'] for w in t[3:6] if w == -1)))

RULES = {'NavMeshDef': rule_navmeshdef, 'PhysMesh': rule_physmesh}

# Per-struct fix-ups after the fields are copied: (converter, 1.0 offset, out, 2.7.7 offset).
def after_subobject(cv, s, out, d):
    # 1.0 had no packed vertices: leave the pack bits clear so 2.7.7 uploads ptVertList
    # as is (VBFORMAT_2TEX_L), and mark the parts the octree holds (2.7.7 sets bit 12).
    fl = struct.unpack_from('<I', out.b, d)[0] & ~((1 << 15) | (1 << 16))
    if cv.octree_subobjects is not None and cv.subobject_index in cv.octree_subobjects:
        fl |= 1 << 12
    out.put(d, '<I', fl)
    cv.subobject_index += 1
AFTER_STRUCT = {'SubObject': after_subobject}

# Fields 2.7.7 added, with the value Blizzard's 2.7.7 files carry.
def after_scene(out, d):
    if struct.unpack_from('<f', out.b, d + 0x208)[0] == 0:
        out.put(d + 0x208, '<f', 1.0)  # flXPDensityMultiplier
# Arrays 2.7.7 keeps outside the block, in GPU-visible memory (sReadIndicesData and
# sReadVerticesData read each subobject's share in order).
BLOBS = {('SubObject', 0x38): 'idx', ('SubObject', 0x10): 'vtx'}

def after_appearance(out, d, cv=None):
    # 2.7.7 reads an appearance as one block that ends where the separate index and vertex
    # blobs begin (AppearanceGroupDef::GetContiguousReadSize). 1.0 keeps everything in the
    # block, so both blobs are empty and start at its end.
    idx, vtx = (bytes(cv.blobs['idx']), bytes(cv.blobs['vtx'])) if cv else (b'', b'')
    at = len(out.b)
    out.b.extend(idx)
    out.put(d + 0x210, '<2I', at, len(idx))  # serAppearanceSubObjectsIndices
    at2 = len(out.b)
    out.b.extend(vtx)
    out.put(d + 0x218, '<2I', at2, len(vtx))  # serAppearanceSubObjectsVertices
    if cv:
        cv.log.append('index blob %d bytes, vertex blob %d bytes' % (len(idx), len(vtx)))
# Markers naming a file 2.7.7 does not have (cv.has) are dropped, and the special-marker
# index list is renumbered to match.
def after_markerset(out, d, cv=None):
    out.put(d + 0xa8, '<I', struct.unpack_from('<I', out.b, d + 0xa8)[0] | 2)  # bounds already checked
    if not cv or not cv.has:
        return
    off, size = struct.unpack_from('<2I', out.b, d + 0xc)
    n = size // 0x58
    keep, remap = [], {}
    for i in range(n):
        m = off + i * 0x58
        g, sno = struct.unpack_from('<2i', out.b, m + 0x28)
        if sno not in (-1, 0) and not cv.has(g, sno):
            if cv:
                cv.log.append('dropped marker %#x: %d:%d is not in 2.7.7' % (struct.unpack_from('<I', out.b, m)[0], g, sno))
            continue
        remap[i] = len(keep)
        keep.append(bytes(out.b[m:m + 0x58]))
    for j, mk in enumerate(keep):
        out.b[off + j * 0x58:off + (j + 1) * 0x58] = mk
    out.put(d + 0xc, '<2I', off if keep else 0, len(keep) * 0x58)
    so, ss = struct.unpack_from('<2I', out.b, d + 0xb4)
    idx = [remap.get(w) for w in struct.unpack_from('<%dH' % (ss // 2), out.b, so)] if ss else []
    idx = [w for w in idx if w is not None]
    struct.pack_into('<%dH' % len(idx), out.b, so, *idx)
    out.put(d + 0xb4, '<2I', so if idx else 0, len(idx) * 2)
    out.put(d + 0xb0, '<i', len(idx))
def after_weather(out, d):
    # Fields 2.7.7 added to each WeatherStateParams (at 0x0e8 and 0x134): a scale after the
    # second colour, and two distances at the end. Weather files both versions ship have
    # the scale 1 and the distances equal to the first two (the fog's start and end).
    for p in (d + 0xe8, d + 0x134):
        if struct.unpack_from('<f', out.b, p + 0x24)[0] == 0:
            struct.pack_into('<f', out.b, p + 0x24, 1.0)
        if struct.unpack_from('<2f', out.b, p + 0x44) == (0.0, 0.0):
            out.b[p + 0x44:p + 0x4c] = out.b[p + 0x8:p + 0x10]
    for p in (d + 0xbc, d + 0x1a8):  # a fourth SNO, and an int, both -1 in Blizzard's files
        if struct.unpack_from('<i', out.b, p)[0] == 0:
            out.put(p, '<i', -1)

AFTER = {'Scene': after_scene, 'Appearance': after_appearance, 'MarkerSet': after_markerset,
         'Weather': after_weather}

# Before converting: facts the per-struct fix-ups need from the whole 1.0 file.
def before_appearance(cv):
    # The visual octree's primitives are (subobject index, triangle) pairs.
    s = T10['Structure']; o = T10['Octree']
    st_off = next(f['offset'] for f in T10['Appearance']['fields'] if f['type'] == 'Structure')
    oct_off = 0x10 + st_off + next(f['offset'] for f in s['fields'] if f['type'] == 'Octree')
    prim = [f for f in o['fields'] if f['type'] == 'DT_VARIABLEARRAY' and f['aux'] == 'OctreePrimitive'][0]
    off, size = cv.ser10(oct_off + prim['offset'], prim)
    cv.octree_subobjects = {struct.unpack_from('<i', cv.src, cv.base + off + i * 8)[0] for i in range(size // 8)}
    cv.log.append('octree holds %d of the subobjects' % len(cv.octree_subobjects))
BEFORE = {'Appearance': before_appearance}

def convert_file(root, src, header, on_sno=None, has=None, t277=None, sno=None):
    """1.0 file bytes -> (2.7.7 file bytes, log lines). header: a 2.7.7 file's first 0x10
    bytes for the same group. sno: the file's 2.7.7 number, if not its 1.0 one."""
    t277 = t277 or root
    cv = Converter(src, on_sno, has)
    if root in BEFORE:
        BEFORE[root](cv)
    out = Out(T277[t277]['size'])
    out.b[0:0xc] = src[0x10:0x1c]  # the definition header: SNO id and two zeros
    if sno is not None:
        out.put(0, '<i', sno)
    cv.convert(root, 0x10, t277, out, 0)
    if t277 in AFTER:
        if t277 in ('Appearance', 'MarkerSet'):
            AFTER[t277](out, 0, cv)
        else:
            AFTER[t277](out, 0)
    return header[:0x10] + bytes(out.b), cv.log

def main():
    load(sys.argv[1])
    root, src_path, out_path, sample_path = sys.argv[2:6]
    src = open(src_path, 'rb').read()
    data, log = convert_file(root, src, open(sample_path, 'rb').read())
    open(out_path, 'wb').write(data)
    print('%s: %d bytes -> %d bytes' % (os.path.basename(out_path), len(src), len(data)))
    for line in log:
        print('  ' + line)

if __name__ == '__main__':
    main()
