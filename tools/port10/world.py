# Builds a 2.7.7 world file that places a list of scenes, from a 2.7.7 world used as a
# template: its scripts and settings are kept, its scene list is replaced, and its
# environment (sky colour, weather, music, ambient sound) is the 1.0 world's where 2.7.7
# still has the files that names.
#
# Each scene goes in a SceneChunk copied from the template's first one (so it keeps that
# chunk's level area and scene specification) with the scene's SNO and position, no rotation
# and cell 0,0, as Blizzard's fixed-layout worlds place theirs (a3dun_Azmodan_Arena).
import struct

B = 0x10      # serialize offsets count from the definition, after the file header
CHUNK = 0x100  # sizeof(SceneChunk)


def tile_position(name, size):
    """Where a tile named like Blizzard's tiled scenes goes, or None.

    *_E01_S02 (compass offsets): as 2.7.7's a3dun_Azmodan_Arena lays its tiles out, W01 is
    row 0 and E01, E02 rows 1, 2 along y; S01 is column 0, S02 column 1 and N01 column -1
    along x. *_x06_y04 (grid, 1.0's PvP_Map2): the name's x runs along y and its y along
    x, which is how the tiles' walkable edges meet (95% of border squares, against 18% the
    other way round). A step is one tile (size units)."""
    parts = name.split('_')
    if len(parts) >= 2:
        a, b = parts[-2], parts[-1]
        if a[:1] in 'EW' and b[:1] in 'NS' and a[1:].isdigit() and b[1:].isdigit():
            y = int(a[1:]) if a[0] == 'E' else 1 - int(a[1:])
            x = int(b[1:]) - 1 if b[0] == 'S' else -int(b[1:])
            return x * size, y * size
        if a[:1] == 'x' and b[:1] == 'y' and a[1:].isdigit() and b[1:].isdigit():
            return int(b[1:]) * size, int(a[1:]) * size
    return None


ENV = 0x60  # Worlds.tEnvironment, in both versions
# 1.0 Environment SNO fields -> 2.7.7's (WorldDefinition.h): music, ambient sound, reverb,
# weather, irradiance, irradiance when dead. 1.0's combat music has no 2.7.7 field.
ENV_SNOS = {0x98: 0x28, 0xa0: 0x38, 0xa4: 0x3c, 0xa8: 0x40, 0xac: 0x44, 0xb0: 0x48}
# Fields an arena may leave empty; the others keep the template's file when 1.0 names none.
ENV_OPTIONAL = {0x28, 0x38, 0x3c}
# Files an unfinished 1.0 arena names in place of its own, which leave it unlit in 2.7.7
# (NoWeather has no sun); the template's are kept instead.
PLACEHOLDERS = {'NoWeather'}


def environment(old_world, template, game_ids, port=None):
    """The 1.0 world's environment, as {2.7.7 offset in Worlds: bytes} to write over the
    template's: its sky colour and post-processing, and each file it names that 2.7.7 still
    has in the same group as the template's (or any group when the template names none).
    game_ids: 2.7.7 SNO -> (group, name). port(sno): the 2.7.7 number of a 1.0 file the
    porter converts, or None. Also returns log lines."""
    out, log = {}, []
    e10, e = B + ENV, B + ENV
    out[ENV] = bytes(old_world[e10:e10 + 0x24])  # rgbaSkyColor, tPostFXParams
    for o10, o in ENV_SNOS.items():
        sno = struct.unpack_from('<i', old_world, e10 + o10)[0]
        mine = struct.unpack_from('<i', template, e + o)[0]
        if sno in (0, -1):
            if o in ENV_OPTIONAL:
                out[ENV + o] = struct.pack('<i', -1)
            continue
        want = game_ids.get(mine, (None,))[0]
        have = game_ids.get(sno)
        if have and have[1] in PLACEHOLDERS:
            log.append('environment: %s is a placeholder, kept the template\'s' % have[1])
            continue
        ported = port(sno) if port and not have else None
        if have and (want is None or have[0] == want):
            out[ENV + o] = struct.pack('<i', sno)
            log.append('environment: %s' % have[1])
        elif ported is not None:
            out[ENV + o] = struct.pack('<i', ported)
            log.append('environment: %d, converted' % sno)
        else:
            log.append('environment: %d is not in 2.7.7, kept the template\'s' % sno)
    return out, log


# Entrances. 2.7.7's brawling puts a hero at one of the dueling world's entrances, picked at
# random (PVPGetDuelingEntranceGUID): the markers whose actor is a portal destination, such
# as Start_Location_0, and whose TAG_MARKER_PORTAL_DEST_ENTRANCE_NUMBER is a start location
# 2.7.7's globals.glo names, each number once (DRLG.cpp records only the first). Several
# 1.0 arenas number their starts 0 (the town's start), repeat a number, or leave it out;
# those starts get free PVP_Neutral_SpawnLocation numbers.
MARKER = 0x58               # sizeof(Marker)
ENTRANCE_TAG = 0x80a04      # TAG_MARKER_PORTAL_DEST_ENTRANCE_NUMBER
PVP_ENTRANCES = (131, 134, 135, 136, 137, 138, 404, 405)  # PVP_Team*_SpawnLocation_*
NEUTRAL = tuple(range(288, 320))  # PVP_Neutral_SpawnLocation_00 to _31


def _markers(b):
    off, size = struct.unpack_from('<2I', b, B + 0xc)  # MarkerSetDefinition.serMarkers
    return [B + off + i * MARKER for i in range(size // MARKER)]


def _actor(b, m):
    return struct.unpack_from('<i', b, m + 0x2c)[0]  # Marker.snoname.snoHandle


def _tags(b, m):
    """A marker's tag map entries, [type, key, value bytes]: a count, then 12-byte entries."""
    to, ts = struct.unpack_from('<2I', b, m + 0x30)
    if not ts:
        return []
    n = struct.unpack_from('<I', b, B + to)[0]
    return [list(struct.unpack_from('<2I4s', b, B + to + 4 + k * 12)) for k in range(n)]


def _append(b, data):
    while len(b) % 8:
        b.append(0)
    at = len(b) - B
    b.extend(data)
    return at


def _set_tags(b, m, entries):
    data = struct.pack('<I', len(entries)) + b''.join(struct.pack('<2I4s', *e) for e in entries)
    struct.pack_into('<2I', b, m + 0x30, _append(b, data), len(data))


def number_starts(sets, start):
    """sets: {marker set sno: bytes}, one arena's. start: Start_Location_0's SNO. Returns the
    sets it changed and log lines."""
    bufs = {s: bytearray(d) for s, d in sets.items()}
    used, todo, kept = set(), [], 0
    for s in sorted(bufs):
        b = bufs[s]
        for m in _markers(b):
            if _actor(b, m) != start:
                continue
            num = next((struct.unpack('<I', v)[0] for _, k, v in _tags(b, m) if k == ENTRANCE_TAG), None)
            if num in PVP_ENTRANCES + NEUTRAL and num not in used:
                used.add(num)
                kept += 1
            else:
                todo.append((s, m))
    free = [n for n in NEUTRAL if n not in used]
    changed = set()
    for (s, m), num in zip(todo, free):
        b = bufs[s]
        rest = [e for e in _tags(b, m) if e[1] != ENTRANCE_TAG]
        _set_tags(b, m, [[7, ENTRANCE_TAG, struct.pack('<I', num)]] + rest)
        changed.add(s)
    log = ['entrances: %d starts kept their numbers, %d numbered' % (kept, min(len(todo), len(free)))]
    if len(todo) > len(free):
        log.append('entrances: %d starts left as they were (2.7.7 names 32 neutral ones)' % (len(todo) - len(free)))
    return {s: bytes(bufs[s]) for s in changed}, log


def build(template, sno_world, scenes, level_area=None, env=None):
    """template: 2.7.7 world bytes. scenes: [(scene sno, x, y)]. env: {offset: bytes} to
    write over the template's (environment()). Returns the new world bytes."""
    b = bytearray(template)
    struct.pack_into('<i', b, B + 0x0, sno_world)  # RequiredDefinitionHeader.snoHandle
    for o, data in (env or {}).items():
        b[B + o:B + o + len(data)] = data
    wsd_off, wsd_size = struct.unpack_from('<2I', b, B + 0x10)
    assert wsd_size == 0x90, 'the template should have one WorldServerData'
    wsd = B + wsd_off
    sp_off, sp_size = struct.unpack_from('<2I', b, wsd + 0x10)
    assert sp_size == 0x18, 'the template should have one SceneParams'
    sp = B + sp_off
    ch_off, ch_size = struct.unpack_from('<2I', b, sp + 0x0)
    assert ch_size and ch_size % CHUNK == 0, 'unexpected SceneChunk size'
    proto = bytes(b[B + ch_off:B + ch_off + CHUNK])
    # The new chunk list goes after the file's end; the template's chunks stay unreferenced.
    while len(b) % 16:
        b.append(0)
    at = len(b) - B
    for sno, x, y in scenes:
        c = bytearray(proto)
        struct.pack_into('<2i', c, 0x0, 33, sno)                       # SNOName {Scene, sno}
        struct.pack_into('<4f3f', c, 0x8, 0, 0, 0, 1, x, y, 0)         # PRTransform
        struct.pack_into('<i2i', c, 0x24, 0, 0, 0)                     # SceneSpecification: cell 0,0
        if level_area is not None:
            struct.pack_into('<i', c, 0x24 + 0xc, level_area)
        b.extend(c)
    struct.pack_into('<2I', b, sp + 0x0, at, CHUNK * len(scenes))
    struct.pack_into('<i', b, sp + 0x8, len(scenes))
    return bytes(b)
