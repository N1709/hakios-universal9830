#!/usr/bin/env python3
"""Inspect Samsung Exynos ISP setfile containers offline; never execute payloads.

Layout: Exynos990 hardware/is-hw-control.{h,c}, parse_setfile_header/info.
Payload interpretation is deliberately left opaque until its DDK schema is known.
No firmware bytes are exported. Output contains metadata and hashes only.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = 0x12345679
DESIGNS = {3: '3AA_ISP', 4: 'DRC', 5: 'SCC', 6: 'ODC', 7: 'VDIS',
           8: 'TDNR', 9: 'MCSC', 10: 'VRA', 11: 'CLAHE'}
# Driver consumes CLAHE before MCSC/VRA, rather than numeric bit order.
DESIGN_ORDER = (3, 4, 5, 6, 7, 8, 11, 9, 10)


def inspect(data):
    def need(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            raise ValueError(f'out-of-bounds region: offset={offset}, size={size}')

    def words(offset, count):
        need(offset, count * 4)
        return struct.unpack_from(f'<{count}I', data, offset)

    magic, = words(0, 1)
    if magic == MAGIC:
        need(0, 28)
        _, bits, version, revision, scenarios, ips, base = struct.unpack_from('<II4s4sIII', data)
        header = 28
        names = [DESIGNS[b] for b in DESIGN_ORDER if bits & (1 << b)]
        if bits & ~sum(1 << b for b in DESIGNS) or len(names) != ips:
            raise ValueError('unsupported design bits / IP count')
        version = version.decode('ascii', errors='replace')
        revision = revision.decode('ascii', errors='replace')
        fmt = 3
    elif magic == MAGIC - 1:
        _, scenarios, ips, base = words(0, 4)
        header, bits, version, revision, fmt = 16, 0, '', '', 2
        names = None  # v2 mapping is platform dependent
    else:
        raise ValueError(f'unknown magic {magic:#x}')
    if not 0 < ips <= 64 or not 0 < scenarios <= 4096:
        raise ValueError('implausible container dimensions')
    if names is None:
        names = [f'IP_{i}' for i in range(ips)]
    indexes = words(header, ips * scenarios)
    counts = words(header + ips * scenarios * 4, ips)
    table = header + ips * scenarios * 4 + ips * 4
    need(table, sum(counts) * 8)
    if base < table + sum(counts) * 8 or base > len(data):
        raise ValueError('payload overlaps tables or begins beyond file')
    groups, entry_at = [], table
    for ip, count in enumerate(counts):
        mapping = indexes[ip * scenarios:(ip + 1) * scenarios]
        if any(i != 0xffffffff and i >= count for i in mapping):
            raise ValueError(f'IP {ip}: scenario references absent entry')
        entries = []
        for i in range(count):
            offset, size = words(entry_at, 2)
            entry_at += 8
            start = base + offset
            need(start, size)
            entries.append(dict(index=i, offset=start, size=size,
                                sha256=hashlib.sha256(data[start:start + size]).hexdigest(),
                                scenarios=[s for s, idx in enumerate(mapping) if idx == i]))
        groups.append(dict(ip=ip, design=names[ip], entries=entries))
    return dict(format=fmt, version=version, revision=revision, designed_bits=hex(bits),
                scenarios=scenarios, payload_base=base, size=len(data),
                sha256=hashlib.sha256(data).hexdigest(), groups=groups)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        result = {p.name: inspect(p.read_bytes()) for p in args.files}
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'setfile: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
