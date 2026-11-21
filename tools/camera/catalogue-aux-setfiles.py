#!/usr/bin/env python3
"""Inventory Samsung 2LA/GW2 set-A tables without copying their payloads.

Labels are stock-source names, not verified iOS capture capabilities.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct

TABLE = runpy.run_path(str(Path(__file__).with_name('import-2ld-setfile.py')))['table']
REGS = {0x034c: 'width', 0x034e: 'height', 0x0340: 'frame_length_lines',
        0x0342: 'line_length_pck', 0x0900: 'readout_register_0900',
        0x0114: 'lane_register_0114', 0x0112: 'format_register_0112'}


def summarize(name, values):
    if not values or len(values) % 3:
        raise ValueError('invalid table triplets: ' + name)
    result = {'name': name, 'records': len(values)//3,
              'words_sha256_be32': hashlib.sha256(struct.pack('>'+str(len(values))+'I', *values)).hexdigest()}
    match = re.search(r'_(\d+)x(\d+)_(\d+)fps$', name)
    if match:
        result['label'] = dict(zip(('width', 'height', 'fps'), map(int, match.groups())))
    page, registers = None, {}
    for i in range(0, len(values), 3):
        address, value, size = values[i:i+3]
        if address == 0xfcfc and size == 2:
            page = value
        elif page == 0x4000 and address in REGS and size == 2:
            registers[REGS[address]] = value
    result['page4000_registers'] = registers
    if match:
        for key in ('width', 'height'):
            if registers.get(key) != result['label'][key]:
                raise ValueError(name + ': mode label disagrees with direct output ' + key)
    return result


def catalogue(path):
    blob = path.read_bytes()
    source = blob.decode()
    names = re.findall(r'const\s+u32\s+(sensor_(?:2la|gw2)_setfile_A_(?:initial_setting|Global(?:_[12])?|Tnp|[0-9]+x[0-9]+_[0-9]+fps))\[\]\s*=\s*\{', source, re.I)
    return {'source': path.name, 'source_sha256': hashlib.sha256(blob).hexdigest(),
            'tables': [summarize(name, TABLE(source, name)) for name in names]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', nargs=2, type=Path)
    args = parser.parse_args()
    print(json.dumps({'schema': 1, 'verified_on_device': False,
        'warning': '2LA revision-gated globals; GW2 TNP/Global and calibration required; no payloads exported',
        'sources': [catalogue(p) for p in args.sources]}, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
