# Converts a 1.0 PC texture (Textures group) to 2.7.7's layout.
#
# 1.0 keeps each mip level as plain rows of DXT blocks, one SerializeData per face and level
# (serTex[6][10]). 2.7.7 (TextureDefinition.h) keeps one: serTex[0][0] holds an NVN texture
# package that GFXNX64NVN::LoadAllMipLevels copies straight into GPU memory, so its levels are
# in the Switch GPU's block-linear layout. The package is a 16-byte archive header and three
# blocks, each behind a 0x24-byte block header: the texture header (nvnTool texpkg
# NVNHWTextureHeader: size, alignment, dimensions, NVN format, level count and offsets), the
# texel data (0x200-aligned) and an end block. The tiling below is the inverse of
# src/gfx/metal/nvn_metal.mm's Deswizzle, level sizes as its LevelSize.
#
# The format numbers are the same in both versions (9, 10 DXT1, 11 DXT3, 12 DXT5); 2.7.7's
# own files pair them with these NVN formats.
import struct

from convert import string_hash_i

NVN_FORMAT = {9: 0x42, 10: 0x42, 11: 0x43, 12: 0x44}
BLOCK_BYTES = {9: 8, 10: 8, 11: 16, 12: 16}
DATA_ALIGN = 0x200


def _align(v, a):
    return (v + a - 1) // a * a


def _block_height(rows):
    bh = 16
    while bh > 1 and _align(rows, 8) <= 8 * bh // 2:
        bh //= 2
    return bh


def _tile_offset(x, y):
    return ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)


def swizzle(plain, row_bytes, rows):
    """Plain rows -> one level in block-linear layout (LevelSize bytes)."""
    bh = _block_height(rows)
    tiles_across = _align(row_bytes, 64) // 64
    block_bytes = 512 * bh
    out = bytearray(_align(row_bytes, 64) * _align(rows, 8 * bh))
    for y in range(rows):
        tile_row = y // 8
        row_base = (tile_row // bh) * tiles_across * block_bytes + (tile_row % bh) * 512
        src = y * row_bytes
        for x in range(0, row_bytes, 16):
            n = min(16, row_bytes - x)
            at = row_base + (x // 64) * block_bytes + _tile_offset(x, y)
            out[at:at + n] = plain[src + x:src + x + n]
    return bytes(out)


def _block_header(size, offset, kind, index):
    return struct.pack('<4sIQQIII', b'HBvN', 0x24, size, offset, kind, index, 0)


def package(fmt, width, height, levels):
    """levels: plain mip data from the largest. Returns the NVN texture package."""
    bb = BLOCK_BYTES[fmt]
    data, offsets = bytearray(), []
    for i, plain in enumerate(levels):
        w, h = max(width >> i, 1), max(height >> i, 1)
        row_bytes, rows = (w + 3) // 4 * bb, (h + 3) // 4
        offsets.append(len(data))
        data += swizzle(plain, row_bytes, rows)
    header = struct.pack('<QIIIIIIII17I', len(data), DATA_ALIGN, width, height, 1, 1, NVN_FORMAT[fmt],
                         len(levels), len(data), *(offsets + [0] * (17 - len(offsets))))
    header += struct.pack('<Iii', 4, 0x10007, 0)  # layout and sparse, as 2.7.7's own textures have
    assert len(header) == 0x78
    out = bytearray(struct.pack('<4sIII', b'DFvN', 0x10, 1, 1))
    out += _block_header(0x78, 0x24, 2, 0) + header
    at = len(out)
    start = _align(at + 0x24, DATA_ALIGN)
    out += _block_header(len(data), start - at, 3, 1)
    out += b'\0' * (start - len(out)) + data
    out += _block_header(0x18, 0x24, 5, 2) + struct.pack('<6I', 0, 0, 1, 1, 0, 0)
    return bytes(out)


def convert_texture(src, header, sno):
    """1.0 texture file bytes -> (2.7.7 file bytes, log lines), or (None, reason)."""
    B = 0x10
    fmt, width, height, faces = struct.unpack_from('<4I', src, B + 0x10)
    mip_min, mip_max = struct.unpack_from('<2I', src, B + 0x20)
    if fmt not in NVN_FORMAT:
        return None, ['format %d is not converted yet' % fmt]
    if faces != 1 or mip_min != 0:
        return None, ['%d faces, first level %d: not converted yet' % (faces, mip_min)]
    levels = []
    for i in range(mip_max + 1):
        w, h = max(width >> i, 1), max(height >> i, 1)
        if w < 4 or h < 4:
            break  # 2.7.7 stops at 4x4 for block formats (sComputeTextureParams)
        off, size = struct.unpack_from('<2I', src, B + 0x28 + i * 8)
        need = (w + 3) // 4 * ((h + 3) // 4) * BLOCK_BYTES[fmt]
        if size < need:
            return None, ['level %d holds %d bytes, %d needed' % (i, size, need)]
        levels.append(src[B + off:B + off + need])
    # The definition (0x278 bytes), then its image ids, frames and the package.
    d = bytearray(0x278)
    struct.pack_into('<i', d, 0x0, sno)                      # RequiredDefinitionHeader.snoHandle
    struct.pack_into('<8I', d, 0x10, fmt, width, height, 1, 1, 0, 0, len(levels) - 1)
    struct.pack_into('<IIIIII', d, 0x258, 0, 1, struct.unpack_from('<I', src, B + 0x35c)[0], 0, 0, 1)
    struct.pack_into('<I', d, 0x270, 1)                      # dwVolumeYSlices
    struct.pack_into('<2i', d, 0x228, *struct.unpack_from('<2i', src, B + 0x218))  # hotspot
    tail = bytearray()

    def put(data, align):
        while (0x278 + len(tail)) % align:
            tail.append(0)
        at = 0x278 + len(tail)
        tail.extend(data)
        return at

    # 1.0 serImageFileIDs: 516-byte entries (two source paths and an int); 2.7.7 keeps the int.
    off, size = struct.unpack_from('<2I', src, B + 0x354)
    ids = b''.join(src[B + off + i * 516 + 512:B + off + i * 516 + 516] for i in range(size // 516))
    if ids:
        struct.pack_into('<2I', d, 0x250, put(ids, 4), len(ids))
    off, size = struct.unpack_from('<2I', src, B + 0x20c)    # 1.0 serFrame: name[64], u0, v0, u1, v1
    frames = b''
    for i in range(size // 0x50):
        f = B + off + i * 0x50
        name = src[f:f + 64].split(b'\0')[0].decode('latin1')
        frames += struct.pack('<I4f', string_hash_i(name) if name else 0, *struct.unpack_from('<4f', src, f + 0x40))
    if frames:
        struct.pack_into('<I2I', d, 0x210, len(frames) // 0x14, put(frames, 8), len(frames))
    pkg = package(fmt, width, height, levels)
    struct.pack_into('<2I', d, 0x30, put(pkg, 1), len(pkg))  # serTex[0][0]; the block ends here
    log = ['%dx%d format %d, %d levels, package %d bytes' % (width, height, fmt, len(levels), len(pkg))]
    return header[:0x10] + bytes(d) + bytes(tail), log
