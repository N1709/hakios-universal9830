#!/usr/bin/env python3
"""Extract the ordered EVT1 calibration subsection for offline review only.

Not a complete PHY initializer: resets, common tail, PCS and controller setup
remain separate. Never execute this plan directly or deduplicate its writes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def extract(source):
    source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
    source = re.sub(r'//[^\n]*', '', source)
    marker = '} else if (chip_ver == 1) {'
    if source.count(marker) != 1:
        raise ValueError('expected exactly one EVT1 calibration branch')
    body = source.split(marker, 1)[1].split('}', 1)[0]
    body = re.sub(r'pr_info\("[^"\n]*",\s*__func__\);', '', body)
    pattern = re.compile(r'writel\((0x[\da-fA-F]+),\s*phy_base_regs\s*\+\s*'
                         r'(0x[\da-fA-F]+)(?:\s*\+\s*(0x[\da-fA-F]+))?\);')
    result = []
    for match in pattern.finditer(body):
        value, offset = int(match[1], 16), int(match[2], 16) + (int(match[3], 16) if match[3] else 0)
        if not 0 <= value <= 0xffffffff or offset & 3 or not 0 <= offset < 0x2000:
            raise ValueError('write outside aligned 8-KiB PHY window or invalid value')
        result.append(dict(offset=offset, value=value))
    if pattern.sub('', body).strip() or not result:
        raise ValueError('unrecognized calibration operation; manual source review required')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_bytes()
    operations = extract(source.decode())
    if len(operations) != 110:
        parser.error('expected reviewed 110-operation EVT1 subsection; review source version')
    plan = dict(scope='OFFLINE REVIEW ONLY; partial calibration, not an executable boot sequence',
                chip_ver=1, source_sha256=hashlib.sha256(source).hexdigest(),
                source_license='GPL-2.0-only; Samsung Electronics 2019, pcie-exynos9830-rc-cal.c',
                phy_base=0x13d50000, phy_size=0x2000, operations=operations)
    with args.output.open('x') as stream:
        json.dump(plan, stream, indent=2)
        stream.write('\n')
    print('Extracted110 ordered EVT1 writes for review; no hardware access.')


if __name__ == '__main__':
    main()
