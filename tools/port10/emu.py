# Runs the 1.0 Mac client's static initialisers (i386 Mach-O) under Unicorn so the
# type descriptor tables they build can be read from memory afterwards.
# Usage: emu.py <binary> <out.pkl>
import pickle, struct, sys
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_EIP

path, out = sys.argv[1], sys.argv[2]
d = open(path, 'rb').read()
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from('<7I', d, 0)
assert magic == 0xfeedface
segs, sects = [], {}
symoff = nsyms = stroff = 0
indoff = nind = 0
o = 28
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<2I', d, o)
    if cmd == 1:  # LC_SEGMENT
        segname = d[o+8:o+24].rstrip(b'\0').decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4I', d, o+24)
        nsects, = struct.unpack_from('<I', d, o+48)
        segs.append((segname, vmaddr, vmsize, fileoff, filesize))
        so = o + 56
        for i in range(nsects):
            sn = d[so:so+16].rstrip(b'\0').decode()
            addr, ssize, soff, align, reloff, nreloc, sflags, r1, r2 = struct.unpack_from('<9I', d, so+32)
            sects[sn + '/' + d[so+16:so+32].rstrip(b'\0').decode()] = (addr, ssize, r1, r2)
            so += 68
    elif cmd == 2:  # LC_SYMTAB
        symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', d, o+8)
    elif cmd == 0xb:  # LC_DYSYMTAB
        vals = struct.unpack_from('<18I', d, o+8)
        indoff, nind = vals[12], vals[13]
    o += size

def symname(i):
    n_strx, = struct.unpack_from('<I', d, symoff + i * 12)
    e = d.index(b'\0', stroff + n_strx)
    return d[stroff + n_strx:e].decode('latin1')
ind = struct.unpack_from('<%dI' % nind, d, indoff)

mu = Uc(UC_ARCH_X86, UC_MODE_32)
PAGE = 0x1000
for name, vmaddr, vmsize, fileoff, filesize in segs:
    if vmsize == 0 or name == '__PAGEZERO':
        continue
    start = vmaddr & ~(PAGE - 1)
    end = (vmaddr + vmsize + PAGE - 1) & ~(PAGE - 1)
    mu.mem_map(start, end - start)
    mu.mem_write(vmaddr, d[fileoff:fileoff + filesize])

# Imported functions: the __symbol_stub entries (6 bytes each).
stub_addr, stub_size, stub_r1, stub_r2 = sects['__symbol_stub/__TEXT']
stubs = {}
for i in range(stub_size // stub_r2):
    s = ind[stub_r1 + i]
    stubs[stub_addr + i * stub_r2] = symname(s) if s < nsyms else '?'
# Imported data pointers (__nl_symbol_ptr): point each at a dummy block, by name.
nl_addr, nl_size, nl_r1, _ = sects['__nl_symbol_ptr/__DATA']
HEAP = 0x30000000
HEAP_SIZE = 0x08000000
mu.mem_map(HEAP, HEAP_SIZE)
heap_top = [HEAP + 0x100000]
def alloc(n):
    a = heap_top[0]
    heap_top[0] += (n + 15) & ~15
    return a
imported_data = {}
for i in range(nl_size // 4):
    s = ind[nl_r1 + i]
    if s & 0x80000000:
        continue  # local: the linker filled it in
    if s < nsyms:
        a = alloc(0x100)
        imported_data[a] = symname(s)
        mu.mem_write(nl_addr + i * 4, struct.pack('<I', a))

STACK = 0x70000000
mu.mem_map(STACK - 0x200000, 0x200000)
RET = 0x7fff0000
mu.mem_map(RET & ~(PAGE - 1), PAGE)
mu.mem_write(RET, b'\xf4')  # hlt

def rd32(a): return struct.unpack('<I', mu.mem_read(a, 4))[0]
def cstr(a, n=256):
    try:
        b = bytes(mu.mem_read(a, n))
    except UcError:
        return None
    return b.split(b'\0')[0].decode('latin1')

counts = {}
raise_stop = [False]
def on_code(uc, addr, size, _):
    name = stubs.get(addr)
    if name is None:
        return
    esp = uc.reg_read(UC_X86_REG_ESP)
    ret = rd32(esp)
    a = [rd32(esp + 4 + 4 * k) for k in range(4)]
    r = 0
    if name in ('__Znwm', '__Znam', '_malloc', '_operator new'):
        r = alloc(max(a[0], 4))
    elif name == '_calloc':
        r = alloc(max(a[0] * a[1], 4))
    elif name in ('_memcpy', '_memmove', '___memcpy_chk', '___memmove_chk'):
        if a[2] and a[2] < 0x1000000:
            uc.mem_write(a[0], bytes(uc.mem_read(a[1], a[2])))
        r = a[0]
    elif name in ('_memset', '___memset_chk'):
        if a[2] and a[2] < 0x1000000:
            uc.mem_write(a[0], bytes([a[1] & 0xff]) * a[2])
        r = a[0]
    elif name == '___cxa_guard_acquire':
        r = 0 if rd32(a[0]) & 0xff else 1
    elif name == '___cxa_guard_release':
        uc.mem_write(a[0], b'\x01')
    elif name == '__Unwind_Resume':
        raise_stop[0] = True
        uc.emu_stop()
        return
    elif name == '_strlen':
        s = cstr(a[0], 4096)
        r = len(s) if s is not None else 0
    elif name in ('_strcpy', '___strcpy_chk'):
        s = cstr(a[1], 4096) or ''
        uc.mem_write(a[0], s.encode('latin1') + b'\0'); r = a[0]
    counts[name] = counts.get(name, 0) + 1
    uc.reg_write(UC_X86_REG_EAX, r)
    uc.reg_write(UC_X86_REG_ESP, esp + 4)
    uc.reg_write(UC_X86_REG_EIP, ret)

mu.hook_add(UC_HOOK_CODE, on_code, begin=stub_addr, end=stub_addr + stub_size)

def on_unmapped(uc, access, addr, size, value, _):
    # Map a page of zeros so stray reads/writes through unknown pointers keep going.
    try:
        uc.mem_map(addr & ~(PAGE - 1), PAGE)
        return True
    except UcError:
        return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

init_addr, init_size, _, _ = sects['__mod_init_func/__DATA']
funcs = struct.unpack('<%dI' % (init_size // 4), bytes(mu.mem_read(init_addr, init_size)))
ok = bad = 0
for f in funcs:
    esp = STACK - 0x1000
    mu.mem_write(esp, struct.pack('<I', RET))
    mu.reg_write(UC_X86_REG_ESP, esp)
    try:
        raise_stop[0] = False
        mu.emu_start(f, RET, count=5_000_000)
        if raise_stop[0]:
            bad += 1
            print('init %#x reached an exception path' % f, file=sys.stderr)
        else:
            ok += 1
    except UcError as e:
        bad += 1
        print('init %#x failed at %#x: %s' % (f, mu.reg_read(UC_X86_REG_EIP), e), file=sys.stderr)
print('initialisers: %d ran, %d failed' % (ok, bad), file=sys.stderr)
print('stub calls:', sorted(counts.items(), key=lambda x: -x[1])[:15], file=sys.stderr)

# Dump the data segment and heap for the descriptor scan.
dump = {}
for name, vmaddr, vmsize, fileoff, filesize in segs:
    if name in ('__DATA', '__TEXT', '__IMPORT', '__OBJC'):
        dump[vmaddr] = bytes(mu.mem_read(vmaddr, vmsize))
dump[HEAP] = bytes(mu.mem_read(HEAP, heap_top[0] - HEAP))
pickle.dump({'mem': dump, 'imported': imported_data}, open(out, 'wb'))
