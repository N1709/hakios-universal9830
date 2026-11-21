#!/usr/bin/env python3
"""Structural checks for this board's existing image format, not boot proof."""
from pathlib import Path
import struct
import zlib


def regular(path):
    p = Path(path)
    if p.is_symlink() or not p.is_file():
        raise ValueError(f'expected regular file: {p}')
    return p


def android_head(path):
    p = regular(path)
    with p.open('rb') as f:
        h = f.read(2048)
        if len(h) < 1660 or h[:8] != b'ANDROID!':
            raise ValueError('not an Android boot image')
        u32 = lambda at: struct.unpack_from('<I', h, at)[0]
        page, version = u32(36), u32(40)
        if page != 2048 or version != 2:
            raise ValueError('x1slte requires Android header v2 and 2048-byte pages')
        align = lambda n: (n + page - 1) // page * page
        dtbo_size = u32(1632)
        dtb_at = page + sum(align(u32(at)) for at in (8, 16, 24)) + align(dtbo_size)
        dtb_size = u32(1648)
        end = dtb_at + align(dtb_size)
        if dtb_size < 4 or end > p.stat().st_size or end >= 0x1000000:
            raise ValueError('DT table missing, truncated or overlapping the log slot')
        f.seek(dtb_at)
        if f.read(4) != bytes.fromhex('d7b7ab1e'):
            raise ValueError('Samsung DT table required; raw FDT is unsafe for BOOT')
        return end


def apfs(path, maximum, offset=0):
    p = regular(path)
    with p.open('rb') as f:
        f.seek(offset)
        h = f.read(48)
    if len(h) != 48 or h[32:36] != b'NXSB':
        raise ValueError('APFS container header missing')
    block, count = struct.unpack_from('<IQ', h, 36)
    if block != 4096 or count == 0 or count * block > maximum:
        raise ValueError('APFS geometry exceeds its device window')
    if offset + count * block > p.stat().st_size:
        raise ValueError('truncated APFS image')
    return count * block


def recovery(path, device):
    p = regular(path)
    if p.stat().st_size != device['partitions']['recovery']:
        raise ValueError('recovery must match the measured partition size exactly')
    android_head(p)
    with p.open('rb') as f:
        f.seek(-64, 2)
        footer = f.read(64)
        if footer[:4] != b'AVBf':
            raise ValueError('AVB footer missing (signature verification still required)')
        original, vb_at, vb_size = struct.unpack_from('>QQQ', footer, 12)
        if not (0 < original <= vb_at < 0x1000000 and 0 < vb_size and vb_at + vb_size <= 0x1000000):
            raise ValueError('AVB metadata overlaps runtime/log slots')
        f.seek(device['layout']['kernel_offset'])
        if f.read(4) != bytes.fromhex('cffaedfe'):
            raise ValueError('ARM64 Mach-O kernel missing from recovery slot')
    apfs(p, device['partitions']['recovery'] - device['layout']['root_offset'] - 64,
         device['layout']['root_offset'])


def hfs_kit(path, userdata_bytes):
    data = regular(path).read_bytes()
    if len(data) < 32 or data[:8] != b'HFSKIT01':
        raise ValueError('HFS format kit missing')
    block, entries, size, crc, _ = struct.unpack_from('<IIQII', data, 8)
    if block != 4096 or size != userdata_bytes or zlib.crc32(data[32:]) & 0xffffffff != crc:
        raise ValueError('HFS kit capacity or CRC mismatch')
    if len(data) < 32 + entries * 16:
        raise ValueError('truncated HFS kit index')
    expected_data = 32 + entries * 16
    end = 0
    for i in range(entries):
        start, count, kind = struct.unpack_from('<QII', data, 32 + i * 16)
        if kind not in (0, 1) or count == 0 or start < end or (start + count) * block > size:
            raise ValueError('invalid or overlapping HFS kit extent')
        end = start + count
        expected_data += count * block if kind else 0
    if expected_data != len(data):
        raise ValueError('HFS kit extent data length mismatch')
