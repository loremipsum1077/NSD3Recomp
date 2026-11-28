# Music and sound banks, 1.0 -> 2.7.7. Both versions keep a bank's samples as files beside
# it, SoundBank\<bank>\0000.ogg and so on (SoundBankGetSampleFullFilename), and 2.7.7's music
# samples are the same plain Ogg Vorbis files 1.0 has, so a bank converts by rewriting its
# sample table and copying the Ogg files. The layouts were found by comparing files both
# versions ship (Music_A1_Atmos_int, Music_Act1_Maghda).
import struct

B = 0x10
VORBIS = 3  # SOUNDCOMPRESSION_VORBIS


def string_hash_i(s):
    """The game's case-insensitive string hash, which 2.7.7 names samples by."""
    h = 0
    for c in s.lower().encode('latin1'):
        h = (h * 33 + c) & 0xffffffff
    return h


def bank_samples(src):
    """1.0 bank bytes -> [(sample name, file name, compression, format, decompressed size,
    file size)]. A 1.0 SoundSample (0x2a8 bytes): name[129], file name[256] at 0x81, source
    path at 0x181, then ints at 0x284: ?, ?, compression, quality, decompressed size,
    format, and serSamples (its size is the file's) at 0x29c."""
    off, size = struct.unpack_from('<2I', src, B + 0xc)
    out = []
    for i in range(size // 0x2a8):
        s = B + off + i * 0x2a8
        name = src[s:s + 129].split(b'\0')[0].decode('latin1')
        filename = src[s + 129:s + 385].split(b'\0')[0].decode('latin1')
        _, _, comp, quality, dsize, fmt = struct.unpack_from('<6I', src, s + 0x284)
        fsize = struct.unpack_from('<I', src, s + 0x2a0)[0]
        out.append((name, filename, comp, quality, fmt, dsize, fsize))
    return out


def convert_bank(src, header, sno):
    """1.0 SoundBank bytes -> (2.7.7 bytes, sample file names, log), or (None, None, log).
    Only Ogg Vorbis banks convert: 2.7.7 reads other kinds from FMOD sample banks."""
    samples = bank_samples(src)
    if not samples or any(c != VORBIS for _, _, c, _, _, _, _ in samples):
        return None, None, ['only Ogg Vorbis banks are converted']
    d = bytearray(0x28)
    struct.pack_into('<i', d, 0x0, sno)                    # RequiredDefinitionHeader.snoHandle
    table = b''
    for name, _, comp, quality, fmt, dsize, fsize in samples:
        packed = (comp & 0x3ff) << 2 | (quality & 0x3ff) << 12 | (fmt & 0x3ff) << 22
        table += struct.pack('<4I', string_hash_i(name), packed, dsize, fsize)
    struct.pack_into('<2I', d, 0xc, 0x28, len(table))     # serSoundSamples, after the block
    struct.pack_into('<i', d, 0x20, len(samples))          # nSoundSampleCount
    files = [f for _, f, _, _, _, _, _ in samples]
    log = ['%d sample%s: %s' % (len(samples), '' if len(samples) == 1 else 's', ', '.join(files))]
    return header[:0x10] + bytes(d) + table, files, log


def convert_music(src, header, sno, on_sno):
    """1.0 Music bytes -> (2.7.7 bytes, log). The definition is 1.0's with 8-byte pointers;
    a 1.0 PlaylistEntry (0xbc bytes) is its bank, the sample's name and thirteen numbers
    (volume, start and end positions, loops, fades) that 2.7.7 keeps after the sample's
    hash, adding fade accelerations, a sample index and a validated bank, all 0 in
    2.7.7's own music."""
    d = bytearray(0x50)
    struct.pack_into('<i', d, 0x0, sno)
    d[0xc:0x1c] = src[B + 0xc:B + 0x1c]                      # flags, volume, times
    off, size = struct.unpack_from('<2I', src, B + 0x1c)
    count = struct.unpack_from('<i', src, B + 0x24)[0]
    randoms = bytearray(src[B + 0x2c:B + 0x4c])             # two RandomMusicSoundParams
    for k in range(2):
        snd = struct.unpack_from('<i', randoms, k * 0x10)[0]
        struct.pack_into('<i', randoms, k * 0x10, on_sno(snd))
    d[0x30:0x50] = randoms
    entries = b''
    for i in range(size // 0xbc):
        e = B + off + i * 0xbc
        bank = on_sno(struct.unpack_from('<i', src, e)[0])
        name = src[e + 4:e + 136].split(b'\0')[0].decode('latin1')
        entries += struct.pack('<iI', bank, string_hash_i(name)) + src[e + 136:e + 188] + bytes(16)
    struct.pack_into('<2I', d, 0x1c, 0x50, len(entries))
    struct.pack_into('<i', d, 0x24, count)
    log = ['%d playlist entr%s' % (len(entries) // 0x4c, 'y' if len(entries) == 0x4c else 'ies')]
    return header[:0x10] + bytes(d) + entries, log
