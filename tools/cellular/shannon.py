#!/usr/bin/env python3
"""Offline Shannon firmware inventory and SIPC5 framing checks. No device I/O.

MIT. Written from the on-disk format and Samsung CPIF protocol facts; no vendor
code or firmware is included. See docs/devices/x1slte/cellular/s20.md for pinned references.
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

ENTRY = struct.Struct('<12s5I')
# This reader deliberately accepts only the S20 image's known firmware sections.
# NV is an external EFS object; OFFSET is metadata, not a firmware payload.
PAYLOADS = frozenset(('BOOT', 'MAIN', 'APM', 'VSS', 'INFO'))
KNOWN = PAYLOADS | {'TOC', 'NV', 'OFFSET'}


def read_toc(stream, file_size, verify_crc=False):
    """Return metadata only. Never extract NV or interpret CP addresses as AP RAM."""
    stream.seek(0)
    table = stream.read(512)
    if len(table) != 512:
        raise ValueError('truncated 512-byte TOC')
    entries = []
    names = set()
    for offset in range(0, 512, ENTRY.size):
        record = table[offset:offset + ENTRY.size]
        if not any(record):
            # The remaining header can contain signatures, not TOC entries.
            break
        raw, start, address, size, crc, entry_id = ENTRY.unpack(record)
        name_bytes, separator, suffix = raw.partition(b'\0')
        if not separator or any(suffix):
            raise ValueError(f'invalid section name at TOC offset {offset:#x}')
        name = name_bytes.decode('ascii', errors='strict')
        if name not in KNOWN or name in names:
            raise ValueError(f'unknown or duplicate TOC section: {name!r}')
        if not entries and name != 'TOC':
            raise ValueError('missing TOC magic')
        names.add(name)
        if not size:
            raise ValueError(f'empty {name} section')
        if address + size > 1 << 32:
            raise ValueError(f'{name} CP address overflows 32 bits')
        item = dict(name=name, file_offset=start, cp_address=address,
                    size=size, crc32=crc, entry_id=entry_id)
        if name in PAYLOADS:
            if start < 512 or start > file_size or size > file_size - start:
                raise ValueError(f'{name} payload outside firmware file')
            if name == 'BOOT' and size > 128 * 1024:
                raise ValueError('BOOT exceeds Samsung SPI loader limit (128 KiB)')
            if verify_crc:
                stream.seek(start)
                remaining, computed = size, 0
                while remaining:
                    chunk = stream.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise ValueError(f'truncated {name} payload')
                    computed = zlib.crc32(chunk, computed)
                    remaining -= len(chunk)
                if computed != crc:
                    raise ValueError(f'{name} CRC mismatch')
                item['crc_verified'] = True
        elif name == 'NV':
            item['source'] = 'external EFS; never read or extracted by this tool'
        elif name == 'OFFSET':
            item['source'] = 'metadata; not a payload'
        entries.append(item)
    if not {'TOC', 'BOOT', 'MAIN'} <= names:
        raise ValueError('TOC, BOOT and MAIN are required')
    if entries[0]['file_offset'] != 0 or not 512 <= entries[0]['size'] <= file_size:
        raise ValueError('invalid TOC span')
    spans = sorted((e['file_offset'], e['file_offset'] + e['size'], e['name'])
                   for e in entries if e['name'] in PAYLOADS)
    if spans and spans[0][0] < entries[0]['size']:
        raise ValueError('payload overlaps TOC/signature span')
    for previous, current in zip(spans, spans[1:]):
        if previous[1] > current[0]:
            raise ValueError(f'overlapping payloads: {previous[2]} / {current[2]}')
    return entries


def sipc5_header(data, *, alignment=8, max_frame=1024 * 1024):
    """Validate a complete frame, returning metadata without its payload.

    S20's ARM64 Samsung sipc5_calc_padding_size uses BITS_PER_LONG / 8 = 8.
    Alignment is explicit because other implementations use 4. This handles
    link framing only; it neither decodes Samsung IPC nor reassembles fragments.
    """
    if alignment not in (4, 8):
        raise ValueError('alignment must be 4 or 8')
    if len(data) < 4 or data[0] & 0xf8 != 0xf8:
        raise ValueError('short or invalid SIPC5 header')
    flags = data[0] & 7
    if flags & 3 == 1:
        raise ValueError('control flag without extended-field flag')
    header_size = 5 if flags & 3 == 3 else 6 if flags & 2 else 4
    if len(data) < header_size:
        raise ValueError('truncated SIPC5 extended header')
    frame_size = int.from_bytes(data[2:6] if flags & 3 == 2 else data[2:4], 'little')
    if frame_size < header_size or frame_size > max_frame:
        raise ValueError('invalid or excessive SIPC5 frame length')
    total = (frame_size + alignment - 1) & -alignment if flags & 4 else frame_size
    if len(data) < total:
        raise ValueError('truncated SIPC5 frame or padding')
    return dict(channel=data[1], flags=flags, header_size=header_size,
                frame_size=frame_size, total_size=total,
                payload_size=frame_size - header_size,
                control=data[4] if flags & 3 == 3 else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('radio', type=Path, help='local backup of RADIO (opened read-only)')
    parser.add_argument('--verify-crc', action='store_true', help='check firmware section CRCs')
    args = parser.parse_args()
    try:
        with args.radio.open('rb') as stream:
            entries = read_toc(stream, args.radio.stat().st_size, args.verify_crc)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(dict(scope='offline metadata only; no modem boot or authenticity proof',
                          sections=entries), indent=2))


if __name__ == '__main__':
    main()
