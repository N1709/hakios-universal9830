#!/usr/bin/env python3
"""relink_bkdm1.py IN OUT MAP.json -- make a dylib cut out of a dyld shared cache (DyldExtractor's output) loadable on
ANOTHER cache of the same iOS build: here the iPhone 8's libBKDM1 (Touch ID) on the iPhone X's 20F66 cache.

What ties an extracted dylib to its old cache, and what this does about it (A, 2026-11-20):
  - segments not on 16 KB pages (the cache packs them): every segment starts at its 16 KB page, padded in front;
    addresses stay as they were, so no code moves.
  - GOT slots (__got, named by the indirect symbol table): bound by name to the library in the symbol's ordinal.
  - the cache builder's pooled GOT (loads straight from shared "_ptr.X" slots): a GOT of our own (__RELINK,__got),
    bound by name, and every ADRP+LDR to a pooled slot pointed at it.
  - isa pointers of constant objects (@"", @[], @{}, @1 -> ___CFConstantStringClassReference, NSConstant*): bound.
  - DyldExtractor's __EXTRA_OBJC (copies of other images' classes): each reference bound to the real class or
    metaclass (_OBJC_CLASS_$_X / _OBJC_METACLASS_$_X); the segment is dropped.
  - every other pointer into the image: a rebase.
MAP.json: {"0x<old address>": "<symbol>"} for the pooled GOT slots and the constant-object classes (ipsw dyld a2s).
Binds outside the GOT use a flat lookup (the classes and data live in libSystem / CoreFoundation / Foundation).
"""
import collections, json, os, struct, sys
import lief
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_INS_ADRP, ARM64_OP_MEM

PAGE = 0x4000
FLAT = -2   # BIND_SPECIAL_DYLIB_FLAT_LOOKUP
inp, outp, mapp = sys.argv[1:4]
raw = open(inp, 'rb').read()
b = lief.parse(inp)
names = {int(k, 16): v for k, v in json.load(open(mapp)).items()}

segs = [s for s in b.segments if s.name not in ('__PAGEZERO', '__LINKEDIT')]
# the raw section headers (flags, alignment, reserved fields as the file has them)
rawsect = {}
_ncmds, _off = struct.unpack_from('<I', raw, 16)[0], 32
for _ in range(_ncmds):
    _cmd, _size = struct.unpack_from('<II', raw, _off)
    if _cmd == 0x19:
        _nsect = struct.unpack_from('<I', raw, _off + 64)[0]
        for j in range(_nsect):
            so = _off + 72 + 80 * j
            sn, gn = (x.rstrip(b'\0').decode() for x in struct.unpack_from('<16s16s', raw, so))
            addr, size, foff, align, reloff, nreloc, flags, r1, r2, r3 = struct.unpack_from('<QQIIIIIIII', raw, so + 32)
            rawsect[(gn, sn)] = (addr, size, align, flags, r1, r2)
    _off += _size
extra = next((s for s in segs if s.name == '__EXTRA_OBJC'), None)
keep = [s for s in segs if s is not extra]
mem = {}
for s in segs:
    buf = bytearray(s.virtual_size)
    buf[:s.file_size] = raw[s.file_offset:s.file_offset + s.file_size]
    mem[s.name] = (s.virtual_address, buf)
# DyldExtractor's __EXTRA_OBJC also holds the method names and types our own method lists point at: it stays, as the
# tail of __DATA_DIRTY (they share a 16 KB page); only its copies of other images' classes are replaced by binds
if extra is not None:
    db, dbuf = mem['__DATA_DIRTY']
    eb, ebuf = mem.pop('__EXTRA_OBJC')
    merged = bytearray(eb + len(ebuf) - db)
    merged[:len(dbuf)] = dbuf
    merged[eb - db:] = ebuf
    mem['__DATA_DIRTY'] = (db, merged)
DATA_DIRTY_END = mem['__DATA_DIRTY'][0] + len(mem['__DATA_DIRTY'][1])

def where(a):
    for n, (base, buf) in mem.items():
        if base <= a < base + len(buf):
            return n, base, buf
    return None, None, None

def rd(a, n=8):
    _, base, buf = where(a)
    return None if buf is None else int.from_bytes(buf[a - base:a - base + n], 'little')

def wr(a, v, n=8):
    _, base, buf = where(a)
    buf[a - base:a - base + n] = v.to_bytes(n, 'little')

def in_keep(v):
    return any(base <= v < base + len(buf) for base, buf in mem.values())

def in_extra(v):
    return extra is not None and extra.virtual_address <= v < extra.virtual_address + extra.virtual_size

def cstr(a):
    n, base, buf = where(a)
    if buf is None:
        return None
    o = a - base
    return bytes(buf[o:buf.index(0, o)]).decode()

# __objc_imageinfo: not "OptimizedByDyld" (1<<3) any more -- that tells libobjc the image is in the shared cache's
# pre-built tables, and it wrote its "loaded" flag into the read-only cache (SIGBUS in map_images, 2026-11-20)
ii = b.get_section('__objc_imageinfo')
if ii is not None:
    _v, _f = struct.unpack('<II', bytes(mem['__DATA_CONST'][1][ii.virtual_address - mem['__DATA_CONST'][0]:][:8]))
    wr(ii.virtual_address, 0, 4)
    wr(ii.virtual_address + 4, _f & ~((1 << 3) | (1 << 7)), 4)

# method lists: the cache marks them "fixed up" (entsizeAndFlags & 3 == 3: selectors uniqued, list sorted by SEL
# address). In a loose image that makes libobjc skip registering the names and binary-search an order that was the
# iPhone 8 cache's: -[BiometricKitXPCServerMesa init] resolved to the superclass, the sensor was never opened
# (2026-11-20). Clear the bits: libobjc uniques the selector refs and searches the (small, read-only) lists linearly.
def unfix_ml(ml):
    if ml and in_keep(ml):
        f = rd(ml, 4)
        if f is not None and f & 3:
            wr(ml, f & ~3, 4)
            return 1
    return 0
nml = 0
_cl = b.get_section('__objc_classlist')
for k in range((_cl.size // 8) if _cl is not None else 0):
    c = rd(_cl.virtual_address + 8 * k)
    for cls in (c, rd(c) if c else 0):
        if not cls or not in_keep(cls):
            continue
        ro = (rd(cls + 32) or 0) & ~7
        if ro and in_keep(ro):
            nml += unfix_ml(rd(ro + 32))
_cat = b.get_section('__objc_catlist')
for k in range((_cat.size // 8) if _cat is not None else 0):
    cat = rd(_cat.virtual_address + 8 * k)
    if cat and in_keep(cat):
        nml += unfix_ml(rd(cat + 16)) + unfix_ml(rd(cat + 24))
print(f'relink: {nml} method lists un-marked "fixed up"')

binds = {}     # slot -> (ordinal, name, weak)
rebases = set()

# 1. the image's own GOT, named by the indirect symbol table
ind = list(b.dynamic_symbol_command.indirect_symbols)
weak_ordinals = set()
for i, cmd in enumerate([c for c in b.commands if isinstance(c, lief.MachO.DylibCommand) and c.command != lief.MachO.LoadCommand.TYPE.ID_DYLIB]):
    if cmd.command == lief.MachO.LoadCommand.TYPE.LOAD_WEAK_DYLIB:
        weak_ordinals.add(i + 1)
for sec in b.sections:
    if sec.name not in ('__got', '__la_symbol_ptr', '__auth_got'):
        continue
    for k in range(sec.size // 8):
        slot = sec.virtual_address + 8 * k
        sym = ind[sec.reserved1 + k]
        nm = sym.name
        if not nm:          # INDIRECT_SYMBOL_LOCAL: a pointer into the image
            if in_keep(rd(slot) or 0):
                rebases.add(slot)
            continue
        ordl = (sym.description >> 8) & 0xff
        binds[slot] = (ordl, nm, ordl in weak_ordinals)
        wr(slot, 0)

# 2. class and metaclass copies in __EXTRA_OBJC -> names
def extra_class(v):
    data = rd(v + 32)
    ro = (data or 0) & ~7
    if not ro:
        return None
    flags = rd(ro, 4)
    try:
        nm = cstr(rd(ro + 24) or 0)
    except (ValueError, UnicodeDecodeError):
        return None
    if not nm or not all(c.isalnum() or c in '_$.' for c in nm):
        return None
    return ('_OBJC_METACLASS_$_' if flags & 1 else '_OBJC_CLASS_$_') + nm

# class references live only here; a pointer into __EXTRA_OBJC anywhere else is a method name, type or list (rebase)
CLASS_SECTIONS = [(x.virtual_address, x.virtual_address + x.size) for x in b.sections
                  if x.name in ('__objc_classrefs', '__objc_superrefs', '__objc_data')]
def class_slot(a):
    return any(lo <= a < hi for lo, hi in CLASS_SECTIONS)

# class and metaclass method caches: 0 in the cache's copy (libobjc's shared tables); a loose image binds them to
# _objc_empty_cache, or objc_msgSend reads a null cache (crash in BKDMInitialize's first message, 2026-11-20)
ncache = 0
_cl = b.get_section('__objc_classlist')
if _cl is not None:
    for k in range(_cl.size // 8):
        c = rd(_cl.virtual_address + 8 * k)
        for a in (c, rd(c) if c else 0):
            if a and in_keep(a) and (rd(a + 16) or 0) == 0 and (a + 16) not in binds:
                binds[a + 16] = (FLAT, '__objc_empty_cache', False)
                ncache += 1
print(f'relink: {ncache} class caches bound to _objc_empty_cache')

unknown = {}
for segname in [n for n in mem if n != '__TEXT']:
    base, buf = mem[segname]
    for off in range(0, len(buf) - 7, 8):
        slot = base + off
        if slot in binds or slot in rebases:
            continue
        v = int.from_bytes(buf[off:off + 8], 'little')
        if v == 0:
            continue
        if in_extra(v) and class_slot(slot) and (nm := extra_class(v)) is not None:
            binds[slot] = (FLAT, nm, False)
            wr(slot, 0)
        elif in_keep(v):
            rebases.add(slot)
        elif v in names:
            binds[slot] = (FLAT, names[v], False)
            wr(slot, 0)
        elif 0x180000000 <= v < 0x300000000 and v & 7 == 0:
            unknown[slot] = v

if os.environ.get('RELINK_UNKNOWN'):
    json.dump({hex(k): hex(v) for k, v in unknown.items()}, open(os.environ['RELINK_UNKNOWN'], 'w'), indent=1)

# 3. pooled GOT loads -> a GOT of our own
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
text = b.get_section('__text')
tb, tbuf = mem['__TEXT']
# pooled slots: outside the image, or inside a segment's range but past its sections (the iPhone 8 cache put part
# of its GOT pool right after libBKDM1's __DATA_CONST sections: ___stack_chk_guard, __os_log_default, ...)
SECTS = [(x.virtual_address, x.virtual_address + x.size) for x in b.sections]
def in_section(a):
    return any(lo <= a < hi for lo, hi in SECTS)
pooled = sorted({v for v in names if not in_section(v) and names[v].startswith('_ptr.')})
last_end = max(base + len(buf) for base, buf in mem.values())
relink_base = (last_end + PAGE - 1) & ~(PAGE - 1)
newslot = {v: relink_base + 8 * i for i, v in enumerate(pooled)}
mem['__RELINK'] = (relink_base, bytearray(max(PAGE, 8 * len(pooled))))
for v, slot in newslot.items():
    binds[slot] = (FLAT, names[v][len('_ptr.'):], False)
# Decoded by bits, not capstone: capstone's disasm() stops at the first word it cannot decode (data in __text), and
# only 583 of ~1,100 pooled loads had been repointed (2026-11-20). Conservative: any other instruction forgets its
# Rd/Rt register, a branch forgets all.
regs, fixed = {}, 0
for sx in [x for x in b.sections if x.name == '__text']:
    for at in range(sx.virtual_address, sx.virtual_address + sx.size, 4):
        ins = rd(at, 4)
        if ins & 0x9f000000 == 0x90000000:                      # ADRP Xd, page
            imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20):
                imm -= 1 << 21
            regs[ins & 31] = ((at & ~0xfff) + (imm << 12), at)
            continue
        if ins & 0xffc00000 == 0xf9400000 and ((ins >> 5) & 31) in regs:   # LDR Xt, [Xn, #imm12*8]
            n = (ins >> 5) & 31
            page, adrp_at = regs[n]
            t = page + (((ins >> 10) & 0xfff) << 3)
            if t in newslot:
                ns = newslot[t]
                delta = ((ns & ~0xfff) - (adrp_at & ~0xfff)) >> 12
                ai = rd(adrp_at, 4)
                wr(adrp_at, (ai & 0x9f00001f) | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5), 4)
                wr(at, (ins & ~(0xfff << 10)) | (((ns & 0xfff) >> 3) << 10), 4)
                regs[n] = (ns & ~0xfff, adrp_at)
                fixed += 1
        if (ins & 0x7c000000) == 0x14000000 or (ins & 0xfe000000) == 0xd6000000 or (ins & 0x7e000000) == 0x34000000 \
                or (ins & 0x7e000000) == 0x36000000 or (ins & 0xff000010) == 0x54000000:
            regs.clear()                                        # B/BL, BR/BLR/RET, CBZ/CBNZ, TBZ/TBNZ, B.cond
        else:
            regs.pop(ins & 31, None)

# 4. stubs: the cache builder pointed them at the cache's own pointer area (outside this image; read as 0 -> a jump to
# address 0 in BKDMInitialize, 2026-11-20). Each stub k is named by the indirect symbol table: point its ADRP+LDR at
# this image's __got slot for that symbol, or at a new slot in __RELINK.
nrelink = len(pooled)
got_of = {nm: slot for slot, (o, nm, w) in binds.items() if any(
    x.name == '__got' and x.virtual_address <= slot < x.virtual_address + x.size for x in b.sections)}
stubs_fixed = 0
for sec in b.sections:
    if sec.name != '__stubs':
        continue
    es = sec.reserved2 or 12
    for k in range(sec.size // es):
        at = sec.virtual_address + es * k
        sym = ind[sec.reserved1 + k]
        if not sym.name:
            continue
        slot = got_of.get(sym.name)
        if slot is None:
            slot = relink_base + 8 * nrelink
            nrelink += 1
            binds[slot] = ((sym.description >> 8) & 0xff, sym.name, ((sym.description >> 8) & 0xff) in weak_ordinals)
            got_of[sym.name] = slot
        delta = ((slot & ~0xfff) - (at & ~0xfff)) >> 12
        ins = rd(at, 4)
        assert ins & 0x9f000000 == 0x90000000, f'stub {at:#x} is not ADRP'
        wr(at, (ins & 0x9f00001f) | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5), 4)
        ins = rd(at + 4, 4)
        assert ins & 0xffc00000 == 0xf9400000, f'stub {at + 4:#x} is not LDR'
        wr(at + 4, (ins & ~(0xfff << 10)) | (((slot & 0xfff) >> 3) << 10), 4)
        stubs_fixed += 1
# 5. __objc_stubs ("objc_msgSend$sel"): ADRP x1/LDR selref (inside), then ADRP x16/LDR x16 from the cache's own
# objc_msgSend pointer (outside): point that at our _objc_msgSend slot.
def slot_for(nm, ordl=FLAT):
    global nrelink
    if nm not in got_of:
        got_of[nm] = relink_base + 8 * nrelink
        binds[got_of[nm]] = (ordl, nm, False)
        nrelink += 1
    return got_of[nm]
osf = 0
os_ = b.get_section('__objc_stubs')
if os_ is not None:
    ms = slot_for('_objc_msgSend')
    for at in range(os_.virtual_address, os_.virtual_address + os_.size - 15, 4):
        i0, i1 = rd(at, 4), rd(at + 4, 4)
        if i0 & 0x9f00001f == 0x90000010 and i1 & 0xffc003ff == 0xf9400210:     # adrp x16 ; ldr x16, [x16, #]
            delta = ((ms & ~0xfff) - (at & ~0xfff)) >> 12
            wr(at, (i0 & 0x9f00001f) | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5), 4)
            wr(at + 4, (i1 & ~(0xfff << 10)) | (((ms & 0xfff) >> 3) << 10), 4)
            osf += 1
assert 8 * nrelink <= len(mem['__RELINK'][1])

# 6. the classes' relative ("small") method lists -> classic lists in __RELINK. A small list's names are offsets to
# selector refs that libobjc expects in __objc_selrefs (uniqued by _read_images); DyldExtractor's refs live in
# __EXTRA_OBJC, so they stayed raw strings: no method was found by its selector where the cache's SEL differed
# (-[BiometricKitXPCServerMesa init] -> the superclass, so the sensor was never opened, 2026-11-20). libobjc uniques
# and sorts classic lists itself (they must be writable: __RELINK is).
rb, rbuf = mem['__RELINK']
cur = (8 * nrelink + 63) & ~63
holders = []
_cl = b.get_section('__objc_classlist')
for k in range((_cl.size // 8) if _cl is not None else 0):
    c = rd(_cl.virtual_address + 8 * k)
    for cls in (c, rd(c) if c else 0):
        ro = (rd(cls + 32) or 0) & ~7 if cls and in_keep(cls) else 0
        if ro and in_keep(ro):
            holders.append(ro + 32)
_cat = b.get_section('__objc_catlist')
for k in range((_cat.size // 8) if _cat is not None else 0):
    cat = rd(_cat.virtual_address + 8 * k)
    if cat and in_keep(cat):
        holders += [cat + 16, cat + 24]
nconv = nmeth = 0
for h in holders:
    ml = rd(h)
    if not ml or not in_keep(ml) or not (rd(ml, 4) & 0x80000000):
        continue
    cnt = rd(ml + 4, 4)
    ents = []
    for j in range(cnt):
        e = ml + 8 + 12 * j
        n, t, i = struct.unpack('<iii', struct.pack('<III', rd(e, 4), rd(e + 4, 4), rd(e + 8, 4)))
        ents.append((rd(e + n), e + 4 + t, e + 8 + i))      # name: the string the ref points at
    need = 8 + 24 * cnt
    if cur + need > len(rbuf):
        rbuf.extend(bytearray(((cur + need - len(rbuf)) + PAGE - 1) & ~(PAGE - 1)))
    nl = rb + cur
    wr(nl, 24, 4)                                           # entsize 24, flags 0: not uniqued, not sorted
    wr(nl + 4, cnt, 4)
    for j, (nm, ty, imp) in enumerate(ents):
        for q, v in enumerate((nm, ty, imp)):
            wr(nl + 8 + 24 * j + 8 * q, v)
            rebases.add(nl + 8 + 24 * j + 8 * q)
    wr(h, nl)
    rebases.add(h)
    cur += (need + 15) & ~15
    nconv += 1
    nmeth += cnt
print(f'relink: {nconv} method lists ({nmeth} methods) rewritten as classic lists in __RELINK')

# what still loads from outside the image (for the log; should be 0)
def image_has(a):
    return any(base <= a < base + len(buf) for base, buf in mem.values())
outside = collections.Counter()
for secname in ('__text', '__stubs', '__objc_stubs'):
    sx = b.get_section(secname)
    if sx is None:
        continue
    r = {}
    for at in range(sx.virtual_address, sx.virtual_address + sx.size, 4):
        ins = rd(at, 4)
        if ins & 0x9f000000 == 0x90000000:
            imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20): imm -= 1 << 21
            r[ins & 31] = (at & ~0xfff) + (imm << 12)
        elif ins & 0xffc00000 == 0xf9400000 and ((ins >> 5) & 31) in r:
            t = r[(ins >> 5) & 31] + (((ins >> 10) & 0xfff) << 3)
            if not image_has(t):
                outside[(secname, t)] += 1
print(f'relink: {osf} objc stubs repointed; loads from outside the image left: {sum(outside.values())}',
      [(n, hex(t), c) for (n, t), c in outside.most_common(6)])

print(f'relink: {len(binds)} binds, {len(rebases)} rebases, {fixed} pooled loads repointed, {stubs_fixed} stubs repointed, {len(unknown)} unknown pointers')
for slot, v in sorted(unknown.items())[:20]:
    print(f'  unknown pointer at {slot:#x} -> {v:#x}')

# 4. layout: every segment on its 16 KB page, the same addresses
order = [s.name for s in keep] + ['__RELINK']   # (__EXTRA_OBJC lives inside __DATA_DIRTY)
lay = []    # (name, vmaddr, pad, vmsize, fileoff, filesize, prot, sections)
cur = 0
for n in order:
    base, buf = mem[n]
    vm = base & ~(PAGE - 1)
    pad = base - vm
    size = (pad + len(buf) + PAGE - 1) & ~(PAGE - 1)
    if n == '__TEXT':
        assert pad == 0 and base == vm
    # file offset = address - image base, as dyld assumes when it reads sections from the unmapped file
    # (MachOAnalyzer::hasSwiftOrObjC crashed reading __objc_imageinfo at its VM offset, 2026-11-20). The gaps between
    # the cache's scattered segments become zeros in the file (~128 MB, compresses to nothing).
    fo = vm - (mem['__TEXT'][0] & ~(PAGE - 1))
    lay.append([n, vm, pad, size, fo, size])
    cur = fo + size
for a, c in zip(lay, lay[1:]):
    assert a[1] + a[3] <= c[1], f'{a[0]} overlaps {c[0]}'

# 5. opcodes
def uleb(v):
    o = bytearray()
    while True:
        x = v & 0x7f
        v >>= 7
        o.append(x | (0x80 if v else 0))
        if not v:
            return o

segidx = {l[0]: k for k, l in enumerate(lay)}
def seg_off(a):
    for k, l in enumerate(lay):
        if l[1] <= a < l[1] + l[3]:
            return k, a - l[1]
    raise ValueError(hex(a))

rb = bytearray([0x11])   # REBASE_OPCODE_SET_TYPE_IMM | POINTER
for a in sorted(rebases):
    k, o = seg_off(a)
    rb += bytes([0x20 | k]) + uleb(o) + bytes([0x51])   # SET_SEGMENT_AND_OFFSET_ULEB, DO_REBASE_IMM_TIMES 1
rb.append(0)
bd = bytearray()
for a in sorted(binds):
    ordl, nm, weak = binds[a]
    k, o = seg_off(a)
    if ordl == FLAT:
        bd.append(0x30 | (FLAT & 0xf))                  # SET_DYLIB_SPECIAL_IMM
    elif ordl < 16:
        bd.append(0x10 | ordl)                          # SET_DYLIB_ORDINAL_IMM
    else:
        bd += bytes([0x20]) + uleb(ordl)
    bd += bytes([0x40 | (1 if weak else 0)]) + nm.encode() + b'\0'   # SET_SYMBOL_TRAILING_FLAGS_IMM (weak import)
    bd += bytes([0x51, 0x70 | k]) + uleb(o) + bytes([0x90])          # TYPE pointer, SEGMENT_AND_OFFSET, DO_BIND
bd.append(0)

def al8(x):
    return x + bytes(-len(x) % 8)

def blob(off, size):
    return raw[off:off + size]
lc_exp = next(c for c in b.commands if c.command.name == 'DYLD_EXPORTS_TRIE')
lc_fs = next(c for c in b.commands if c.command.name == 'FUNCTION_STARTS')
st = b.symbol_command
dst = b.dynamic_symbol_command
le_parts = [al8(bytes(rb)), al8(bytes(bd)), al8(blob(lc_exp.data_offset, lc_exp.data_size)),
            al8(blob(lc_fs.data_offset, lc_fs.data_size)),
            al8(blob(st.symbol_offset, st.numberof_symbols * 16)),
            al8(blob(dst.indirect_symbol_offset, dst.nb_indirect_symbols * 4)),
            blob(st.strings_offset, st.strings_size)]
le_vm = (lay[-1][1] + lay[-1][3])
le_off = le_vm - lay[0][1]
offs, o = [], le_off
for p in le_parts:
    offs.append(o)
    o += len(p)
le_size = o - le_off

# 6. load commands
segcmds = b''
sect_of = {}
for s in keep:
    sect_of[s.name] = [x for x in b.sections if x.segment_name == s.name]
sect_of['__RELINK'] = []
prot = {s.name: (s.max_protection, s.init_protection) for s in keep}
prot['__RELINK'] = (3, 3)
for n, vm, pad, size, fo, fsz in lay:
    secs = sect_of[n]
    body = b''
    for x in secs:
        addr, size_, align, flags, r1, r2 = rawsect[(n, x.name)]
        zerofill = (flags & 0xff) in (1, 0xc, 0x12)
        body += struct.pack('<16s16sQQIIIIIIII', x.name.encode(), n.encode(), addr, size_,
                            0 if zerofill else fo + (addr - vm), align, 0, 0, flags, r1, r2, 0)
    nsect = len(secs)
    if n == '__RELINK':
        body += struct.pack('<16s16sQQIIIIIIII', b'__got', b'__RELINK', vm, 8 * nrelink, fo, 3, 0, 0, 6, 0, 0, 0)
        nsect = 1
    elif n == '__DATA_DIRTY' and extra is not None:
        # the merged __EXTRA_OBJC as a section of its own: every fixup must lie in a section (dyld_info stopped there)
        ea, es = extra.virtual_address, extra.virtual_size
        body += struct.pack('<16s16sQQIIIIIIII', b'__extra_objc', b'__DATA_DIRTY', ea, es, fo + (ea - vm), 3, 0, 0, 0, 0, 0, 0)
        nsect += 1
    mp, ip = prot[n]
    segcmds += struct.pack('<II16sQQQQiiII', 0x19, 72 + len(body), n.encode(), vm, size, fo, size, mp, ip, nsect, 0) + body
segcmds += struct.pack('<II16sQQQQiiII', 0x19, 72, b'__LINKEDIT', le_vm, (le_size + PAGE - 1) & ~(PAGE - 1), le_off, le_size, 1, 1, 0, 0)

keepcmds = b''
for c in b.commands:
    if c.command.name in ('ID_DYLIB', 'UUID', 'BUILD_VERSION', 'SOURCE_VERSION', 'LOAD_DYLIB', 'LOAD_WEAK_DYLIB'):
        keepcmds += bytes(c.data)
lc_dyld = struct.pack('<IIIIIIIIIIII', 0x80000022, 48, offs[0], len(le_parts[0]), offs[1], len(le_parts[1]), 0, 0, 0, 0, offs[2], len(le_parts[2]))
lc_fs2 = struct.pack('<IIII', 0x26, 16, offs[3], len(le_parts[3]))
lc_sym = struct.pack('<IIIIII', 0x2, 24, offs[4], st.numberof_symbols, offs[6], len(le_parts[6]))
lc_dys = struct.pack('<IIIIIIIIIIIIIIIIIIII', 0xb, 80, dst.idx_local_symbol, dst.nb_local_symbols, dst.idx_external_define_symbol,
                     dst.nb_external_define_symbols, dst.idx_undefined_symbol, dst.nb_undefined_symbols, 0, 0, 0, 0, 0, 0,
                     offs[5], dst.nb_indirect_symbols, 0, 0, 0, 0)
cmds = segcmds + keepcmds + lc_dyld + lc_fs2 + lc_sym + lc_dys
ncmds = (len(lay) + 1) + sum(1 for c in b.commands if c.command.name in ('ID_DYLIB', 'UUID', 'BUILD_VERSION', 'SOURCE_VERSION', 'LOAD_DYLIB', 'LOAD_WEAK_DYLIB')) + 4
hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 6, ncmds, len(cmds), 0x00100085 & ~0x80000000 | 0x4 | 0x80 | 0x100000, 0)
first_sect = min(x.virtual_address for x in sect_of['__TEXT']) - lay[0][1]
assert len(hdr) + len(cmds) <= first_sect, f'load commands {len(hdr) + len(cmds)} > {first_sect}'

out = bytearray(le_off + le_size)
for n, vm, pad, size, fo, fsz in lay:
    base, buf = mem[n]
    out[fo + pad:fo + pad + len(buf)] = buf
out[0:len(hdr) + len(cmds)] = hdr + cmds
for p, o in zip(le_parts, offs):
    out[o:o + len(p)] = p
open(outp, 'wb').write(out)
print(f'relink: wrote {outp} ({len(out)} bytes), __RELINK at {relink_base:#x} ({nrelink} slots)')
