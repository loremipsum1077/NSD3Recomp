# Builds a 2.7.7 world file that places a list of scenes, from a 2.7.7 world used as a
# template: its environment, scripts and settings are kept, and its scene list is replaced.
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


def build(template, sno_world, scenes, level_area=None):
    """template: 2.7.7 world bytes. scenes: [(scene sno, x, y)]. Returns the new world bytes."""
    b = bytearray(template)
    struct.pack_into('<i', b, B + 0x0, sno_world)  # RequiredDefinitionHeader.snoHandle
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
