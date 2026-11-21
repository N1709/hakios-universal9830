#!/usr/bin/env python3
"""
A format kit (the HFSKIT01 format of tools/hfs-kit.py, read by userland/src/init.c) for an HFSX of ANY size, read
straight from a device or image that macOS's newfs_hfs has just formatted. Unlike hfs-kit.py (made for the S20+'s
whole userdata, 116253523968 bytes, with the catalog's place written in), this follows the volume header: the
allocation file's extents give the bitmap, the bitmap gives the used blocks, and each is read from the device.

    tools/hfs-kit-any.py /dev/rdiskN OUT         (tools/mk-var-hfs.sh runs it on a sparse image it formatted)

Every used block goes in, as data where it is not all zeros and as a run of zeros where it is, so nothing that was
in the window before (Android's free space) shows through the bitmap, the B-trees or the journal.
"""
import struct
import sys
import zlib

BLOCK = 4096


def main():
    dev, out = sys.argv[1], sys.argv[2]
    f = open(dev, 'rb', buffering=0)

    def read_block(b, n=1):
        f.seek(b * BLOCK)
        d = f.read(n * BLOCK)
        assert len(d) == n * BLOCK, 'short read at block %d' % b
        return d

    vh = read_block(0)[1024:1536]
    sig, version = struct.unpack('>2sH', vh[:4])
    bsize, total = struct.unpack('>II', vh[40:48])
    assert sig == b'HX' and version == 5 and bsize == BLOCK, (sig, version, bsize)
    # allocationFile: HFSPlusForkData at 112: logicalSize u64, clumpSize u32, totalBlocks u32, extents[8] (start, count)
    alloc_ext = [struct.unpack('>II', vh[112 + 16 + 8 * i:112 + 24 + 8 * i]) for i in range(8)]
    bitmap = b''.join(read_block(s, c) for s, c in alloc_ext if c)
    assert len(bitmap) * 8 >= total, 'bitmap shorter than the volume'
    used = [b for b in range(total) if (bitmap[b >> 3] >> (7 - (b & 7))) & 1]

    zero = bytes(BLOCK)
    entries, data = [], bytearray()
    for b in used:
        content = read_block(b)
        kind = 0 if content == zero else 1
        if entries and entries[-1][2] == kind and entries[-1][0] + entries[-1][1] == b:
            entries[-1][1] += 1
        else:
            entries.append([b, 1, kind])
        if kind:
            data += content
    body = b''.join(struct.pack('<QII', *e) for e in entries) + bytes(data)
    device = total * BLOCK
    hdr = b'HFSKIT01' + struct.pack('<IIQII', BLOCK, len(entries), device, zlib.crc32(body) & 0xffffffff, 0)
    open(out, 'wb').write(hdr + body)
    zeros = sum(e[1] for e in entries if e[2] == 0)
    print('hfs-kit-any: HFSX of %d blocks (%d MiB): %d used, %d of data (%d KB), %d of zeros (%d MB), %d runs -> %s (%d KB)'
          % (total, device >> 20, len(used), len(data) // BLOCK, len(data) >> 10, zeros, zeros * BLOCK >> 20,
             len(entries), out, (len(hdr) + len(body)) >> 10))


main()
